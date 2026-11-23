#include "config/tab_features.hpp"
#include "policy/rules.hpp"
#include "policy/entries.hpp"
#include "config/config.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace sinan {
namespace {
using J=FeatureJson;
void keys(const J& j,std::initializer_list<const char*> allowed) {
    if(!j.is_object())throw Error(2,"Expected a feature table");
    for(auto it=j.begin();it!=j.end();++it)
        if(std::none_of(allowed.begin(),allowed.end(),[&](auto k){return it.key()==k;}))throw Error(2,"Unknown feature key: "+it.key());
}
bool color(const J& value) {
    if(!value.is_string())return false;
    auto s=value.get<std::string>();
    return s=="system"||(s.size()==7&&s[0]=='#'&&std::all_of(s.begin()+1,s.end(),[](unsigned char c){return std::isxdigit(c)!=0;}));
}
void merge(J& target,const J& source) {
    if(!source.is_object())return;
    // A higher-layer generic text color also overrides lower-layer themed
    // colors. The same layer's explicit theme/state fields are merged below.
    if(source.contains("text")&&source.at("text").is_object()&&source.at("text").contains("font_color")&&source.at("text").at("font_color")!="inherit")
        for(auto theme:{"light","dark"})for(auto state:{"normal","hover","selected"})target[theme]["text"][state]=source.at("text").at("font_color");
    for(auto it=source.begin();it!=source.end();++it) {
        if(it.key()=="enabled")continue;
        if(it.value().is_object()){if(!target[it.key()].is_object())target[it.key()]=J::object();merge(target[it.key()],it.value());}
        else if(it.value()!="inherit")target[it.key()]=it.value();
    }
}
}
bool installed_font(const std::wstring& family) {
    LOGFONTW lf{};lf.lfCharSet=DEFAULT_CHARSET;wcsncpy_s(lf.lfFaceName,family.c_str(),_TRUNCATE);
    auto dc=GetDC(nullptr);if(!dc)return false;
    bool found=false;
    EnumFontFamiliesExW(dc,&lf,[](const LOGFONTW*,const TEXTMETRICW*,DWORD,LPARAM data)->int{*reinterpret_cast<bool*>(data)=true;return 0;},reinterpret_cast<LPARAM>(&found),0);
    ReleaseDC(nullptr,dc);return found;
}
Shortcut parse_shortcut(std::string_view chord) {
    if(chord.empty())return {};
    if(chord.back()=='+')throw Error(2,"Shortcut cannot end with +");
    std::istringstream stream{std::string(chord)};std::string token;Shortcut out;std::set<std::string> used;
    while(std::getline(stream,token,'+')) {
        std::transform(token.begin(),token.end(),token.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
        if(token.empty()||!used.insert(token).second)throw Error(2,"Invalid shortcut chord");
        if(token=="CTRL")out.modifiers|=MOD_CONTROL;
        else if(token=="ALT")out.modifiers|=MOD_ALT;
        else if(token=="SHIFT")out.modifiers|=MOD_SHIFT;
        else if(token=="WIN")out.modifiers|=MOD_WIN;
        else {
            if(out.key)throw Error(2,"Shortcut needs exactly one key");
            if(token.size()==1&&((token[0]>='A'&&token[0]<='Z')||(token[0]>='0'&&token[0]<='9')))out.key=token[0];
            else if(token.size()>1&&token[0]=='F') {
                int n=0;try{size_t end{};n=std::stoi(token.substr(1),&end);if(end!=token.size()-1)n=0;}catch(...){}
                if(n<1||n>24)throw Error(2,"Shortcut F key must be F1..F24");out.key=VK_F1+n-1;
            }else throw Error(2,"Shortcut key must be A..Z, 0..9 or F1..F24");
        }
    }
    if(!out.key||!(out.modifiers&(MOD_CONTROL|MOD_ALT|MOD_WIN)))throw Error(2,"Shortcut needs Ctrl, Alt or Win and a key");
    // Reserved Explorer / system commands. Do not take over native navigation or close.
    if((out.modifiers==MOD_CONTROL&&(out.key=='W'||out.key=='T'||out.key=='N'||out.key=='L'||out.key=='R'||out.key=='E'||out.key=='F'))||
       (out.modifiers==MOD_ALT&&(out.key==VK_F4||out.key=='D'))||(out.modifiers==(MOD_CONTROL|MOD_SHIFT)&&(out.key=='N'||out.key=='T'||out.key=='W'))||out.modifiers==MOD_WIN)
        throw Error(2,"Shortcut conflicts with a reserved Explorer/system command");
    out.modifiers|=MOD_NOREPEAT;return out;
}
std::string_view shell_shortcut_action(std::string_view key) {
    if(key=="open_terminal")return "explorer.open_terminal";
    if(key=="copy_directory")return "explorer.copy_directory";
    if(key=="copy_selection")return "explorer.copy_selection";
    if(key=="locate_clipboard"||key=="locate_clipboard_global")return "explorer.locate_clipboard";
    if(key=="new_folder")return "explorer.new_folder";
    if(key=="new_file")return "explorer.new_file";
    if(key=="transfer_selection")return "explorer.transfer_selection";
    if(key=="extract_archive")return "explorer.extract_archive";
    if(key=="reopen_closed_tab")return "explorer.reopen_closed_tab";
    if(key=="merge_duplicate_tabs")return "explorer.merge_duplicate_tabs";
    if(key=="open_parent_tab")return "explorer.open_parent_tab";
    if(key=="toggle_file_extensions")return "explorer.toggle_file_extensions";
    return {};
}
void validate_folder_name(std::wstring_view name) {
    if(name.empty()||name.size()>255||name.back()==L'.'||name.back()==L' '||
        name.find_first_of(L"<>:\"/\\|?*")!=name.npos||
        std::any_of(name.begin(),name.end(),[](wchar_t c){return c<32;}))
        throw Error(2,"Expected one valid Windows folder name");
    auto stem=std::wstring(name.substr(0,name.find(L'.')));
    while(!stem.empty()&&stem.back()==L' ')stem.pop_back();
    if(!stem.empty())CharUpperBuffW(stem.data(),static_cast<DWORD>(stem.size()));
    if(stem==L"CON"||stem==L"PRN"||stem==L"AUX"||stem==L"NUL"||stem==L"CONIN$"||stem==L"CONOUT$"||
        (stem.size()==4&&(stem.starts_with(L"COM")||stem.starts_with(L"LPT"))&&
            std::wstring_view(L"123456789¹²³").find(stem.back())!=std::wstring_view::npos))
        throw Error(2,"Windows reserves this folder name");
}
FeatureJson default_features() {
    J shortcuts{{"new_folder_name",""}};
    for(auto key:explorer_shortcut_keys)shortcuts[key]="";
    return {{"directories",{{"entries",J::array()}}},{"scripts",{{"entries",J::array()}}},{"shortcuts",std::move(shortcuts)},
        {"archives",{{"tools",J::array()},{"default_tool",""},{"formats",J::object()},{"recycle",false}}},
        {"notifications",{{"mode","all"}}},{"tab_behavior",{{"pinned_tabs_protect_close",true},{"locked_navigation","new_tab"},{"pinned_position","keep"}}},
        {"tab_style",{{"default",J::object()},{"pinned",{{"enabled",true},{"icon",{{"source","builtin:pin-rounded"},{"color","#F59E0B"}}}}},
            {"protected",{{"enabled",true}}},{"locked",{{"enabled",true},{"icon",{{"source","builtin:lock-rounded"},{"color","#F59E0B"}}}}},
            {"rules",J::array()}}},{"restore",{{"automatic",false},{"max_tabs",20}}}};
}
std::string archive_extension(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(!value.starts_with('.'))value.insert(value.begin(),'.');
    if(value.size()<2||value.size()>32||value.ends_with('.')||value.find("..")!=value.npos||
        value.find_first_not_of(".abcdefghijklmnopqrstuvwxyz0123456789")!=value.npos)
        throw Error(2,"Invalid archive extension");
    return value;
}
static void validate_archives(const J& archives) {
    keys(archives,{"tools","default_tool","formats","recycle"});
    const auto& tools=archives.at("tools");
    const auto& formats=archives.at("formats");
    if(!tools.is_array()||!formats.is_object())throw Error(2,"Invalid archive tools or format rules");
    std::set<std::string> ids;
    for(const auto& tool:tools) {
        keys(tool,{"id","name","type","path"});
        const auto id=tool.at("id").get<std::string>();
        const auto name=tool.at("name").get<std::string>();
        const auto type=tool.at("type").get<std::string>();
        const auto path=wide(tool.at("path").get<std::string>());
        if(id.empty()||id.size()>64||id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_")!=id.npos||
           !ids.insert(id).second||name.empty()||name.size()>256)
            throw Error(2,"Invalid or duplicate archive tool ID or name");
        if(type!="7zip"&&type!="rar"&&type!="tar")throw Error(2,"Unknown archive tool type");
        try {directory_key(path);} catch(const PolicyError& error){throw Error(2,error.what());}
        if(_wcsicmp(fs::path(path).extension().c_str(),L".exe")!=0)throw Error(2,"Archive tool requires an absolute .exe path");
    }
    const auto fallback=archives.at("default_tool").get<std::string>();
    if(!fallback.empty()&&!ids.contains(fallback))throw Error(2,"Default archive tool does not exist");
    std::set<std::string> extensions;
    for(const auto& [extension,tool]:formats.items())
        if(!extensions.insert(archive_extension(extension)).second||!ids.contains(tool.get<std::string>()))
            throw Error(2,"Duplicate archive extension or unknown archive tool");
    archives.at("recycle").get<bool>();
}
void validate_style(const J& s,bool fonts) {
    try {
    keys(s,{"enabled","text","icon","light","dark"});
    if(s.contains("enabled"))s.at("enabled").get<bool>();
    if(s.contains("text")) {
        auto t=s.at("text");keys(t,{"font_family","font_color","bold"});
        if(t.contains("font_family")) {
            auto f=t.at("font_family").get<std::string>();
            if(f.empty()||f.find(char(0))!=f.npos||wide(f).size()>=LF_FACESIZE)throw Error(2,"Invalid font family");
            if(fonts&&f!="system"&&!installed_font(wide(f)))throw Error(2,"Font is not installed: "+f);
        }
        if(t.contains("font_color")&&!color(t.at("font_color")))throw Error(2,"Text color must be system or #RRGGBB");
        if(t.contains("bold")&&!t.at("bold").is_boolean()&&t.at("bold")!="inherit"&&t.at("bold")!="system")throw Error(2,"bold must be inherit, system or a boolean");
    }
    if(s.contains("icon")) {
        auto i=s.at("icon");keys(i,{"source","color"});
        if(i.contains("source")) {
            auto v=i.at("source").get<std::string>();if(v!="system"&&v!="builtin:pin-rounded"&&v!="builtin:lock-rounded")throw Error(2,"Unknown icon source");
        }
        if(i.contains("color")&&!color(i.at("color")))throw Error(2,"Icon color must be system or #RRGGBB");
    }
    for(auto theme:{"light","dark"})if(s.contains(theme)) {
        keys(s.at(theme),{"background","text"});
        for(auto property:{"background","text"})if(s.at(theme).contains(property)) {
            auto values=s.at(theme).at(property);keys(values,{"normal","hover","selected"});
            for(auto& v:values)if(!color(v))throw Error(2,"State color must be system or #RRGGBB");
        }
    }
    }catch(const J::exception& ex){throw Error(2,std::string("Invalid tab style: ")+ex.what());}
}
void validate_features(const J& j,bool fonts) {
    try {
        keys(j,{"shortcuts","notifications","tab_behavior","tab_style","restore","directories","scripts","archives"});
        auto defaults=default_features();
        for(auto it=defaults.begin();it!=defaults.end();++it)if(!j.contains(it.key()))throw Error(2,"Missing feature group: "+it.key());
        validate_archives(j.at("archives"));
        for(auto group:{"shortcuts","notifications","tab_behavior","restore"}){if(!j.at(group).is_object())throw Error(2,"Expected feature table");for(auto it=defaults.at(group).begin();it!=defaults.at(group).end();++it)if(!j.at(group).contains(it.key()))throw Error(2,"Missing feature field: "+std::string(group)+"."+it.key());}
        keys(j.at("directories"),{"entries"});auto entries=entries_from_json(j.at("directories").at("entries"));
        std::set<std::pair<UINT,UINT>> all_chords;
        const auto& shortcuts=j.at("shortcuts");
        if(shortcuts.size()!=defaults.at("shortcuts").size())throw Error(2,"Unknown shortcut field");
        const auto name=shortcuts.at("new_folder_name").get<std::string>();
        if(!name.empty())validate_folder_name(wide(name));
        for(auto key:explorer_shortcut_keys) {
            auto chord=parse_shortcut(shortcuts.at(key).get<std::string>());
            if(chord.key&&!all_chords.insert({chord.modifiers,chord.key}).second)throw Error(2,"Duplicate shortcut");
        }
        for(auto& e:entries){auto c=parse_shortcut(e.shortcut);if(e.enabled&&c.key&&!all_chords.insert({c.modifiers,c.key}).second)throw Error(2,"Duplicate directory/Explorer shortcut: "+e.shortcut);}
        keys(j.at("scripts"),{"entries"});
        const auto& scripts=j.at("scripts").at("entries");
        if(!scripts.is_array()||scripts.size()>128)throw Error(2,"Expected up to 128 PowerShell actions");
        std::set<std::string> script_ids;
        for(const auto& script:scripts) {
            keys(script,{"id","name","path","scope","shortcut"});
            const auto id=script.at("id").get<std::string>();
            const auto script_name=script.at("name").get<std::string>();
            const auto script_path=wide(script.at("path").get<std::string>());
            const auto scope=script.at("scope").get<std::string>();
            if(id.empty()||id.size()>64||id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_")!=id.npos||!script_ids.insert(id).second)
                throw Error(2,"PowerShell action ID must be unique and use lowercase letters, digits, hyphens or underscores");
            if(script_name.find_first_not_of(" \t")==script_name.npos||script_name.size()>256||
                std::any_of(script_name.begin(),script_name.end(),[](unsigned char ch){return ch<32;}))
                throw Error(2,"Invalid PowerShell action name");
            try {directory_key(script_path);} catch(const PolicyError& ex){throw Error(2,ex.what());}
            if(_wcsicmp(fs::path(script_path).extension().c_str(),L".ps1")!=0)throw Error(2,"PowerShell action requires an absolute .ps1 path");
            if(scope!="explorer"&&scope!="global")throw Error(2,"Invalid PowerShell action scope");
            const auto chord=parse_shortcut(script.at("shortcut").get<std::string>());
            if(chord.key&&!all_chords.insert({chord.modifiers,chord.key}).second)throw Error(2,"Duplicate PowerShell shortcut: "+id);
        }
        if(j.contains("notifications")) {
            auto n=j.at("notifications");keys(n,{"mode"});auto mode=n.value("mode",std::string("all"));
            if(mode!="all"&&mode!="errors"&&mode!="off")throw Error(2,"Notification mode must be all, errors or off");
        }
        if(j.contains("tab_behavior")) {
            auto b=j.at("tab_behavior");keys(b,{"pinned_tabs_protect_close","locked_navigation","pinned_position"});
            auto position=b.value("pinned_position",std::string("keep"));if(position!="keep"&&position!="left"&&position!="right")throw Error(2,"Pinned position must be keep, left or right");
            if(!b.value("pinned_tabs_protect_close",true)||b.value("locked_navigation",std::string("new_tab"))!="new_tab")throw Error(2,"Unsupported tab behavior");
        }
        if(j.contains("restore")) {
            auto r=j.at("restore");keys(r,{"automatic","max_tabs"});r.value("automatic",false);
            if(r.contains("max_tabs")&&(!r.at("max_tabs").is_number_integer()||r.at("max_tabs").get<int64_t>()<1||r.at("max_tabs").get<int64_t>()>100))throw Error(2,"max_tabs must be 1..100");
        }
        if(j.contains("tab_style")) {
            auto styles=j.at("tab_style");keys(styles,{"default","pinned","protected","locked","rules"});
            for(auto name:{"default","pinned","protected","locked"})if(styles.contains(name))validate_style(styles.at(name),fonts);
            std::set<std::string> ids;
            if(styles.contains("rules")) {
                if(!styles.at("rules").is_array()||styles.at("rules").size()>256)throw Error(2,"Style rules must be an array of up to 256 rules");
                for(auto& rule:styles.at("rules")) {
                    keys(rule,{"id","name","enabled","match","patterns","case_sensitive","style"});
                    if(!rule.at("patterns").is_array())throw Error(2,"Style rule patterns must be an array");
                    auto match=rule;match.erase("style");match["action"]="depth";match["depth"]=2;
                    // Same engine and validation as title rules, including positive-only glob syntax.
                    DirectoryRule r;r.id=match.at("id");r.name=wide(match.value("name",r.id));r.enabled=match.value("enabled",true);
                    auto mode=match.value("match",std::string("glob"));if(mode!="glob"&&mode!="exact")throw Error(2,"Invalid style match mode");r.exact=mode=="exact";
                    r.case_sensitive=match.value("case_sensitive",false);for(auto& p:match.at("patterns"))r.patterns.push_back(wide(p.get<std::string>()));
                    RuleSet compiled({r});if(!ids.insert(r.id).second)throw Error(2,"Duplicate style rule ID");validate_style(rule.at("style"),fonts);
                }
            }
        }
        if(j.dump().size()>200000)throw Error(2,"Feature configuration exceeds 200000 bytes");
    }catch(const J::exception& e){throw Error(2,std::string("Invalid feature configuration: ")+e.what());}
    catch(const PolicyError& e){throw Error(2,e.what());}
}
FeatureJson resolve_style(const J& features,const TabFlags& flags,const std::wstring& path,const J& temporary) {
    J out=J::object();auto styles=features.at("tab_style");merge(out,styles.value("default",J::object()));
    for(auto& rule:styles.value("rules",J::array())) {
        DirectoryRule r;r.id=rule.at("id");r.enabled=rule.value("enabled",true);r.exact=rule.value("match",std::string("glob"))=="exact";r.case_sensitive=rule.value("case_sensitive",false);
        for(auto& p:rule.at("patterns"))r.patterns.push_back(wide(p.get<std::string>()));
        RuleSet compiled({r});if(compiled.resolve({}, {}, path).source==PolicySource::rule){merge(out,rule.at("style"));out["rule_id"]=r.id;break;}
    }
    for(auto [name,enabled]:{std::pair{"protected",flags.protected_tab},std::pair{"pinned",flags.pinned},std::pair{"locked",flags.locked}})
        if(enabled&&styles.contains(name)&&styles.at(name).value("enabled",true))merge(out,styles.at(name));
    merge(out,temporary);return out;
}
FeatureJson style_for_theme(const J& s,bool dark,bool selected,bool hover,bool high_contrast) {
    J out=s;out.erase("light");out.erase("dark");
    if(high_contrast){out.erase("text");out.erase("background");return out;}
    auto theme=s.value(dark?"dark":"light",J::object());auto state=selected?"selected":hover?"hover":"normal";
    for(auto field:{"background","text"})if(theme.contains(field)) {
        auto values=theme.at(field);auto value=values.contains(state)?values.at(state):values.value("normal",J("inherit"));
        if(value!="inherit"){if(std::string_view(field)=="text")out["text"]["font_color"]=value;else out["background"]=value;}
    }
    return out;
}
double contrast_ratio(std::string_view a,std::string_view b) {
    auto luminance=[](std::string_view s){if(s.size()!=7||s[0]!='#')throw Error(2,"Contrast preview needs two #RRGGBB colors");double v[3]{};
        for(int i=0;i<3;++i){auto x=std::stoi(std::string(s.substr(1+i*2,2)),nullptr,16)/255.0;v[i]=x<=0.04045?x/12.92:std::pow((x+0.055)/1.055,2.4);}return v[0]*0.2126+v[1]*0.7152+v[2]*0.0722;};
    auto x=luminance(a),y=luminance(b);return (std::max(x,y)+0.05)/(std::min(x,y)+0.05);
}
FeatureJson flags_json(const TabFlags& f){return {{"pinned",f.pinned},{"protected",f.protected_tab},{"locked",f.locked},{"effective_close_protection",f.close_protected()}};}
}
