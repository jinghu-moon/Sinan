#include "policy/rules.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <set>

namespace tabtail {
namespace {
[[noreturn]] void invalid(const char* message) { throw PolicyError(PolicyErrorCode::invalid_argument, message); }
using Text = std::u32string;
Text unicode(std::wstring_view input) {
    Text out;
    for (size_t i = 0; i < input.size(); ++i) {
        char32_t c = input[i];
        if (!c) invalid("NUL is not allowed in a path");
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i >= input.size() || input[i] < 0xdc00 || input[i] > 0xdfff) invalid("Invalid Unicode surrogate");
            c = 0x10000 + ((c - 0xd800) << 10) + (input[i] - 0xdc00);
        } else if (c >= 0xdc00 && c <= 0xdfff) invalid("Invalid Unicode surrogate");
        out += c;
    }
    return out;
}
bool equal(char32_t a, char32_t b, bool sensitive) {
    if (a == b) return true;
    if (sensitive) return false;
    if (a < 128 && b < 128) {
        if (a >= U'a' && a <= U'z') a -= 32;
        if (b >= U'a' && b <= U'z') b -= 32;
        return a == b;
    }
    auto utf16 = [](char32_t c, wchar_t* out) {
        if (c < 0x10000) { out[0] = static_cast<wchar_t>(c); return 1; }
        c -= 0x10000; out[0] = static_cast<wchar_t>(0xd800 + (c >> 10)); out[1] = static_cast<wchar_t>(0xdc00 + (c & 1023)); return 2;
    };
    wchar_t x[2]{}, y[2]{}; auto nx = utf16(a,x), ny = utf16(b,y);
    return CompareStringOrdinal(x,nx,y,ny,TRUE) == CSTR_EQUAL;
}
bool equal(const Text& a, const Text& b, bool sensitive) {
    if (a.size() != b.size()) return false;
    for (size_t i=0;i<a.size();++i) if (!equal(a[i],b[i],sensitive)) return false;
    return true;
}
struct Path { Text root; std::vector<Text> parts; };
Path path(Text input) {
    if (input.size() > 32768) invalid("Path is too long");
    for (auto& c : input) if (c == U'/') c = U'\\';
    if (input.starts_with(U"\\\\?\\UNC\\") || input.starts_with(U"\\\\?\\unc\\")) input = U"\\\\" + input.substr(8);
    else if (input.starts_with(U"\\\\?\\")) input.erase(0,4);
    Path out; size_t pos = 0;
    if (input.size() >= 3 && ((input[0] >= U'A' && input[0] <= U'Z') || (input[0] >= U'a' && input[0] <= U'z')) && input[1] == U':' && input[2] == U'\\') {
        out.root = input.substr(0,3); pos = 3;
    } else if (input.starts_with(U"\\\\")) {
        auto server = input.find(U'\\',2); auto share = server == Text::npos ? Text::npos : input.find(U'\\',server+1);
        if (server == Text::npos || server == 2 || server+1 >= input.size() || share == server+1) invalid("UNC requires server and share");
        if (share == Text::npos) share = input.size();
        out.root = input.substr(0,share); pos = share;
    } else invalid("An absolute drive or UNC path is required");
    if (out.root.find_first_of(U"*?[]{}") != Text::npos) invalid("Wildcards are not allowed in the path root");
    while (pos < input.size()) {
        if (input[pos] == U'\\') { ++pos; continue; }
        auto end = input.find(U'\\',pos); if (end == Text::npos) end = input.size();
        auto part = input.substr(pos,end-pos); pos = end;
        if (part == U".") continue;
        if (part == U"..") { if (out.parts.empty()) invalid("Path goes above root"); out.parts.pop_back(); }
        else out.parts.push_back(std::move(part));
    }
    return out;
}
void expand(const Text& pattern, std::vector<Text>& out,size_t generation=0) {
    if(generation>256) invalid("Brace alternatives exceed 256");
    size_t begin=Text::npos, end=Text::npos; int nesting=0; bool in_class=false;
    for (size_t i=0;i<pattern.size();++i) {
        if (pattern[i]==U'[' && !in_class) { in_class=true; if (i+1<pattern.size() && pattern[i+1]==U']') ++i; continue; }
        if (pattern[i]==U']' && in_class) { in_class=false; continue; }
        if (in_class) continue;
        if (pattern[i]==U'{') { if (begin==Text::npos) begin=i; if (++nesting > 4) invalid("Brace nesting exceeds 4 levels"); }
        if (pattern[i]==U'}') { if (!nesting) invalid("Unmatched brace"); if (--nesting==0) {end=i;break;} }
    }
    if (begin==Text::npos) { if (out.size() >= 256) invalid("Brace alternatives exceed 256"); out.push_back(pattern); return; }
    if (end==Text::npos) invalid("Unclosed brace");
    auto body=pattern.substr(begin+1,end-begin-1); size_t start=0; nesting=0; in_class=false; bool comma=false;
    for (size_t i=0;i<=body.size();++i) {
        if (i<body.size()) {
            if (body[i]==U'[' && !in_class) {in_class=true; if(i+1<body.size() && body[i+1]==U']') ++i; continue;}
            if (body[i]==U']' && in_class) {in_class=false;continue;}
            if (in_class) continue;
            if (body[i]==U'{') ++nesting;
            if (body[i]==U'}') --nesting;
        }
        if (i==body.size() || (body[i]==U',' && nesting==0)) {
            if (start==i) invalid("Empty brace alternative");
            if (i<body.size()) comma=true;
            expand(pattern.substr(0,begin)+body.substr(start,i-start)+pattern.substr(end+1),out,generation+1); start=i+1;
        }
    }
    if (!comma) invalid("Braces require comma-separated alternatives; sequence expansion is not supported");
}
enum class Kind { literal, star, any, characters };
struct Token { Kind kind; char32_t c=0; std::vector<std::pair<char32_t,char32_t>> ranges; };
struct Segment { bool recursive=false; std::vector<Token> tokens; };
struct Pattern { Path path; std::vector<Segment> segments; std::wstring original; };
Segment segment(const Text& value, bool exact) {
    Segment out;
    if (!exact && value==U"**") {out.recursive=true;return out;}
    if (!exact && value.find(U"**")!=Text::npos) invalid("** must occupy a complete path segment");
    for(size_t i=0;i<value.size();++i) {
        auto c=value[i];
        if (!exact && c==U'*') out.tokens.push_back({Kind::star});
        else if (!exact && c==U'?') out.tokens.push_back({Kind::any});
        else if (!exact && c==U'[') {
            Token token{Kind::characters}; size_t j=i+1;
            if(j==value.size() || value[j]==U'!' || value[j]==U'^') invalid("Negated or empty character classes are not supported");
            if(value[j]==U']') {token.ranges.emplace_back(U']',U']');++j;}
            while(j<value.size() && value[j]!=U']') {
                auto first=value[j++], last=first;
                if(j+1<value.size() && value[j]==U'-' && value[j+1]!=U']') {
                    last=value[j+1];j+=2;
                    if(first>127 || last>127 || first>last) invalid("Character ranges require ascending ASCII endpoints");
                }
                token.ranges.emplace_back(first,last);
            }
            if(j==value.size() || token.ranges.empty()) invalid("Unclosed or empty character class");
            i=j;out.tokens.push_back(std::move(token));
        } else if (!exact && (c==U']' || c==U'{' || c==U'}')) invalid("Unmatched pattern delimiter");
        else out.tokens.push_back({Kind::literal,c});
    }
    return out;
}
void spend(size_t& budget) { if (!budget--) invalid("Rule matching work limit exceeded"); }
bool matches(const Segment& segment, const Text& text, bool sensitive,size_t& budget) {
    std::vector<unsigned char> row(text.size()+1), next(row.size()); row[0]=1;
    for(const auto& token:segment.tokens) {
        std::fill(next.begin(),next.end(),static_cast<unsigned char>(0));
        for(size_t j=0;j<=text.size();++j) {
            spend(budget);
            if(token.kind==Kind::star) next[j]=row[j] || (j && next[j-1]);
            else if(j && row[j-1]) {
                bool ok=token.kind==Kind::any || (token.kind==Kind::literal && equal(token.c,text[j-1],sensitive));
                if(token.kind==Kind::characters) for(auto [a,b]:token.ranges) {
                    spend(budget);
                    if(a==b) ok |= equal(a,text[j-1],sensitive);
                    else for(auto c=a;c<=b;++c) {spend(budget);if(equal(c,text[j-1],sensitive)) {ok=true;break;}}
                    if(ok) break;
                }
                next[j]=ok;
            }
        }
        row.swap(next);
    }
    return row.back()!=0;
}
bool matches(const Pattern& pattern,const Path& target,bool sensitive,size_t& budget) {
    if(!equal(pattern.path.root,target.root,sensitive)) return false;
    std::vector<unsigned char> row(target.parts.size()+1),next(row.size());row[0]=1;
    for(const auto& s:pattern.segments) {
        std::fill(next.begin(),next.end(),static_cast<unsigned char>(0));
        for(size_t j=0;j<=target.parts.size();++j) {
            spend(budget);
            if(s.recursive) next[j]=row[j] || (j && next[j-1]);
            else if(j && row[j-1]) next[j]=matches(s,target.parts[j-1],sensitive,budget);
        }
        row.swap(next);
    }
    return row.back()!=0;
}
}
struct RuleSet::Impl { std::vector<DirectoryRule> rules; std::vector<std::vector<Pattern>> compiled; };
RuleSet::RuleSet(std::vector<DirectoryRule> rules) {
    if(rules.size()>512) invalid("At most 512 directory rules are allowed");
    auto result=std::make_shared<Impl>();std::set<std::string> ids; size_t total=0;
    for(const auto& rule:rules) {
        if(rule.id.empty() || rule.id.size()>80 || !std::all_of(rule.id.begin(),rule.id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';}) || !ids.insert(rule.id).second) invalid("Rule IDs must be unique ASCII letters, digits, - or _ (1..80)");
        if(rule.name.size()>256 || rule.name.find(L'\0')!=std::wstring::npos) invalid("Invalid rule name");
        validate_depth(rule.depth);
        if(rule.patterns.empty() || rule.patterns.size()>64) invalid("Each rule requires 1..64 patterns");
        std::vector<Pattern> compiled;
        for(const auto& source:rule.patterns) {
            auto value=unicode(source);
            if(value.empty() || value.size()>4096) invalid("Patterns require 1..4096 Unicode characters");
            if(!rule.exact && value[0]==U'!') invalid("Leading ! exclusions are not supported");
            std::vector<Text> alternatives;
            if(rule.exact) alternatives.push_back(value);else expand(value,alternatives);
            for(const auto& alternative:alternatives) {
                if(++total>65536) invalid("Compiled rule state limit exceeded");
                Pattern p; p.path=path(alternative);p.original=source;
                total+=p.path.root.size();
                if(total>65536) invalid("Compiled rule state limit exceeded");
                for(const auto& part:p.path.parts) {
                    auto s=segment(part,rule.exact);total+=s.tokens.size()+1;
                    for(const auto& token:s.tokens)total+=token.ranges.size();
                    if(total>65536) invalid("Compiled rule state limit exceeded");
                    p.segments.push_back(std::move(s));
                }
                compiled.push_back(std::move(p));
            }
        }
        result->compiled.push_back(std::move(compiled));
    }
    result->rules=std::move(rules);impl_=std::move(result);
}
const std::vector<DirectoryRule>& RuleSet::definitions() const {return impl_->rules;}
EffectiveDisplay RuleSet::resolve(const DisplayOptions& defaults,const std::optional<DepthOverride>& override,const std::optional<std::wstring>& input) const {
    auto result=resolve_policy(defaults,override);
    if(override || !input || impl_->rules.empty()) return result;
    auto target=path(unicode(*input));size_t budget=2000000;
    for(size_t i=0;i<impl_->rules.size();++i) {
        const auto& r=impl_->rules[i];if(!r.enabled) continue;
        for(const auto& p:impl_->compiled[i]) if(matches(p,target,r.case_sensitive,budget)) {
            result.source=PolicySource::rule;result.display.depth=r.depth;result.native=r.native;
            result.rule_id=r.id;result.matched_pattern=p.original;return result;
        }
    }
    return result;
}
EffectiveDisplay RuntimePolicy::resolve(const std::optional<std::wstring>& path) const {
    return rules ? rules->resolve(defaults,override,path) : resolve_policy(defaults,override);
}
}
