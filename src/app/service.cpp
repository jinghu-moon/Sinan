#include "app/service.hpp"
#include "agent/runtime.hpp"
#include "formatter/formatter.hpp"
#include "platform/explorer.hpp"
#include "installer/startup.hpp"
#include "installer/install.hpp"
#include "compat/symbols.hpp"
#include <algorithm>

namespace tabtail {
namespace { struct AppearanceConflict {}; }
Json test_rules(const Config& config,const std::wstring& path) {
    RuleSet rules(config.rules);auto policy=rules.resolve(config.display_options(),{},path);
    auto title=policy.native?Title{path}:format_title(path,policy.display);
    return {{"path",utf8(path)},{"source",policy.source==PolicySource::rule?"rule":"global"},
        {"rule_id",policy.rule_id},{"matched_pattern",utf8(policy.matched_pattern)},
        {"action",policy.native?"native":"depth"},{"depth",policy.display.depth?Json(*policy.display.depth):Json("all")},
        {"preview_title",policy.native?Json(nullptr):Json(utf8(title.text))}};
}
Json application_action(const fs::path& root,const Json& request) {
    auto action=request.at("action").get<std::string>();
    auto response=[](Json result,int code=0){return Json{{"code",code},{"result",std::move(result)}};};
    if(action=="symbols.status")return response(compat::status());
    if(action=="symbols.prepare") {auto result=compat::prepare(compat::cache_directory(),request.value("offline",false),request.value("force",false));auto code=result.value("prepared",false)?0:5;return response(std::move(result),code);}
    if(action=="agent.start")return response(start_observer_agent(root));
    if(action=="agent.status")return response(observer_status(root));
    if(action=="config.show")return response({{"text",config_toml(ConfigStore(root).load())},{"config_file",utf8(ConfigStore(root).path().wstring())}});
    if(action=="status")return response({{"version",TABTAIL_VERSION},{"executable",utf8(executable_path().wstring())},
        {"config_file",utf8(ConfigStore(root).path().wstring())},{"config",config_json(ConfigStore(root).load())},
        {"path",path_status(data_directory())},{"observer",observer_status(root)},{"explorer",explorer_status()}});
    if(action=="path.status")return response(path_status(data_directory()));
    if(action=="appearance.status")return response(observer_status(root).value("layout",Json{{"layout_applied",false}}));
    if(action=="install"||action=="uninstall"||action=="path.add"||action=="path.remove"){
        auto actual=fs::absolute(root).lexically_normal().wstring(),standard=fs::absolute(data_directory()).lexically_normal().wstring();
        if(_wcsicmp(actual.c_str(),standard.c_str())!=0)throw Error(2,"Installation and PATH changes require the standard user data directory");
        if(action=="install")return response(install(data_directory()));
        if(action=="uninstall"){auto result=uninstall(data_directory(),request.value("purge",false));return response(result,result.value("complete",false)?0:5);}
        auto result=manage_path(data_directory(),action=="path.add");return response(result,result.value("changed",false)&&!result.value("notified",false)?5:0);
    }
    if(action=="style.read"){
        auto file=fs::path(wide(request.at("file").get<std::string>()));
        if(!fs::is_regular_file(file)||fs::file_size(file)>200000)throw Error(2,"Choose a style JSON file no larger than 200000 bytes");
        auto bytes=read_file(file);if(bytes.size()>200000)throw Error(2,"Style file exceeds 200000 bytes");
        auto desired=Json::parse(bytes);auto features=ConfigStore(root).load().features;features["tab_style"]=desired;validate_features(features);
        return response({{"style",desired},{"expected",request.at("expected")},{"file",utf8(fs::absolute(file).wstring())}});
    }
    if(action=="snapshot") {
        int code=0;Json state;
        if(request.value("connect",false)) start_observer_agent(root);
        state=observer_status(root);
        if(state.value("running",false)) state=require_agent_result(root,{{"command","tabs.list"}},code);
        return response({{"config",config_json(ConfigStore(root).load())},{"agent",state}},code);
    }
    if(action=="start"||action=="stop") {
        if(action=="start") start_observer_agent(root);
        else if(!observer_status(root).value("running",false)) {
            ConfigStore(root).update([](Config& c){c.enabled=false;});return response({{"stopped",true}});
        }
        int code=0;auto result=require_agent_result(root,{{"command",action=="start"?"enable":"disable"}},code);return response(result,result.value("partial",false)?5:code);
    }
    if(action=="directories.open"){int code=0;auto result=require_agent_result(root,{{"command",action},{"id",request.at("id")},{"foreground",request.value("foreground",reinterpret_cast<uint64_t>(GetForegroundWindow()))}},code);return response(result,code);}
    if(action.starts_with("workspace.")){int code=0;auto result=require_agent_result(root,{{"command",action}},code);return response(result,result.contains("complete")&&!result.at("complete").get<bool>()?5:code);}
    if(action=="tabs.batch") {
        auto ids=request.at("tab_ids");if(ids.empty())return response({{"complete",true},{"results",Json::array()}});
        auto id=ids[0].get<std::string>();auto split=id.find('.');if(split==std::string::npos)throw Error(2,"Invalid tab ID");
        auto message=request;message.erase("action");message["command"]=action;message["runtime_id"]=id.substr(0,split);int code=0;
        auto result=require_agent_result(root,message,code);return response(result,result.value("complete",false)?code:5);
    }
    if(action.starts_with("tabs.")) {
        auto id=request.at("tab_id").get<std::string>();auto split=id.find('.');
        if(split==std::string::npos)throw Error(2,"Invalid tab ID");
        auto message=request;message.erase("action");message["command"]=action;message["runtime_id"]=id.substr(0,split);
        int code=0;auto result=require_agent_result(root,message,code);return response(result,result.value("partial",false)?5:code);
    }
    if(action=="features.save") {
        auto group=request.at("group").get<std::string>();
        if(!Config{}.features.contains(group))throw Error(2,"Unknown feature group");
        Config updated;
        try {updated=ConfigStore(root).update([&](Config& c){
            auto desired=request.at("value");auto expected=request.at("expected");
            if(c.features.at(group)!=expected&&c.features.at(group)!=desired )throw AppearanceConflict{};
            c.features[group]=desired;
        });}catch(const AppearanceConflict&){return response({{"saved",false},{"conflict",group}},2);}
        int code=0;Json result{{"saved",true},{"revision",updated.revision}};
        if(group=="restore")try{result["startup"]=manage_restore_startup(root,updated.features.at("restore").at("automatic"));}catch(const std::exception& ex){code=5;result["startup_error"]=ex.what();}
        try{if(auto peer=agent_request(root,{{"command","reload"},{"config_only",true}})){result["agent"]=peer->at("result");if(peer->at("code").get<int>())code=peer->at("code").get<int>();}}
        catch(const std::exception& ex){result["reload_error"]=ex.what();code=5;}
        if(result.contains("agent")) {
            auto agent=result.at("agent");
            if(group=="directories"||group=="shortcuts") {
                auto registration=agent.value(group=="directories"?"directory_shortcuts":"shortcuts",Json::object());
                result["shortcut_registration"]=registration;
                if(!registration.value("errors",Json::object()).empty()){
                    code=5;result["partial"]=true;result["message"]=updated.language=="en"?"Shortcut saved, but registration failed; see shortcut_registration.errors.":"快捷键已保存，但注册失败，请查看 shortcut_registration.errors。";
                }
            }
            if(group=="tab_behavior"&&agent.value("mode",std::string{})=="customize"&&!agent.value("features",Json::object()).value("complete",false)){code=5;result["partial"]=true;}
        }
        return response(result,code);
    }
    if(action=="tab.set"||action=="tab.reset") {
        auto id=request.at("tab_id").get<std::string>();auto split=id.find('.');
        if(split==std::string::npos) throw Error(2,"Invalid tab ID");
        Json message{{"command",action=="tab.set"?"tabs.set":"tabs.reset"},{"runtime_id",id.substr(0,split)},{"tab_id",id}};
        if(action=="tab.set") message["depth"]=request.at("depth");
        int code=0;auto result=require_agent_result(root,message,code);return response(result,result.value("partial",false)?5:code);
    }
    if(action=="test") return response(test_rules(ConfigStore(root).load(),wide(request.at("path").get<std::string>())));
    if(action=="doctor") {auto result=system_diagnostics();result["path"]=path_status(data_directory());result["observer"]=observer_status(root);return response(result);}
    if(action=="restart") return response(restart_explorer(true));
    if(action=="agent.stop") return response(stop_observer_agent(root));
    Config updated;
    try { updated=ConfigStore(root).update([&](Config& c){
        if(action=="appearance"&&request.contains("expected_tabs")) {
            auto expected=appearance_from_json(request.at("expected_tabs"));
            auto desired=appearance_from_json(request.at("tabs"));
            // Compare the edited section under the file lock. An unrelated
            // language, depth, rule or enable update must not invalidate it.
            if(c.tabs!=expected&&c.tabs!=desired)throw AppearanceConflict{};
            c.tabs=std::move(desired);return;
        }
        if(request.contains("revision")&&request.at("revision").get<int64_t>()!=c.revision) throw Error(2,"Configuration changed. Refresh before editing again.");
        if(action=="language") {c.language=request.at("language").get<std::string>();return;}
        if(action=="display") {c.depth=parse_depth(request.at("depth").get<std::string>());c.separator=wide(request.at("separator").get<std::string>());return;}
        if(action=="display.reset"){c.depth=2;c.separator=L"\\";return;}
        if(action=="appearance") {c.tabs=appearance_from_json(request.at("tabs"));return;}
        auto id=request.value("id",std::string{});
        auto it=std::find_if(c.rules.begin(),c.rules.end(),[&](const auto& r){return r.id==id;});
        if(action=="rule.put") {
            auto rule=rule_from_json(request.at("rule"));
            if(rule.id!=id) throw Error(2,"Rule ID does not match edit target");
            if(it==c.rules.end()) {
                if(request.value("replace",false)) throw Error(2,"Rule no longer exists. Refresh before editing again.");
                c.rules.push_back(std::move(rule));
            }
            else {if(!request.value("replace",false)) throw Error(2,"Rule ID already exists");*it=std::move(rule);}
            return;
        }
        if(it==c.rules.end()) throw Error(2,"Rule not found");
        if(action=="rule.delete") c.rules.erase(it);
        else if(action=="rule.toggle") it->enabled=!it->enabled;
        else if(action=="rule.move") {
            auto destination=request.at("position").get<int>();
            if(destination<1||destination>static_cast<int>(c.rules.size())) throw Error(2,"Rule position is out of range");
            auto rule=*it;c.rules.erase(it);c.rules.insert(c.rules.begin()+destination-1,std::move(rule));
        } else throw Error(2,"Unknown application action");
    }); } catch(const AppearanceConflict&) {
        return response({{"saved",false},{"conflict","appearance"}},2);
    }
    Json result{{"saved",true},{"revision",updated.revision},{"runtime_applied",false}};
    if(action=="language") return response(result); // No explicit title refresh on language changes.
    try {
        if(auto peer=agent_request(root,{{"command","reload"}})) {
            result["agent"]=peer->at("result");result["runtime_applied"]=peer->at("result").value("title_applied",false);
            result["layout"]=peer->at("result").value("layout",Json::object());
            if(action=="appearance"&&peer->at("code").get<int>()==0) {
                const auto& layout=result.at("layout");
                if(!layout.value("complete",false)||(updated.tabs.width_mode!="system"&&!layout.value("layout_applied",false)))
                    return response(result,5);
            }
            return response(result,peer->at("code").get<int>());
        }
    }catch(const std::exception& error) {result["reload_error"]=error.what();return response(result,5);}
    return response(result);
}
}
