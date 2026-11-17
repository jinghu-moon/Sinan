#include "agent/runtime.hpp"
#include "agent/tab_tracker.hpp"
#include "agent/layout_tracker.hpp"
#include "agent/features.hpp"
#include "agent/interaction.hpp"
#include "agent/workspace.hpp"
#include "policy/entries.hpp"
#include "hook/controller.hpp"
#include <algorithm>
#include <limits>

namespace tabtail {
namespace {
std::string new_runtime_id() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw Error(1, "无法生成代理运行期 ID");
    wchar_t text[40]{}; StringFromGUID2(guid, text, 40);
    std::wstring token(text + 1, text + 37);
    return utf8(token);
}
Json required(const fs::path& root, Json request) {
    auto response = agent_request(root, std::move(request));
    if (!response) throw Error(3, "标签观察代理未运行；请先执行 tabtail agent start。");
    return *response;
}
void check(const Json& response) {
    auto code = response.at("code").get<int>();
    if (code) throw Error(code, response.at("result").value("message", "代理请求失败"));
}
}
int run_observer_agent(const fs::path& root) {
    auto endpoint = agent_endpoint(root);
    auto mutex_name = L"Local\\" + endpoint.substr(9);
    Handle singleton(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    if (!singleton) throw Error(1, system_error("创建代理单实例锁"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) throw Error(1, "当前数据目录的标签观察代理已运行");
    ShellTabEnumerator enumerator;
    auto runtime_id = new_runtime_id();
    ConfigStore store(root);
    auto config = store.load();
    ShellTabTracker tracker(runtime_id, config);
    LayoutTracker layouts;
    TabFeatures features;
    ExplorerShortcuts shortcuts;
    DirectoryShortcuts directory_shortcuts;
    Json last_directory_open=Json::object();
    SystemNotifications notifications;
    Json feature_state=Json::object();
    Json automatic_restore=Json::object();
    std::map<std::string,uint64_t> close_counts,navigation_counts,redirect_counts;
    PipeServer server(root);
    bool stopping = false;
    bool startup_restore_pending=config.enabled&&config.features.at("restore").value("automatic",false);
    bool active = false; // agent start always starts with observation only.
    Json application{{"applied_count", 0}, {"failed_count", 0}, {"runtime_applied", false}};
    Json layout_state{{"mode","system"},{"complete",true},{"layout_applied",false},{"threads",Json::array()}};
    struct RestoreOnExit {
        ShellTabTracker& tracker;
        LayoutTracker& layouts;
        TabFeatures& features;
        ~RestoreOnExit() { try {features.stop();}catch(...){} try { layouts.synchronize(tracker.native_tabs(),false,{}); } catch (...) {} try { tracker.synchronize_titles(false); } catch (...) {} }
    } restore_on_exit{tracker,layouts,features};
    auto synchronize=[&] {
        feature_state=features.synchronize(tracker,config,active);
        shortcuts.update(config.features.at("shortcuts"),tracker.native_tabs(),active);
        directory_shortcuts.update(config.features.at("directories").at("entries"));
        for(auto& t:features.list(tracker,config)) {
            auto id=t.at("tab_id").get<std::string>();auto actual=t.at("behavior").value("actual",Json::object());
            auto closes=actual.value("blocked_close",uint64_t{});auto navigations=actual.value("blocked_navigation",uint64_t{});
            if(closes>close_counts[id])notifications.show(config.features.at("notifications").at("mode"),config.language=="en"?"This tab is protected. Unpin and release independent protection before closing.":"此标签已保护，请先取消固定并解除独立保护。",id,true,id+"close");
            auto redirected=actual.value("redirected",uint64_t{});auto error=actual.value("error",std::string{});
            if(redirected>redirect_counts[id])notifications.show(config.features.at("notifications").at("mode"),config.language=="en"?"Navigation opened in a new tab; the locked directory was preserved.":"导航已在新标签中打开，原目录保持锁定。",id,false,id+"navigate");
            else if(navigations>navigation_counts[id]&&!error.empty())notifications.show(config.features.at("notifications").at("mode"),error,id,true,id+"navigate");
            close_counts[id]=closes;navigation_counts[id]=navigations;redirect_counts[id]=redirected;
        }
        application=tracker.synchronize_titles(active);
        layout_state=layouts.synchronize(tracker.native_tabs(),active,config.tabs);
    };
    std::string scan_error, config_error;
    std::vector<std::string> warnings;
    bool scan_complete = false;
    uint64_t last_scan = 0;
    auto scan = [&](bool include_native=false) {
        try {
            auto observation = enumerator.scan(include_native||active);
            tracker.reconcile(observation);
            scan_complete = observation.complete;
            warnings = std::move(observation.warnings);
            scan_error.clear();
        } catch (const std::exception& e) {
            scan_error = e.what(); scan_complete = false;
            // Failed COM enumeration must still retire confirmed dead Explorer lifetimes.
            tracker.reconcile({{}, false, {}});
        }
        last_scan = GetTickCount64();
    };
    auto reload = [&]() {
        try {
            auto next = store.load();
            tracker.apply_config(next);
            config = std::move(next);
            if(!config.enabled||!config.features.at("restore").value("automatic",false))startup_restore_pending=false;
            if (!config.enabled && active) {
                active = false; synchronize();
            }
            config_error.clear();
        } catch (const std::exception& e) { config_error = e.what(); }
    };
    scan();
    auto status = [&]() {
        return Json{{"running", true}, {"pid", GetCurrentProcessId()}, {"runtime_id", runtime_id}, {"mode", active ? "customize" : "observe"},
            {"version", TABTAIL_VERSION}, {"data_directory", utf8(root.wstring())}, {"config_revision", config.revision},
            {"directory_shortcuts",directory_shortcuts.status()},{"last_directory_open",last_directory_open},{"automatic_restore",automatic_restore},{"features",feature_state},{"shortcuts",shortcuts.status()},{"notifications",notifications.status()},{"layout",layout_state},
            {"hook_available", native_build_supported()}, {"runtime_applied", scan_complete && scan_error.empty() && config_error.empty() && application.value("runtime_applied", false)}, {"application", application}, {"scan_complete", scan_complete},
            {"scan_error", scan_error}, {"config_error", config_error}, {"warnings", warnings},
            {"scan_age_ms", GetTickCount64() - last_scan}, {"tab_count", tracker.list().size()}};
    };
    auto open_directory=[&](const std::string& id,HWND foreground)->Json {
        reload();scan(true);
        auto entries=entries_from_json(config.features.at("directories").at("entries"));
        auto entry=std::find_if(entries.begin(),entries.end(),[&](auto& e){return e.id==id&&e.enabled;});
        if(entry==entries.end())throw Error(2,"Directory entry is missing or disabled");
        if(!config_error.empty())throw Error(5,config_error);
        if(!scan_complete)throw Error(5,"Incomplete Explorer scan; directory opening cancelled to avoid duplicates");
        if(!workspace_directory_available(entry->path))throw Error(3,config.language=="en"?"Directory is missing, offline or did not respond within 800 ms":"目录不存在、已离线或未在 800 毫秒内响应");
        auto key=directory_key(entry->path);std::string match;std::optional<NativeTab> anchor;
        for(auto& t:tracker.list()) {
            auto tab_id=t.at("tab_id").get<std::string>();std::optional<NativeTab> native;
            try{native=tracker.native_tab(tab_id);}catch(...){}
            if(native&&native->window==foreground)anchor=native;
            if(t.at("path").is_string()&&directory_key(wide(t.at("path").get<std::string>()))==key&&(match.empty()||(native&&native->window==foreground)))match=tab_id;
        }
        if(!match.empty()) {
            auto result=features.change(tracker,config,{{"command","tabs.locate"},{"tab_id",match}});
            result["entry_id"]=id;result["reused"]=true;result["opened"]=false;return result;
        }
        if(anchor) {
            auto result=send_feature_request(*anchor,FeatureAction::open,{{"path",utf8(entry->path)},{"select",true}});
            result["entry_id"]=id;result["opened"]=true;result["reused"]=false;result["target"]="current_window";return result;
        }
        // No foreground Explorer frame: preserve its background windows.
        open_workspace_window(entry->path);
        return {{"entry_id",id},{"opened",true},{"reused",false},{"target","new_window"},{"confirmation","launch_requested"}};
    };
    auto handler = [&](const Json& request) -> Json {
        auto command = request.at("command").get<std::string>();
        if (command == "status") return status();
        if(command=="directories.open")return last_directory_open=open_directory(request.at("id").get<std::string>(),reinterpret_cast<HWND>(request.value("foreground",uint64_t{})));
        if (command == "stop") {
            active = false; synchronize(); stopping = true;
            return {{"stopping", true},{"partial",!feature_state.value("complete",false)||!application.value("complete",false)||!layout_state.value("complete",false)}, {"pid", GetCurrentProcessId()}, {"runtime_id", runtime_id}, {"restoration", application},{"feature_restoration",feature_state},{"layout_restoration",layout_state}};
        }
        if (command == "enable" || command == "disable") {
            if(command=="disable")startup_restore_pending=false;
            if (command == "enable" && !native_build_supported()) throw Error(3, "当前 ExplorerFrame 不在标题适配范围；请运行 doctor");
            config = store.update([&](Config& c) { c.enabled = command == "enable"; });
            tracker.apply_config(config); active = config.enabled; scan();
            synchronize();
            auto result = status();result["partial"]=!active&&(!feature_state.value("complete",false)||!layout_state.value("complete",false)||!application.value("complete",false)); result["title_applied"] = active ? result.at("runtime_applied") : application.at("complete");
            result["message"] = active ? (tracker.list().empty() ? "标题定制已启用，等待 Explorer 标签；尚无标题确认。" : "标题定制已启用；请通过 tabs list 检查逐标签结果。") : "标题定制已停止，已请求恢复 Shell 显示名。";
            return result;
        }
        if (command == "reload") {
            reload();
            if (!config_error.empty()) throw Error(5, "配置已保存，但代理未接收：" + config_error);
            synchronize();
            return {{"directory_shortcuts",directory_shortcuts.status()},{"config_received", true}, {"mode",active?"customize":"observe"}, {"revision", config.revision}, {"title_applied", active && application.value("complete", false)},
                {"layout",layout_state},{"layout_applied",layout_state.value("layout_applied",false)},
                {"application", application},{"features",feature_state}, {"message", active ? "代理已接收并应用默认配置；请检查逐标签结果。" : "代理已接收默认配置；观察模式下未应用标题。"}};
        }
        if(command=="tabs.batch") {
            if(!active)throw Error(3,"Start customization before batch changes");
            if(request.at("runtime_id").get<std::string>()!=runtime_id)throw Error(2,"Runtime changed; refresh tabs");
            auto action=request.at("operation").get<std::string>();if(action!="pin"&&action!="protect"&&action!="lock")throw Error(2,"Invalid batch operation");
            auto ids=request.at("tab_ids");if(!ids.is_array()||ids.size()>100)throw Error(2,"Batch limit is 100 tabs");
            scan();if(!scan_complete)throw Error(5,"Incomplete scan; batch cancelled");Json results=Json::array();auto deadline=GetTickCount64()+20000;auto renewed=GetTickCount64();
            for(auto& id:ids)try {if(GetTickCount64()>=deadline)throw Error(5,"Batch time limit reached");if(GetTickCount64()-renewed>2000){synchronize();renewed=GetTickCount64();}auto result=features.change(tracker,config,{{"command","tabs."+action},{"tab_id",id},{"enabled",request.at("enabled")}});results.push_back(result);}
            catch(const std::exception& ex){results.push_back({{"tab_id",id},{"state_confirmed",false},{"error",ex.what()}});}
            if(config.features.at("restore").value("automatic",false))save_workspace(root,features.list(tracker,config),config.features.at("restore").at("max_tabs"));
            return {{"results",results},{"complete",std::all_of(results.begin(),results.end(),[](auto& r){return r.value("state_confirmed",false)&&!r.value("partial",false);})}};
        }
        if(command=="workspace.save") {scan();if(!scan_complete)throw Error(5,"Incomplete scan; workspace not overwritten");return save_workspace(root,features.list(tracker,config),config.features.at("restore").at("max_tabs"));}
        if(command=="workspace.show")return {{"entries",load_workspace(root,config.features.at("restore").at("max_tabs"))}};
        if(command=="workspace.restore") {
            if(!active)throw Error(3,"Start customization before restoring workspace protection");
            auto entries=load_workspace(root,config.features.at("restore").at("max_tabs"));Json results=Json::array();auto deadline=GetTickCount64()+20000;auto renewed=GetTickCount64();
            // Saved groups map deterministically to live windows in ascending handle order.
            auto live=features.list(tracker,config);std::map<uint64_t,std::string> windows;
            for(auto& t:live)windows.emplace(t.at("window").get<uint64_t>(),t.at("tab_id").get<std::string>());
            std::vector<std::string> anchors;for(auto& [window,id]:windows)anchors.push_back(id);
            for(auto& entry:entries) {
                auto path=entry.at("path").get<std::string>();Json result{{"path",path},{"restored",false}};
                try {
                    if(GetTickCount64()-renewed>2000){synchronize();renewed=GetTickCount64();}
                    auto w=wide(path);if(GetTickCount64()>=deadline)throw Error(5,"Restore time limit reached; remaining directories were not opened");
                    if(!workspace_directory_available(w))throw Error(3,"Directory is missing, offline or did not respond within 800ms");
                    auto group=entry.at("window_group").get<size_t>();std::string target;
                    live=features.list(tracker,config);
                    // Dedupe by path within the mapped window, never by transient tab_id persisted on disk.
                    uint64_t target_window=0;if(!anchors.empty())target_window=reinterpret_cast<uint64_t>(tracker.native_tab(anchors[group%anchors.size()]).window);
                    for(auto& t:live)if(t.at("path").is_string()&&workspace_path_key(t.at("path").get<std::string>())==workspace_path_key(path)&&(!target_window||t.at("window")==target_window)){target=t.at("tab_id");break;}
                    if(target.empty()) {
                        if(anchors.empty()) {
                            open_workspace_window(w);
                            for(int retry=0;retry<30&&anchors.empty()&&GetTickCount64()<deadline;++retry){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(100);scan();for(auto& t:tracker.list())if(t.at("path").is_string()&&workspace_path_key(t.at("path").get<std::string>())==workspace_path_key(path)){target=t.at("tab_id");anchors.push_back(target);break;}}
                            if(anchors.empty())throw Error(5,"Explorer startup is pending; retry restore when the window is available");
                        }
                        if(target.empty()) {
                        auto anchor=tracker.native_tab(anchors[group%anchors.size()]);auto opened=send_feature_request(anchor,FeatureAction::open,{{"path",path}});
                        for(int retry=0;retry<20&&target.empty()&&GetTickCount64()<deadline;++retry){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(100);scan();
                        for(auto& t:tracker.list()) {auto native=tracker.native_tab(t.at("tab_id").get<std::string>());if(guid_text(native.guid)==opened.at("opened_guid").get<std::string>()){target=t.at("tab_id");break;}}
                        }
                        if(target.empty())throw Error(5,"Directory opened; tab observation is pending. Retry restore to attach protection");
                        }
                    }
                    for(auto action:{"pin","protect","lock"}) {
                        auto field=std::string_view(action)=="pin"?"pinned":std::string_view(action)=="protect"?"protected":"locked";
                        if(entry.at(field).get<bool>())features.change(tracker,config,{{"command",std::string("tabs.")+action},{"tab_id",target},{"enabled",true}});
                    }
                    if(!entry.at("style").empty())features.change(tracker,config,{{"command","tabs.style"},{"tab_id",target},{"style",entry.at("style")}});
                    result["restored"]=true;result["tab_id"]=target;
                }catch(const std::exception& ex){result["error"]=ex.what();}
                results.push_back(result);
            }
            return {{"results",results},{"complete",std::all_of(results.begin(),results.end(),[](auto& r){return r.at("restored").template get<bool>();})}};
        }
        if (command == "tabs.list" || command == "tabs.set" || command == "tabs.reset" || command == "tabs.probe" || command=="tabs.pin" || command=="tabs.protect" || command=="tabs.lock" || command=="tabs.style" || command=="tabs.style-reset" || command=="tabs.locate" || command=="tabs.capabilities" || command=="tabs.visual-probe") {
            reload(); scan();
            if (active) synchronize();
            if (!scan_error.empty()) throw Error(5, "本次标签扫描失败：" + scan_error);
            if (command == "tabs.list") {
                auto result = status(); result["tabs"] = features.list(tracker,config); return result;
            }
            if (request.at("runtime_id").get<std::string>() != runtime_id) throw Error(2, "代理运行期已变化，请重新执行 tabs list");
            auto id = request.at("tab_id").get<std::string>();
            const auto known = tracker.list();
            if (std::none_of(known.begin(), known.end(), [&](const auto& tab) { return tab.at("tab_id") == id; }))
                throw Error(2, "标签不存在、已关闭或属于其他代理运行期");
            if (!scan_complete) throw Error(5, "本次标签扫描不完整，未修改策略；请重新执行 tabs list 后重试。");
            if (!config_error.empty()) throw Error(5, "代理配置无效，未修改标签策略：" + config_error);
            if(command!="tabs.probe"&&command!="tabs.set"&&command!="tabs.reset") {
                if(command!="tabs.locate"&&command!="tabs.capabilities"&&command!="tabs.visual-probe"&&!active)throw Error(3,"Start customization before changing tab protection or appearance");
                try {
                    auto result=features.change(tracker,config,request);
                    if(command=="tabs.pin"||command=="tabs.protect"||command=="tabs.lock") {
                        std::string title;for(auto& t:tracker.list())if(t.at("tab_id")==id)title=t.value("preview_title",std::string{});
                        bool zh=config.language!="en";
                        auto action=command=="tabs.pin"?(zh?"固定":"pin"):command=="tabs.protect"?(zh?"独立保护":"protection"):(zh?"目录锁定":"directory lock");
                        auto text=zh?std::string(request.at("enabled").get<bool>()?"已启用":"已取消")+action+"："+title:std::string(action)+(request.at("enabled").get<bool>()?" enabled: ":" disabled: ")+title;
                        if(result.value("partial",false))text+=result.value("appearance_pending",false)?(zh?"；外观正在初始化，请稍候":"; appearance is initializing"):(zh?"；行为已生效，外观未确认，请查看详情":"; behavior enabled, appearance is unconfirmed; see details");
                        result["notification_sent"]=notifications.show(config.features.at("notifications").at("mode"),text,id,result.value("partial",false)&&!result.value("appearance_pending",false));
                    }
                    if(config.features.at("restore").value("automatic",false)&&result.value("state_confirmed",false))save_workspace(root,features.list(tracker,config),config.features.at("restore").at("max_tabs"));
                    return result;
                }catch(const std::exception& ex){notifications.show(config.features.at("notifications").at("mode"),ex.what(),id,true);throw;}

            }
            if(command=="tabs.probe") {
                auto result = send_title_request(tracker.native_tab(id), BridgeAction::probe);
                return {{"tab_id", id}, {"ui_thread_verified", true}, {"native_title", utf8(result.title)}, {"title_changed", false}};
            }
            auto result = command == "tabs.set" ? tracker.set_depth(id, parse_depth(request.at("depth").get<std::string>())) : tracker.reset_depth(id);
            if (active) {
                synchronize();
                for (const auto& tab : tracker.list()) if (tab.at("tab_id") == id) {
                    result["title_applied"] = tab.at("title_applied"); result["applied_title"] = tab.at("applied_title");
                    result["apply_error"] = tab.at("apply_error");
                    result["message"] = tab.at("title_applied").get<bool>() ? "标签层级已应用。" : "策略已保存，标题未确认；请检查 apply_error。";
                }
            }
            return result;
        }
        throw Error(2, "未知代理请求");
    };
    auto startup_started=GetTickCount64();
    while (!stopping) {
        auto served=server.poll(handler, 100);
        if(startup_restore_pending&&(served||GetTickCount64()-startup_started>=2000)&&!stopping){startup_restore_pending=false;active=true;try{automatic_restore=handler({{"command","workspace.restore"}});}catch(const std::exception& ex){automatic_restore={{"error",ex.what()}};}synchronize();}
        // Pump STA messages for COM callbacks without stealing focus or navigating Explorer.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        for(auto& id:notifications.clicks())try {features.change(tracker,config,{{"command","tabs.locate"},{"tab_id",id}});}catch(const std::exception& ex){notifications.show(config.features.at("notifications").at("mode"),ex.what(),"",true);}
        for(auto& event:directory_shortcuts.take())try {
            last_directory_open=open_directory(event.action,event.window);
        }catch(const std::exception& ex){last_directory_open={{"entry_id",event.action},{"error",ex.what()}};notifications.show(config.features.at("notifications").at("mode"),ex.what(),"",true,event.action);}
        for(auto& event:shortcuts.take()) {
            if(!active||event.window!=GetForegroundWindow())continue;
            std::string target;
            try {
                scan();if(!scan_complete)throw Error(5,"Incomplete Explorer scan; shortcut cancelled");
                for(auto& t:features.list(tracker,config)) {
                    auto id=t.at("tab_id").get<std::string>();auto native=tracker.native_tab(id);if(native.window!=event.window||native.guid!=event.expected_guid||native.process_started!=event.process_started)continue;
                    auto report=send_feature_request(native,FeatureAction::inspect,Json::object(),true);
                    if(report.value("active",false)){target=id;break;}
                }
                if(target.empty())throw Error(3,"Active tab could not be verified");
                auto command=event.action=="toggle_pin"?"tabs.pin":event.action=="toggle_protection"?"tabs.protect":"tabs.lock";
                auto field=event.action=="toggle_pin"?"pinned":event.action=="toggle_protection"?"protected":"locked";bool enabled=false;
                for(auto& t:features.list(tracker,config))if(t.at("tab_id")==target)enabled=!t.at("behavior").value(field,false);
                auto report=features.change(tracker,config,{{"command",command},{"tab_id",target},{"enabled",enabled}},true);
                bool zh=config.language!="en";std::string action=event.action=="toggle_pin"?(zh?"固定":"Pin"):event.action=="toggle_protection"?(zh?"独立保护":"Protection"):(zh?"目录锁定":"Directory lock");
                std::string text=zh?std::string(enabled?"已启用":"已取消")+action:action+(enabled?" enabled":" disabled");
                if(report.value("partial",false))text+=report.value("appearance_pending",false)?(zh?"；外观正在初始化，请稍候":"; appearance is initializing"):(zh?"；行为已生效，外观未确认，请查看详情":"; behavior confirmed, appearance unconfirmed; see details");
                notifications.show(config.features.at("notifications").at("mode"),text,target,report.value("partial",false)&&!report.value("appearance_pending",false));
                if(config.features.at("restore").value("automatic",false))save_workspace(root,features.list(tracker,config),config.features.at("restore").at("max_tabs"));
            }catch(const std::exception& ex){notifications.show(config.features.at("notifications").at("mode"),ex.what(),target,true);}
        }
        if (!stopping && GetTickCount64() - last_scan >= 2000) { reload(); scan(); synchronize(); }
    }
    return 0;
}
Json observer_status(const fs::path& root) {
    auto response = agent_request(root, {{"command", "status"}});
    if (!response) return {{"running", false}, {"mode", "observe"}, {"hook_available", false}};
    check(*response);
    return response->at("result");
}
Json start_observer_agent(const fs::path& root) {
    auto prior = observer_status(root);
    if (prior.value("running", false)) { prior["message"] = "标签代理已运行。"; return prior; }
    // Validate configuration before spawning a background process.
    ConfigStore(root).load();
    auto exe = executable_path();
    auto command = quote_argument(exe.wstring()) + L" --data-dir " + quote_argument(root.wstring()) + L" agent run";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION raw{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &startup, &raw))
        throw Error(1, system_error("启动标签观察代理"));
    Handle process(raw.hProcess), thread(raw.hThread);
    auto deadline = GetTickCount64() + 12000;
    while (GetTickCount64() < deadline) {
        try {
            auto result = observer_status(root);
            if (result.value("running", false)) {
                result["message"] = "标签观察代理已启动；执行 tabtail start 启用真实标题定制。";
                return result;
            }
        } catch (const Error& e) { if (e.code == 4) throw; }
        if (WaitForSingleObject(process.get(), 100) == WAIT_OBJECT_0) throw Error(1, "标签观察代理启动失败；可执行 agent run 查看错误。");
    }
    TerminateProcess(process.get(), 1); WaitForSingleObject(process.get(), 2000);
    throw Error(5, "标签观察代理启动超时，已终止本次启动的进程。");
}
Json stop_observer_agent(const fs::path& root) {
    auto before = agent_request(root, {{"command", "status"}});
    if (!before) return {{"stopped", true}, {"message", "标签观察代理未运行。"}};
    check(*before);
    auto pid = before->at("result").at("pid").get<DWORD>();
    Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) throw Error(4, system_error("等待代理退出"));
    verify_peer_process(pid);
    auto response = required(root, {{"command", "stop"}}); check(response);
    if (response.at("result").value("runtime_id", "") != before->at("result").value("runtime_id", ""))
        throw Error(5, "代理在停止期间重启，请检查状态");
    if (WaitForSingleObject(process.get(), 6000) != WAIT_OBJECT_0) throw Error(5, "代理退出超时；未强制结束进程");
    auto feature_restore=response.at("result").value("feature_restoration",Json::object());
    return {{"stopped",true},{"feature_restoration",feature_restore},{"restoration",response.at("result").at("restoration")},{"complete",feature_restore.value("complete",false)},{"message",feature_restore.value("complete",false)?"代理已停止，保护与外观已恢复。":"代理已停止；部分恢复未确认，失效租约将继续解除限制。"}};
}
Json require_agent_result(const fs::path& root, Json request, int& result_code) {
    auto response = required(root, std::move(request));
    result_code = response.at("code").get<int>();
    return response.at("result");
}
}
