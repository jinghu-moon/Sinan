#include "config/config.hpp"
#include "policy/tab_registry.hpp"
#include "formatter/formatter.hpp"
#include "app/service.hpp"
#include <iostream>

using namespace tabtail;
int main() {
    int checks=0;
    auto expect=[&](bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);};
    auto match=[&](std::wstring pattern,std::wstring value,bool exact=false,bool sensitive=false){
        DirectoryRule r;r.id="r";r.patterns={std::move(pattern)};r.depth=3;r.exact=exact;r.case_sensitive=sensitive;
        return RuleSet({r}).resolve({}, {}, value).source==PolicySource::rule;
    };
    auto rejects=[&](auto run){bool rejected=false;try{run();}catch(const Error&){rejected=true;}catch(const PolicyError&){rejected=true;}expect(rejected,"expected invalid rule");};
    try {
        expect(match(L"D:\\P\\*",L"D:\\P\\src"),"star");
        expect(!match(L"D:\\P\\*",L"D:\\P"),"star requires a segment");
        expect(!match(L"D:\\P\\*",L"D:\\P\\src\\A"),"star cannot cross separator");
        expect(match(L"D:\\P\\**",L"D:\\P"),"recursive zero");
        expect(match(L"D:\\P\\**",L"D:\\P\\A\\B"),"recursive many");
        expect(match(L"D:\\P\\**\\src",L"D:\\P\\src"),"recursive middle zero");
        expect(match(L"D:\\P\\**\\src",L"D:\\P\\A\\B\\src"),"recursive middle many");
        expect(!match(L"D:\\P\\**",L"D:\\P-other\\A"),"directory boundary");
        expect(match(L"D:\\P\\App?",L"D:\\P\\App中"),"question Unicode");
        expect(match(L"D:\\P\\?",L"D:\\P\\😀"),"question surrogate pair");
        expect(!match(L"D:\\P\\?",L"D:\\P\\ab"),"question one scalar");
        expect(match(L"D:\\P\\**\\{src,docs,tests}",L"D:\\P\\A\\docs"),"braces");
        expect(match(L"D:\\P\\{src,{docs,tests}}",L"D:\\P\\tests"),"nested braces");
        expect(match(L"D:\\P\\App[abc]",L"D:\\P\\Appb"),"class");
        expect(match(L"D:\\P\\v[0-9]",L"D:\\P\\v7"),"range");
        expect(!match(L"D:\\P\\v[0-9]",L"D:\\P\\vX"),"range rejects");
        expect(match(L"D:\\P\\[[]x[]]",L"D:\\P\\[x]"),"literal brackets");
        expect(match(L"D:\\P\\[{]x[}]",L"D:\\P\\{x}"),"literal braces");
        expect(match(L"D:\\P\\{x}",L"D:\\P\\{x}",true),"exact special names");
        expect(match(L"D:/P/**/src/",L"d:\\p\\A\\SRC"),"case separator trailing");
        expect(!match(L"D:\\P\\src",L"D:\\P\\SRC",false,true),"case sensitive");
        expect(match(L"D:\\Ä\\*",L"D:\\ä\\X"),"ordinal Unicode case");
        expect(match(L"D:\\P\\**",L"\\\\?\\D:\\P\\A"),"extended drive");
        expect(match(L"\\\\nas\\share\\**",L"\\\\?\\UNC\\nas\\share\\A"),"extended UNC");
        expect(match(L"D:\\P\\A\\..\\src",L"D:\\P\\src"),"dot normalization");
        expect(match(L"D:\\P\\*",L"D:\\P\\.git"),"dot folders");
        expect(match(L"D:\\P\\Hi!",L"D:\\P\\Hi!"),"literal bang");
        for(auto bad:{L"!D:\\P\\**",L"D:\\P\\[!abc]",L"D:\\P\\[^abc]",L"D:\\P\\x**y",L"D:\\P\\***",L"D:\\P\\{x,}",L"D:\\P\\[z-a]",L"D:\\P\\[a",L"relative\\*",L"D:\\..\\x",L"D:\\P\\{1..9}"}) rejects([&]{match(bad,L"D:\\P\\x");});
        DirectoryRule a;a.id="a";a.patterns={L"D:\\P\\**\\src",L"D:\\P\\**\\docs"};a.depth=3;
        DirectoryRule b;b.id="b";b.patterns={L"D:\\P\\**"};b.depth=1;
        auto rules=std::make_shared<RuleSet>(std::vector<DirectoryRule>{a,b});
        expect(rules->resolve({}, {},L"D:\\P\\docs").rule_id=="a","OR patterns");
        expect(rules->resolve({}, {},L"D:\\P\\assets").rule_id=="b","fallback rule");
        expect(RuleSet({b,a}).resolve({}, {},L"D:\\P\\src").rule_id=="b","first match order");
        expect(rules->resolve({}, DepthOverride{std::nullopt},L"D:\\P\\docs").source==PolicySource::tab,"explicit all overrides rules");
        expect(rules->resolve({}, {},std::nullopt).source==PolicySource::global,"virtual location");
        a.native=true;expect(RuleSet({a,b}).resolve({}, {},L"D:\\P\\src").native,"native action");
        a.enabled=false;expect(RuleSet({a,b}).resolve({}, {},L"D:\\P\\src").rule_id=="b","disabled rule");
        rejects([&]{RuleSet duplicate({b,b});});
        auto huge=b;huge.patterns={L"D:\\P\\{a,b,c,d,e,f,g,h,i}\\{a,b,c,d,e,f,g,h,i}\\{a,b,c,d,e,f,g,h,i}"};rejects([&]{RuleSet over({huge});});
        auto root=fs::temp_directory_path()/(L"TabTail-rules-"+std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root);ConfigStore store(root);
        atomic_write(store.path(),"schema_version=1\nrevision=5\nenabled=false\nautostart=false\n[display]\ndepth=2\nseparator='\\'\n");
        expect(store.load().revision==5&&store.load().rules.empty(),"legacy read without migration");
        expect(read_file(store.path()).find("schema_version=1")!=std::string::npos,"read does not rewrite");
        auto c=store.update([&](Config& c){c.rules={b};c.language="en";});
        expect(store.load().rules==c.rules&&store.load().language=="en","schema2 TOML roundtrip");
        expect(config_json(store.load())["schema_version"]==4,"write upgrades schema");
        auto saved=read_file(store.path());rejects([&]{store.update([](Config& c){c.rules[0].patterns={L"D:\\[!abc]"};});});
        expect(read_file(store.path())==saved,"invalid save atomic");
        auto result=application_action(root,{{"action","language"},{"language","zh-CN"},{"revision",c.revision}});
        expect(result.at("code")==0&&store.load().language=="zh-CN","application saves language");
        rejects([&]{application_action(root,{{"action","display"},{"depth","4"},{"separator"," / "},{"revision",c.revision}});});
        expect(store.load().depth==2,"stale edit never overwrites configuration");
        auto revision=store.load().revision;
        result=application_action(root,{{"action","display"},{"depth","4"},{"separator"," / "},{"revision",revision}});
        expect(result.at("result").at("saved")==true&&store.load().depth==4&&store.load().separator==L" / ","application display save");
        rejects([&]{application_action(root,{{"action","rule.put"},{"id","other"},{"rule",rule_json(b)}});});
        auto missing=b;missing.id="missing";
        rejects([&]{application_action(root,{{"action","rule.put"},{"id","missing"},{"rule",rule_json(missing)},{"replace",true}});});
        expect(store.load().rules==c.rules,"invalid rule targets preserved");
        RuntimePolicy p{{},rules,DepthOverride{1}};
        auto q=runtime_policy_from_json(runtime_policy_json(p));
        expect(q.resolve(L"D:\\P\\src")==p.resolve(L"D:\\P\\src"),"bridge policy roundtrip");
        TabRegistry registry("rules-test",{},1,rules);TabIdentity identity{1,42,1,1,1,{},1};identity.tab_guid[0]=1;
        auto t=registry.register_tab(identity,{L"D:\\P\\src",L"src"});expect(t.effective.rule_id=="a","register rules");
        t=registry.navigate(t.id,{L"D:\\P\\assets",L"assets"},2);expect(t.effective.rule_id=="b"&&t.effective.display.depth==1,"navigation resolves again");
        t=registry.set_depth(t.id,DepthOverride{4},1);t=registry.navigate(t.id,{L"D:\\P\\src",L"src"},3);expect(t.effective.display.depth==4,"override follows navigation");
        t=registry.reset_depth(t.id,2);expect(t.effective.rule_id=="a"&&t.effective.display.depth==3,"reset automatic rules");
        fs::remove_all(root);
        std::cout<<checks<<" rules checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
}
