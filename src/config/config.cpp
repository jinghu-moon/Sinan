#include "config/config.hpp"
#include <toml.hpp>
#include "policy/entries.hpp"
#include <charconv>
#include <limits>
#include <sstream>

namespace sinan {
std::optional<int> parse_depth(std::string_view text) {
    if (text == "all") return std::nullopt;
    int value = 0;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || p != text.data() + text.size() || value < 1 || value > 32)
        throw Error(2, "depth 必须为 1..32 或 all");
    return value;
}
namespace {
void keys(const Json& value, std::initializer_list<const char*> allowed) {
    if (!value.is_object()) throw Error(2, "Expected a configuration table");
    for (auto it=value.begin();it!=value.end();++it) {
        if (std::none_of(allowed.begin(),allowed.end(),[&](const char* k){return it.key()==k;}))
            throw Error(2,"Unknown configuration key: "+it.key());
    }
}
std::optional<int> json_depth(const Json& value) {
    if (value.is_string()) return parse_depth(value.get<std::string>());
    if (!value.is_number_integer()) throw Error(2,"Depth must be an integer or all");
    auto n=value.get<int64_t>();if(n<1||n>32) throw Error(2,"Depth must be 1..32 or all");return static_cast<int>(n);
}
}
Json entry_json(const DirectoryEntry& e) {
    return {{"id",e.id},{"name",utf8(e.name)},{"path",utf8(e.path)},{"shortcut",e.shortcut},{"enabled",e.enabled},{"descendants",e.descendants},{"title",e.title}};
}
std::vector<DirectoryEntry> entries_from_json(const Json& value) {
    if(!value.is_array()||value.size()>128)throw Error(2,"directories.entries must be an array of up to 128 entries");
    std::vector<DirectoryEntry> out;
    try {for(auto& j:value){keys(j,{"id","name","path","shortcut","enabled","descendants","title"});DirectoryEntry e;
        e.id=j.at("id");e.name=wide(j.at("name").get<std::string>());e.path=wide(j.at("path").get<std::string>());
        e.shortcut=j.value("shortcut",std::string{});e.enabled=j.value("enabled",true);e.descendants=j.value("descendants",false);e.title=j.value("title",std::string("alias_depth"));out.push_back(e);
    }validate_entries(out);}catch(const Json::exception& e){throw Error(2,std::string("Invalid directory entry: ")+e.what());}catch(const PolicyError& e){throw Error(2,e.what());}return out;
}
DisplayOptions Config::display_options() const {return {depth,separator,entries_from_json(features.at("directories").at("entries"))};}
Json rule_json(const DirectoryRule& r) {
    Json patterns=Json::array();for(const auto& p:r.patterns) patterns.push_back(utf8(p));
    Json out{{"id",r.id},{"name",utf8(r.name)},{"enabled",r.enabled},{"match",r.exact?"exact":"glob"},
        {"patterns",patterns},{"action",r.native?"native":"depth"},{"case_sensitive",r.case_sensitive}};
    if(!r.native) out["depth"]=r.depth?Json(*r.depth):Json("all");return out;
}
DirectoryRule rule_from_json(const Json& j) {
    keys(j,{"id","name","enabled","match","patterns","action","depth","case_sensitive"});
    try {
        DirectoryRule r;r.id=j.at("id").get<std::string>();r.name=wide(j.value("name",r.id));
        r.enabled=j.value("enabled",true);r.case_sensitive=j.value("case_sensitive",false);
        auto mode=j.value("match",std::string("glob"));if(mode!="glob"&&mode!="exact") throw Error(2,"match must be exact or glob");r.exact=mode=="exact";
        auto action=j.value("action",std::string("depth"));if(action!="depth"&&action!="native") throw Error(2,"action must be depth or native");r.native=action=="native";
        if(r.native && j.contains("depth")) throw Error(2,"native rules must not specify depth");
        if(!r.native) r.depth=json_depth(j.at("depth"));
        if(!j.at("patterns").is_array()) throw Error(2,"patterns must be an array");
        for(const auto& p:j.at("patterns")) r.patterns.push_back(wide(p.get<std::string>()));
        return r;
    } catch(const Json::exception& e){throw Error(2,std::string("Invalid directory rule: ")+e.what());}
}
void validate_config(const Config& config) {
    if (config.revision < 1) throw Error(2, "revision must be positive");
    if(config.language!="auto"&&config.language!="zh-CN"&&config.language!="en") throw Error(2,"language must be auto, zh-CN or en");
    validate_appearance(config.tabs);
    validate_features(config.features);
    try { validate_display(config.display_options()); RuleSet compiled(config.rules); }
    catch (const PolicyError& e) { throw Error(2, e.what()); }
    size_t size=0;for(const auto& r:config.rules) size+=rule_json(r).dump().size()+1;
    size+=config.features.at("directories").dump().size();
    if(size>200000) throw Error(2,"Serialized directory rules exceed 230000 bytes");
}
Json config_json(const Config& c) {
    Json rules=Json::array();for(const auto& r:c.rules) rules.push_back(rule_json(r));
    auto out = Json{{"schema_version",4},{"revision",c.revision},{"enabled",c.enabled},{"autostart",c.autostart},
        {"display",{{"depth",c.depth?Json(*c.depth):Json("all")},{"separator",utf8(c.separator)}}},
        {"ui",{{"language",c.language}}},{"appearance",{{"tabs",appearance_json(c.tabs)}}},{"rules",rules}};
    for(auto it=c.features.begin();it!=c.features.end();++it)out[it.key()]=it.value();
    return out;
}
Config config_from_json(const Json& table) {
    try {
        keys(table,{"schema_version","revision","enabled","autostart","display","ui","rules","appearance","shortcuts","notifications","tab_behavior","tab_style","restore","directories"});
        if(!table.at("schema_version").is_number_integer()) throw Error(2,"Invalid schema_version");
        if(table.at("schema_version")!=1&&table.at("schema_version")!=2&&table.at("schema_version")!=3&&table.at("schema_version")!=4)throw Error(2,"Unsupported schema_version");
        auto schema=table.at("schema_version").get<int>();if(schema!=1&&schema!=2&&schema!=3&&schema!=4) throw Error(2,"Unsupported schema_version");
        if(schema==1&&(table.contains("rules")||table.contains("ui"))) throw Error(2,"rules/ui require schema_version=2");
        Config c;if(!table.at("revision").is_number_integer()) throw Error(2,"revision must be an integer");
        c.revision=table.at("revision").get<int64_t>();c.enabled=table.at("enabled").get<bool>();c.autostart=table.at("autostart").get<bool>();
        const auto& display=table.at("display");keys(display,{"depth","separator"});
        c.depth=json_depth(display.at("depth"));c.separator=wide(display.at("separator").get<std::string>());
        if(table.contains("ui")) {keys(table.at("ui"),{"language"});c.language=table.at("ui").value("language",std::string("auto"));}
        if(table.contains("appearance")) {
            if(schema<2) throw Error(2,"appearance requires schema_version=2");
            keys(table.at("appearance"),{"tabs"});
            if(table.at("appearance").contains("tabs")) c.tabs=appearance_from_json(table.at("appearance").at("tabs"));
        }
        if(table.contains("rules")) {
            if(!table.at("rules").is_array()) throw Error(2,"rules must be an array");
            for(const auto& r:table.at("rules")) c.rules.push_back(rule_from_json(r));
        }
        for(auto key:{"shortcuts","notifications","tab_behavior","tab_style","restore","directories"})if(table.contains(key)) {
            if(std::string_view(key)=="directories"&&schema<4)throw Error(2,"Directory entries require schema_version=4");
            if(std::string_view(key)=="tab_behavior"&&schema<4&&table.at(key).contains("pinned_position"))throw Error(2,"Pinned docking requires schema_version=4");
            if(schema<3)throw Error(2,"Tab protection and styles require schema_version=3");
            if(std::string_view(key)=="tab_style") {
                if(!table.at(key).is_object())throw Error(2,"tab_style must be a table");
                for(auto it=table.at(key).begin();it!=table.at(key).end();++it)c.features[key][it.key()]=it.value();
            }else c.features[key].merge_patch(table.at(key));
        }
        validate_config(c);return c;
    } catch(const Json::exception& e){throw Error(2,std::string("Invalid configuration: ")+e.what());}
}
namespace {
void insert_json(toml::table&,const std::string&,const Json&);
void push_json(toml::array& array,const Json& j) {
    if(j.is_object()){toml::table t;for(auto it=j.begin();it!=j.end();++it)insert_json(t,it.key(),it.value());array.push_back(std::move(t));}
    else if(j.is_array()){toml::array a;for(auto& v:j)push_json(a,v);array.push_back(std::move(a));}
    else if(j.is_boolean())array.push_back(j.get<bool>());
    else if(j.is_number_integer())array.push_back(j.get<int64_t>());
    else if(j.is_string())array.push_back(j.get<std::string>());
    else throw Error(2,"Unsupported TOML value");
}
void insert_json(toml::table& table,const std::string& key,const Json& j) {
    if(j.is_object()){toml::table t;for(auto it=j.begin();it!=j.end();++it)insert_json(t,it.key(),it.value());table.insert(key,std::move(t));}
    else if(j.is_array()){toml::array a;for(auto& v:j)push_json(a,v);table.insert(key,std::move(a));}
    else if(j.is_boolean())table.insert(key,j.get<bool>());
    else if(j.is_number_integer())table.insert(key,j.get<int64_t>());
    else if(j.is_string())table.insert(key,j.get<std::string>());
    else throw Error(2,"Unsupported TOML value");
}
}
std::string config_toml(const Config& c) {
    validate_config(c);auto j=config_json(c);toml::table table;
    for(auto it=j.begin();it!=j.end();++it)insert_json(table,it.key(),it.value());
    std::ostringstream out;out<<table<<'\n';return out.str();
}
Config ConfigStore::load() const {
    if(!fs::exists(path())) return {};
    try {
        auto bytes=read_file(path());if(bytes.size()>1024*1024) throw Error(2,"Configuration exceeds 1 MiB");
        auto table=toml::parse(bytes);std::ostringstream out;out<<toml::json_formatter(table);
        return config_from_json(Json::parse(out.str()));
    } catch(const toml::parse_error& e){throw Error(2,"TOML: "+std::string(e.description()));}
}
void validate_appearance(const TabAppearance& value) {
    if(value.width_mode!="system"&&value.width_mode!="fixed"&&value.width_mode!="content")
        throw Error(2,"width_mode must be system, fixed or content");
    for(auto n:{value.width,value.min_width,value.max_width})
        if(n<80||n>1000) throw Error(2,"Tab widths must be integers in 80..1000 DIP");
    if(value.min_width>value.max_width) throw Error(2,"min_width must not exceed max_width");
}
int parse_tab_width(std::string_view text) {
    int value=0;auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if(error!=std::errc{}||end!=text.data()+text.size()||value<80||value>1000)
        throw Error(2,"Tab widths must be integers in 80..1000 DIP");
    return value;
}
Json appearance_json(const TabAppearance& value) {
    return {{"width_mode",value.width_mode},{"width",value.width},{"min_width",value.min_width},{"max_width",value.max_width}};
}
TabAppearance appearance_from_json(const Json& value) {
    keys(value,{"width_mode","width","min_width","max_width"});
    try {
        TabAppearance result;
        if(value.contains("width_mode")) result.width_mode=value.at("width_mode").get<std::string>();
        for(auto [key,destination]:{std::pair{"width",&result.width},std::pair{"min_width",&result.min_width},std::pair{"max_width",&result.max_width}}) {
            if(!value.contains(key)) continue;
            const auto& n=value.at(key);
            if(!n.is_number_integer()||n.get<int64_t>()<80||n.get<int64_t>()>1000) throw Error(2,"Tab widths must be integers in 80..1000 DIP");
            *destination=n.get<int>();
        }
        validate_appearance(result);return result;
    }catch(const Json::exception& e){throw Error(2,std::string("Invalid tab appearance: ")+e.what());}
}
std::string runtime_policy_json(const RuntimePolicy& policy) {
    Json rules=Json::array();if(policy.rules) for(const auto& r:policy.rules->definitions()) rules.push_back(rule_json(r));
    Json entries=Json::array();for(auto& e:policy.defaults.entries)entries.push_back(entry_json(e));
    Json out{{"entries",entries},{"depth",policy.defaults.depth?Json(*policy.defaults.depth):Json("all")},{"separator",utf8(policy.defaults.separator)},
        {"rules",rules},{"override",policy.override?Json(policy.override->depth?Json(*policy.override->depth):Json("all")):Json(nullptr)}};
    return out.dump();
}
RuntimePolicy runtime_policy_from_json(std::string_view value) {
    try {
        const auto j=Json::parse(value);keys(j,{"depth","separator","rules","override","entries"});RuntimePolicy p;
        p.defaults={json_depth(j.at("depth")),wide(j.at("separator").get<std::string>())};if(j.contains("entries"))p.defaults.entries=entries_from_json(j.at("entries"));validate_display(p.defaults);
        if(!j.at("override").is_null()) p.override=DepthOverride{json_depth(j.at("override"))};
        std::vector<DirectoryRule> rules;if(!j.at("rules").is_array()) throw Error(2,"rules must be an array");
        for(const auto& r:j.at("rules")) rules.push_back(rule_from_json(r));p.rules=std::make_shared<RuleSet>(std::move(rules));return p;
    }catch(const Json::exception& e){throw Error(2,std::string("Invalid runtime policy: ")+e.what());}
}
Config ConfigStore::update(const std::function<void(Config&)>& edit) const {
    FileLock lock(root_ / L"config.lock");
    auto c = load();
    edit(c); validate_config(c);
    if (c.revision == std::numeric_limits<int64_t>::max()) throw Error(2, "revision 已超出范围");
    ++c.revision;
    if(fs::exists(path())) {
        auto bytes=read_file(path());auto old=toml::parse(bytes);
        if(old["schema_version"].value_or(0)<4) {
            auto backup=root_/L"sinan.pre-schema4.toml";
            if(!fs::exists(backup))atomic_write(backup,bytes);
        }
        if(old["schema_version"].value_or(0)<3) {
            auto backup=root_/L"sinan.pre-schema3.toml";
            if(!fs::exists(backup))atomic_write(backup,bytes);
        }
    }
    atomic_write(path(), config_toml(c)); return c;
}
}
