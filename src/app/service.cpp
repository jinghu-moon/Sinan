#include "app/service.hpp"
#include "platform/everything.hpp"
#include "app/actions.hpp"
#include "client/agent.hpp"
#include "formatter/formatter.hpp"
#include "platform/explorer.hpp"
#include "installer/startup.hpp"
#include "installer/install.hpp"
#include "compat/symbols.hpp"
#include "platform/shell_actions.hpp"
#include <algorithm>
#include <cwctype>

namespace sinan {
namespace {
struct AppearanceConflict {};
Json shortcut_listener(bool running, const Json& state) {
    const auto status=state.value("shortcuts",Json::object());
    return {{"running",running},{"hook_active",status.value("hook_active",false)},
        {"global_available",status.value("global",Json::object()).value("available",false)},
        {"errors",status.value("errors",Json::object())}};
}
Json directory_entry_settings(const Config& config) {
    auto entries=Json::array();
    for(const auto& entry:entries_from_json(config.features.at("directories").at("entries")))
        entries.push_back(entry_json(entry));
    return {{"entries", std::move(entries)},
        {"pinned_position", config.features.at("tab_behavior").at("pinned_position")}};
}
Json directory_rules_snapshot(const Config& config) {
    auto rules=Json::array();
    for(const auto& rule:config.rules) rules.push_back(rule_json(rule));
    return {{"rules",std::move(rules)},{"rules_revision",std::to_string(config.revision)}};
}
Json tab_appearance_settings(const Config& config) {
    return {{"depth",config.depth ? std::to_string(*config.depth) : "all"},
        {"separator",utf8(config.separator)},{"tabs",appearance_json(config.tabs)}};
}
std::wstring fold_search(std::string_view text) {
    auto folded=wide(text);
    std::replace(folded.begin(),folded.end(),L'/',L'\\');
    if(!folded.empty())CharUpperBuffW(folded.data(),static_cast<DWORD>(folded.size()));
    return folded;
}
}
TabSearch::TabSearch(std::string_view query) {
    std::wstring term;
    for(auto c:fold_search(query)) {
        if(!std::iswspace(c)&&c!=L'\u3000')term+=c;
        else if(!term.empty()){terms_.push_back(std::move(term));term.clear();}
    }
    if(!term.empty())terms_.push_back(std::move(term));
}
bool TabSearch::matches(const Json& tab) const {
    if(terms_.empty())return true;
    std::vector<std::wstring> fields;
    for(auto key:{"path","original_title","preview_title","applied_title","tab_id"}) {
        auto field=tab.find(key);
        if(field!=tab.end()&&field->is_string())fields.push_back(fold_search(field->get_ref<const std::string&>()));
    }
    return std::all_of(terms_.begin(),terms_.end(),[&](const auto& term){
        return std::any_of(fields.begin(),fields.end(),[&](const auto& field){return field.find(term)!=field.npos;});
    });
}
Json test_rules(const Config& config,const std::wstring& path) {
    RuleSet rules(config.rules);auto policy=rules.resolve(config.display_options(),{},path);
    auto title=policy.native?Title{path}:format_title(path,policy.display);
    return {{"path",utf8(path)},{"source",policy.source==PolicySource::rule?"rule":"global"},
        {"rule_id",policy.rule_id},{"matched_pattern",utf8(policy.matched_pattern)},
        {"action",policy.native?"native":"depth"},{"depth",policy.display.depth?Json(*policy.display.depth):Json("all")},
        {"preview_title",policy.native?Json(nullptr):Json(utf8(title.text))}};
}
static Json application_action_impl(const fs::path& root,const Json& request) {
    auto action=request.at("action").get<std::string>();
    auto response=[](Json result,int code=0){return Json{{"code",code},{"result",std::move(result)}};};
    if(action=="actions.list") {
        const auto config=ConfigStore(root).load();
        return response(action_catalog(config.features.at("scripts").at("entries"),request.value("query",std::string{})));
    }
    if(action=="actions.execute")return execute_action(root,request);
    if(action=="config.get")return response(config_json(ConfigStore(root).load()));
    if(action=="rules.get")return response(directory_rules_snapshot(ConfigStore(root).load()));
    if(action=="entries.settings.get")return response(directory_entry_settings(ConfigStore(root).load()));
    if(action=="shortcuts.settings.get") {
        const auto config=ConfigStore(root).load();
        Json listener;
        try {
            const auto agent=observer_status(root);
            listener=shortcut_listener(agent.value("running",false),agent);
        } catch(const std::exception& error) {
            listener={{"error",error.what()}};
        }
        return response({{"shortcut_settings",config.features.at("shortcuts")},{"shortcut_listener",listener}});
    }
    if(action=="preview"){
        auto c=ConfigStore(root).load();
        if(request.contains("depth"))c.depth=parse_depth(request.at("depth").get<std::string>());
        if(request.contains("separator"))c.separator=wide(request.at("separator").get<std::string>());
        validate_config(c);auto title=format_title(wide(request.at("path").get<std::string>()),c.display_options());
        return response({{"title",utf8(title.text)},{"filesystem",title.filesystem},{"root",title.root}});
    }
    if(action=="appearance.get")return response(appearance_json(ConfigStore(root).load().tabs));
    if(action=="appearance.settings.get")return response(tab_appearance_settings(ConfigStore(root).load()));
    if(action=="command-bar.get") {
        const auto settings=ConfigStore(root).load().command_bar;
        const auto state=observer_status(root).value("command_bar",Json{{"state",settings.enabled?"waiting":"disabled"}});
        return response({{"command_bar",command_bar_json(settings)},{"command_bar_application",state}});
    }
    if(action=="command-bar.save") {
        const auto settings=command_bar_from_json(request.at("settings"));
        const auto expected=command_bar_from_json(request.at("expected"));
        ConfigStore(root).update([&](Config& config) {
            if(config.command_bar!=expected&&config.command_bar!=settings)
                throw Error(2,"命令栏配置已变更，请刷新后重试。");
            config.command_bar=settings;
        });
        Json state={{"state",settings.enabled?"waiting":"disabled"}};
        Json result={{"saved",true},{"command_bar",command_bar_json(settings)}};
        try {
            if(settings.enabled)start_observer_agent(root);
            if(auto peer=agent_request(root,{{"command","reload"},{"config_only",true}})) {
                if(peer->at("code")!=0)throw Error(5,peer->at("result").value("message","Agent could not load command bar settings"));
                state=peer->at("result").value("command_bar",state);
            }
        }catch(const std::exception& error) {state={{"state","unavailable"},{"errors",Json::array({error.what()})}};}
        result["command_bar_application"]=state;
        return response(result,state.value("state",std::string{})=="unavailable"?5:0);
    }
    if(action=="features.get"){
        auto features=ConfigStore(root).load().features;auto group=request.at("group").get<std::string>();
        if(!features.contains(group))throw Error(2,"Unknown feature group");return response(features.at(group));
    }
    if(action=="scripts.put"||action=="scripts.remove") {
        const auto config=ConfigStore(root).load();
        const auto& expected=config.features.at("scripts");
        auto desired=expected;
        const auto id=request.at("id").get<std::string>();
        auto& entries=desired.at("entries");
        const auto existing=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return entry.at("id")==id;});
        if(action=="scripts.remove") {
            if(existing==entries.end())throw Error(2,"Unknown PowerShell action");
            entries.erase(existing);
        } else {
            if(existing!=entries.end()&&!request.value("replace",false))throw Error(2,"Script ID exists; use --replace");
            Json entry{{"id",id},{"path",request.at("path")},{"name",request.at("name")},
                {"scope",request.value("scope",std::string("global"))},{"shortcut",request.value("shortcut",std::string{})}};
            if(existing==entries.end())entries.push_back(std::move(entry));
            else *existing=std::move(entry);
        }
        return application_action(root,{{"action","features.save"},{"group","scripts"},{"expected",expected},{"value",desired}});
    }
    if(action=="scripts.run") {
        Json message{{"command",action},{"id",request.at("id").get<std::string>()},
            {"foreground",reinterpret_cast<uint64_t>(GetForegroundWindow())}};
        if(request.contains("tab_id"))message["tab_id"]=request.at("tab_id").get<std::string>();
        int code=0;
        auto result=require_agent_result(root,std::move(message),code);
        return response(std::move(result),code);
    }
    if(action=="entries.put"||action=="entries.remove"||action=="shortcuts.set"||action=="pinned.position"||action=="restore.automatic"||action=="style.load"){
        std::string group=action.starts_with("entries.")?"directories":action=="shortcuts.set"?"shortcuts":action=="pinned.position"?"tab_behavior":action=="restore.automatic"?"restore":"tab_style";
        auto c=ConfigStore(root).load();auto desired=c.features.at(group);
        const auto entry_settings=request.contains("expected_entry")||request.contains("expected_position");
        if(action=="entries.put"||action=="entries.remove"){
            auto id=request.at("id").get<std::string>();auto& rows=desired["entries"];
            auto it=std::find_if(rows.begin(),rows.end(),[&](auto& e){return e.at("id")==id;});
            if(request.contains("expected_entry")) {
                const auto current=it==rows.end()?Json(nullptr):entry_json(entries_from_json(Json::array({*it})).front());
                if(current!=request.at("expected_entry"))return response({{"saved",false},{"conflict",group}},2);
            }
            if(action=="entries.remove"){if(it==rows.end())throw Error(2,"Unknown directory entry");rows.erase(it);}
            else {
                if(it!=rows.end()&&!request.value("replace",false))throw Error(2,"Entry ID exists; use --replace");
                Json entry{{"id",id},{"path",request.at("path")},{"name",request.at("name")},{"shortcut",request.value("shortcut",std::string{})},
                    {"title",request.value("title",std::string("alias_depth"))},{"enabled",request.value("enabled",true)},{"descendants",request.value("descendants",false)}};
                const auto scope=request.value("scope",it==rows.end()?std::string("global"):it->value("scope",std::string("global")));
                if(scope!="global")entry["scope"]=scope;
                if(it==rows.end())rows.push_back(entry);else *it=std::move(entry);
            }
        }else if(action=="shortcuts.set"){
            auto id=request.at("id").get<std::string>();
            if(std::find(explorer_shortcut_keys.begin(),explorer_shortcut_keys.end(),id)==explorer_shortcut_keys.end())throw Error(2,"Unknown shortcut action");
            if((request.contains("expected_chord")&&desired.at(id)!=request.at("expected_chord"))||
               (request.contains("expected_name")&&desired.at("new_folder_name")!=request.at("expected_name")))
                return response({{"saved",false},{"conflict",group}},2);
            auto chord=request.at("chord").get<std::string>();parse_shortcut(chord);desired[id]=chord;
            if(request.contains("name")) {
                if(id!="new_folder")throw Error(2,"Only new_folder accepts a preset name");
                desired["new_folder_name"]=request.at("name").get<std::string>();
            }
        }else if(action=="pinned.position"){
            if(request.contains("expected_position")&&desired.at("pinned_position")!=request.at("expected_position"))
                return response({{"saved",false},{"conflict",group}},2);
            desired["pinned_position"]=request.at("position");
        }
        else if(action=="restore.automatic")desired["automatic"]=request.at("enabled");
        else {auto r=application_action(root,{{"action","style.read"},{"file",request.at("file")},{"expected",desired}});desired=r.at("result").at("style");}
        return application_action(root,{{"action","features.save"},{"group",group},
            {"expected",c.features.at(group)},{"value",desired},{"entry_settings",entry_settings},
            {"shortcut_settings",action=="shortcuts.set"&&request.contains("expected_chord")}});
    }
    if(action=="tabs.settings.get") {
        auto state=observer_status(root);
        if(!state.value("running",false))
            return response({{"tab_list",{{"running",false},{"tabs",Json::array()}}}});
        int code=0;
        state=require_agent_result(root,{{"command","tabs.list"}},code);
        if(code!=0)return response(std::move(state),code);
        return response({{"tab_list",{
            {"running",true},{"mode",state.at("mode")},{"scan_complete",state.value("scan_complete",false)},
            {"config_error",state.value("config_error",std::string{})},{"tabs",std::move(state.at("tabs"))}
        }}});
    }
    if(action=="tabs.list") {
        auto query=request.find("query");
        std::optional<TabSearch> search;
        if(query!=request.end())search.emplace(query->get_ref<const std::string&>());
        int code=0;
        auto result=require_agent_result(root,{{"command","tabs.list"}},code);
        if(search&&result.contains("tabs")) {
            auto& tabs=result.at("tabs");
            auto matches=Json::array();
            for(auto& tab:tabs)if(search->matches(tab))matches.push_back(std::move(tab));
            tabs=std::move(matches);
            result["match_count"]=tabs.size();
        }
        return response(result,code);
    }
    if(action=="symbols.status")return response(compat::status(request.contains("cache_dir")?fs::absolute(wide(request.at("cache_dir").get<std::string>())):compat::cache_directory()));
    if(action=="symbols.prepare") {auto result=compat::prepare(request.contains("cache_dir")?fs::absolute(wide(request.at("cache_dir").get<std::string>())):compat::cache_directory(),request.value("offline",false),request.value("force",false));auto code=result.value("prepared",false)?0:5;return response(std::move(result),code);}
    if(action=="agent.start")return response(start_observer_agent(root));
    if(action=="agent.status")return response(observer_status(root));
    if(action=="general.get") {
        const auto config=ConfigStore(root).load();
        Json state{
            {"data_directory",utf8(root.wstring())},
            {"autostart",config.autostart},
            {"show_tray_icon",config.show_tray_icon},
            {"double_click_parent",config.double_click_parent},
            {"external_tabs",config.external_tabs},
            {"everything_search",config.everything_search},
            {"everything_search_scope",config.everything_search_all_drives?"all-drives":"current-folder"},
            {"search_box_hidden",config.search_box_hidden},
            {"folder_sizes",folder_sizes_json(config.folder_sizes,true)},
            {"selection_summary",selection_summary_json(config.selection_summary)},
        };
        try {
            const auto agent=observer_status(root);
            state["agent"]={
                {"running",agent.value("running",false)},
                {"mode",agent.value("mode",std::string("observe"))},
                {"runtime_applied",agent.value("runtime_applied",false)},
                {"hook_available",agent.value("hook_available",false)},
                {"tab_count",agent.value("tab_count",0)},
            };
            for (const auto* key : {"tray","double_click_parent","external_tabs","everything_search","folder_sizes"})
                if(agent.contains(key))state["agent"][key]=agent.at(key);
        } catch(const std::exception& error) {
            state["agent"]={{"error",error.what()}};
        }
        try {
            state["startup"]=login_startup_status(root,config);
        } catch(const std::exception& error) {
            state["startup"]={{"error",error.what()}};
        }
        return response(std::move(state));
    }
    if(action=="startup.status")return response(login_startup_status(root,ConfigStore(root).load()));
    if(action=="startup.set"){
        const auto enabled=request.at("enabled").get<bool>();
        auto config=ConfigStore(root).update([enabled](Config& c){c.autostart=enabled;});
        try{return response(sync_login_startup(root));}
        catch(const std::exception& ex){
            return response({{"enabled",config.autostart},{"message",ex.what()},{"saved",true}},5);
        }
    }
    if(action=="tray.status"){
        const auto enabled=ConfigStore(root).load().show_tray_icon;
        const auto agent=observer_status(root);
        auto result=agent.value("tray",Json{{"registered",false},{"error",""}});
        result["enabled"]=enabled;
        result["running"]=agent.value("running",false);
        return response(result);
    }
    if(action=="tray.set"){
        const auto enabled=request.at("enabled").get<bool>();
        const auto updated=ConfigStore(root).update([enabled](Config& c){c.show_tray_icon=enabled;});
        Json result{{"enabled",enabled},{"saved",true},{"revision",updated.revision}};
        try {
            const auto peer=agent_request(root,{{"command","reload"},{"config_only",true}});
            if(!peer)return response(result);
            const auto& state=peer->at("result");
            if(!state.contains("tray")){
                if(state.value("config_received",false)||!observer_status(root).contains("tray"))
                    result["restart_required"]=true;
                else
                    result["error"]=state.value("message",std::string{});
                return response(result,5);
            }
            const auto& tray=state.at("tray");
            result.update(tray);
            return response(result,tray.value("registered",false)==enabled?0:5);
        } catch(const std::exception& e) {
            result["error"]=e.what();
            return response(result,5);
        }
    }
    if(action=="config.show")return response({{"text",config_toml(ConfigStore(root).load())},{"config_file",utf8(ConfigStore(root).path().wstring())}});
    if(action=="status"){
        auto observer=observer_status(root);
        return response({{"schema_version",1},{"name","Sinan"},{"version",SINAN_VERSION},{"executable",utf8(executable_path().wstring())},
            {"config_file",utf8(ConfigStore(root).path().wstring())},{"config",config_json(ConfigStore(root).load())},
            {"path",path_status(data_directory())},{"observer",observer},{"explorer",explorer_status()},
            {"agent_running",observer.value("running",false)},{"hook_available",observer.value("hook_available",false)},{"runtime_applied",observer.value("runtime_applied",false)}});
    }
    if(action=="path.status")return response(path_status(data_directory()));
    if(action=="appearance.status")return response(observer_status(root).value("layout",Json{{"layout_applied",false}}));
    if(action=="selection-summary.set") {
        const auto settings=selection_summary_from_json(request.at("settings"));
        const auto updated=ConfigStore(root).update([&](Config& c){c.selection_summary=settings;});
        Json result{{"saved",true},{"revision",updated.revision}};
        try {
            if(settings.show_files || settings.show_folders || settings.show_size) start_observer_agent(root);
            if(auto peer=agent_request(root,{{"command","reload"},{"config_only",true}}))
                return response(result,peer->at("code").get<int>());
        } catch(const std::exception& error) {
            result["error"]=error.what();
            return response(result,5);
        }
        return response(result);
    }
    if(action=="folder-sizes.set") {
        const auto settings=folder_sizes_from_json(request.at("settings"));
        const auto updated=ConfigStore(root).update([&](Config& c){c.folder_sizes=settings;});
        Json result{{"saved",true},{"revision",updated.revision}};
        try {
            if(settings.enabled)start_observer_agent(root);
            if(auto peer=agent_request(root,{{"command","reload"},{"config_only",true}}))
                return response(result,peer->at("code").get<int>());
        } catch(const std::exception& error) {
            result["error"]=error.what();
            return response(result,5);
        }
        return response(result);
    }
    if(action=="everything-search.query") {
        const auto query=wide(request.at("query").get<std::string>());
        everything_search_group(query);
        return response(query_everything(wide(request.at("directory").get<std::string>()),query,request.value("limit",100u)));
    }
    if(action=="everything-search.scope") {
        const auto scope=request.at("scope").get<std::string>();
        if(scope!="all-drives"&&scope!="current-folder") throw Error(2,"无效的 Everything 搜索范围");
        ConfigStore(root).update([&](Config& c){c.everything_search_all_drives=scope=="all-drives";});
        Json result{{"saved",true},{"scope",scope}};
        try {
            if(auto peer=agent_request(root,{{"command","reload"},{"config_only",true}}))
                return response(result,peer->at("code").get<int>());
        } catch(const std::exception& error) {
            result["error"]=error.what();
            return response(result,5);
        }
        return response(result);
    }
    if(action=="double-click-parent.status"||action=="double-click-parent.set"||
        action=="everything-search.status"||action=="everything-search.set"||
        action=="external-tabs.status"||action=="external-tabs.set") {
        const bool everything=action.starts_with("everything");
        const bool external=action.starts_with("external-tabs");
        const auto field=everything?"everything_search":external?"external_tabs":"double_click_parent";
        const auto option=everything?&Config::everything_search:external?&Config::external_tabs:&Config::double_click_parent;
        if(action.ends_with(".set")) {
            const auto enabled=request.at("enabled").get<bool>();
            ConfigStore(root).update([&](Config& c){c.*option=enabled;});
            if(enabled)start_observer_agent(root);
            if(auto peer=agent_request(root,{{"command","reload"},{"config_only",true}})) {
                auto listener=peer->at("result").value(field,Json::object());
                auto code=peer->at("code").get<int>();
                if(!everything&&listener.value("active",false)!=enabled&&code==0&&
                    !(external&&listener.value("pending",false)))code=5;
                return response({{"enabled",enabled},{"listener",listener}},code);
            }
        }
        auto state=observer_status(root);
        const auto config = ConfigStore(root).load();
        Json result{{"enabled",config.*option},{"listener",state.value(field,Json{{"active",false}})}};
        if (everything) result["scope"] = config.everything_search_all_drives ? "all-drives" : "current-folder";
        return response(result);
    }
    if(action=="search-box.status") {
        const auto state=observer_status(root);
        return response({{"configured_hidden",ConfigStore(root).load().search_box_hidden},
            {"agent_running",state.value("running",false)},{"layout",state.value("layout",Json::object())}});
    }
    if(action=="search-box.set") {
        const auto hidden=request.at("hidden").get<bool>();
        auto updated=ConfigStore(root).update([&](Config& c){c.search_box_hidden=hidden;});
        Json result{{"saved",true},{"configured_hidden",hidden},{"revision",updated.revision},{"confirmed",false}};
        try {
            if(hidden&&request.value("connect",false))start_observer_agent(root);
            if(auto peer=agent_request(root,{{"command","reload"},{"config_only",true}})) {
                auto layout=peer->at("result").value("layout",Json::object());
                result["confirmed"]=layout.value("complete",false)&&(!hidden||layout.value("search_box_hidden",false));
                result["layout"]=std::move(layout);
                return response(result,result.at("confirmed").get<bool>()?0:5);
            }
        }catch(const std::exception& error) {
            result["reload_error"]=error.what();return response(result,5);
        }
        return response(result);
    }
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
        const auto config=ConfigStore(root).load();
        Json startup;
        try{startup=login_startup_status(root,config);}
        catch(const std::exception& ex){startup={{"error",ex.what()}};}
        return response({{"config",config_json(config)},{"agent",state},{"startup",std::move(startup)}},code);
    }
    if(action=="start"||action=="stop") {
        if(action=="start") start_observer_agent(root);
        else if(!observer_status(root).value("running",false)) {
            ConfigStore(root).update([](Config& c){c.enabled=false;});return response({{"stopped",true}});
        }
        int code=0;auto result=require_agent_result(root,{{"command",action=="start"?"enable":"disable"}},code);return response(result,result.value("partial",false)?5:code);
    }
    if(action=="directories.open"){int code=0;auto result=require_agent_result(root,{{"command",action},{"id",request.at("id").get<std::string>()},{"foreground",reinterpret_cast<uint64_t>(GetForegroundWindow())}},code);return response(result,code);}
    if(action=="archives.jobs"||action=="archives.open"||action=="archives.cancel") {
        auto message=request;message.erase("action");message["command"]=action;
        int code=0;auto result=require_agent_result(root,message,code);return response(result,code);
    }
    if(action=="explorer.toggle_file_extensions")return response(toggle_file_extensions());
    if(action.starts_with("explorer.")) {
        Json message{{"command",action},{"foreground",reinterpret_cast<uint64_t>(GetForegroundWindow())}};
        if(action=="explorer.search_everything")message["query"]=request.at("query").get<std::string>();
        if(action=="explorer.new_folder") {
            if(request.contains("parent"))
                return response(create_named_folder(wide(request.at("parent").get<std::string>()),wide(request.at("name").get<std::string>())));
            if(request.contains("name")) {
                auto name=request.at("name").get<std::string>();
                validate_folder_name(wide(name));
                message["name"]=name;
            }
        }
        if(action!="explorer.locate_clipboard"&&request.contains("tab_id"))message["tab_id"]=request.at("tab_id").get<std::string>();
        int code=0;
        auto result=require_agent_result(root,message,code);
        return response(result,result.value("partial",false)?5:code);
    }
    if(action.starts_with("workspace.")){int code=0;auto result=require_agent_result(root,{{"command",action}},code);return response(result,result.contains("complete")&&!result.at("complete").get<bool>()?5:code);}
    if(action=="tabs.batch") {
        auto ids=request.at("tab_ids");if(ids.empty())return response({{"complete",true},{"results",Json::array()}});
        auto id=ids[0].get<std::string>();auto split=id.find('.');if(split==std::string::npos||split==0||split+1==id.size())throw Error(2,"Invalid tab ID");
        auto message=request;message.erase("action");message["command"]=action;message["runtime_id"]=id.substr(0,split);int code=0;
        auto result=require_agent_result(root,message,code);return response(result,result.value("complete",false)?code:5);
    }
    if(action.starts_with("tabs.")) {
        auto id=request.at("tab_id").get<std::string>();auto split=id.find('.');
        if(split==std::string::npos||split==0||split+1==id.size())throw Error(2,"Invalid tab ID");
        if(action=="tabs.set")parse_depth(request.at("depth").get<std::string>());
        if(action=="tabs.style")validate_style(request.at("style"));
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
        if(request.value("entry_settings",false))result.update(directory_entry_settings(updated));
        if(group=="restore")try{result["startup"]=sync_login_startup(root);}catch(const std::exception& ex){code=5;result["startup_error"]=ex.what();}
        try{if(auto peer=agent_request(root,{{"command","reload"},{"config_only",true}})){result["agent"]=peer->at("result");if(peer->at("code").get<int>()&&code==0)code=peer->at("code").get<int>();}}
        catch(const std::exception& ex){result["reload_error"]=ex.what();code=5;}
        if(result.contains("agent")) {
            auto agent=result.at("agent");
            if(group=="directories"||group=="shortcuts"||group=="scripts") {
                auto registration=agent.value(group=="directories"?"directory_shortcuts":"shortcuts",Json::object());
                if(group=="scripts"&&registration.contains("errors")) {
                    auto errors=Json::object();
                    for(const auto& [id,error]:registration.at("errors").items())
                        if(id.starts_with("script:"))errors[id.substr(7)]=error;
                    registration={{"errors",std::move(errors)}};
                }
                result["shortcut_registration"]=registration;
                if(!registration.value("errors",Json::object()).empty()){
                    code=5;result["partial"]=true;result["message"]=updated.language=="en"?"Shortcut saved, but registration failed; see shortcut_registration.errors.":"快捷键已保存，但注册失败，请查看 shortcut_registration.errors。";
                }
            }
            if(group=="tab_behavior"&&agent.value("mode",std::string{})=="customize"&&!agent.value("features",Json::object()).value("complete",false)){code=5;result["partial"]=true;}
        }
        if(group=="shortcuts"&&request.value("shortcut_settings",false)) {
            result["shortcut_settings"]=updated.features.at("shortcuts");
            result["shortcut_listener"]=result.contains("reload_error")
                ? Json{{"error",result.at("reload_error")}}
                : shortcut_listener(result.contains("agent"),result.value("agent",Json::object()));
        }
        return response(result,code);
    }
    if(action=="tab.set"||action=="tab.reset") {
        auto id=request.at("tab_id").get<std::string>();auto split=id.find('.');
        if(split==std::string::npos||split==0||split+1==id.size()) throw Error(2,"Invalid tab ID");
        Json message{{"command",action=="tab.set"?"tabs.set":"tabs.reset"},{"runtime_id",id.substr(0,split)},{"tab_id",id}};
        if(action=="tab.set") {parse_depth(request.at("depth").get<std::string>());message["depth"]=request.at("depth");}
        int code=0;auto result=require_agent_result(root,message,code);return response(result,result.value("partial",false)?5:code);
    }
    if(action=="test") return response(test_rules(ConfigStore(root).load(),wide(request.at("path").get<std::string>())));
    if(action=="doctor") {
        auto result=system_diagnostics();result["schema_version"]=1;result["path"]=path_status(data_directory());result["observer"]=observer_status(root);
        try{ConfigStore(root).load();result["config_valid"]=true;}catch(const std::exception& e){result["config_valid"]=false;result["config_error"]=e.what();}
        return response(result,result.at("config_valid").get<bool>()?0:2);
    }
    if(action=="restart"){if(!request.value("force",false))throw Error(3,"Explicit force required");return response(restart_explorer(true));}
    if(action=="agent.stop") return response(stop_observer_agent(root));
    const std::vector<std::string> mutations={"language","display","display.reset","config.set","appearance","appearance.patch",
        "appearance.settings.save","rule.put","rule.delete","rule.toggle","rule.move"};
    if(std::find(mutations.begin(),mutations.end(),action)==mutations.end())throw Error(2,"Unknown application action");
    Config updated;
    try { updated=ConfigStore(root).update([&](Config& c){
        if(action=="appearance.settings.save") {
            const auto& desired=request.at("settings");
            const auto current=tab_appearance_settings(c);
            if(current!=request.at("expected")&&current!=desired)throw AppearanceConflict{};
            c.depth=parse_depth(desired.at("depth").get<std::string>());
            c.separator=wide(desired.at("separator").get<std::string>());
            c.tabs=appearance_from_json(desired.at("tabs"));
            return;
        }
        if(action=="appearance"&&request.contains("expected_tabs")) {
            auto expected=appearance_from_json(request.at("expected_tabs"));
            auto desired=appearance_from_json(request.at("tabs"));
            // Compare the edited section under the file lock. An unrelated
            // language, depth, rule or enable update must not invalidate it.
            if(c.tabs!=expected&&c.tabs!=desired)throw AppearanceConflict{};
            c.tabs=std::move(desired);return;
        }
        if(action.starts_with("rule.")&&request.contains("rules_revision")&&
            request.at("rules_revision")!=std::to_string(c.revision))
            throw Error(2,"规则配置已更新，请重新载入后再操作。");
        if(request.contains("revision")&&request.at("revision").get<int64_t>()!=c.revision) throw Error(2,"Configuration changed. Refresh before editing again.");
        if(action=="config.set"){
            auto key=request.at("key").get<std::string>();auto value=request.at("value").get<std::string>();
            if(key=="depth")c.depth=parse_depth(value);else if(key=="separator")c.separator=wide(value);else throw Error(2,"Configuration key must be depth or separator");return;
        }
        if(action=="appearance.patch"){
            auto tabs=appearance_json(c.tabs);tabs["width_mode"]=request.at("mode");
            for(auto key:{"width","min_width","max_width"})if(request.contains(key))tabs[key]=request.at(key);
            c.tabs=appearance_from_json(tabs);return;
        }
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
    if(action.starts_with("rule.")&&request.contains("rules_revision"))
        result.update(directory_rules_snapshot(updated));
    if(action=="language") return response(result); // No explicit title refresh on language changes.
    try {
        if(auto peer=agent_request(root,{{"command","reload"}})) {
            result["agent"]=peer->at("result");result["runtime_applied"]=peer->at("result").value("title_applied",false);
            result["layout"]=peer->at("result").value("layout",Json::object());
            if((action=="appearance"||action=="appearance.patch"||action=="appearance.settings.save")&&peer->at("code").get<int>()==0) {
                const auto& layout=result.at("layout");
                if(!layout.value("complete",false)||(updated.tabs.width_mode!="system"&&!layout.value("layout_applied",false)))
                    return response(result,5);
            }
            return response(result,peer->at("code").get<int>());
        }
    }catch(const std::exception& error) {result["reload_error"]=error.what();return response(result,5);}
    return response(result);
}
Json application_action(const fs::path& root,const Json& request){
    try{return application_action_impl(root,request);}
    catch(const Json::exception& e){throw Error(2,std::string("Invalid application request: ")+e.what());}
    catch(const PolicyError& e){throw Error(2,e.what());}
}

}
