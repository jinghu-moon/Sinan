#include "agent/runtime.hpp"
#include "ipc/pipe.hpp"
#include "agent/tab_tracker.hpp"
#include "agent/layout_tracker.hpp"
#include "agent/features.hpp"
#include "agent/interaction.hpp"
#include "agent/folder_click.hpp"
#include "agent/folder_sizes.hpp"
#include "agent/selection_summary.hpp"
#include "agent/workspace.hpp"
#include "agent/archive_jobs.hpp"
#include "agent/tray.hpp"
#include "installer/startup.hpp"
#include "policy/entries.hpp"
#include "hook/controller.hpp"
#include "platform/shell_actions.hpp"
#include "platform/user_process.hpp"
#include <algorithm>
#include <limits>
#include <set>

namespace sinan {
namespace {
constexpr uint64_t scan_interval_ms = 2000;

std::string new_runtime_id() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw Error(1, "无法生成代理运行期 ID");
    wchar_t text[40]{}; StringFromGUID2(guid, text, 40);
    std::wstring token(text + 1, text + 37);
    return utf8(token);
}

}
int run_observer_agent(const fs::path& root,bool from_login) {
    auto endpoint = agent_endpoint(root);
    auto mutex_name = L"Local\\" + endpoint.substr(9);
    Handle singleton(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    if (!singleton) throw Error(1, system_error("创建代理单实例锁"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) throw Error(1, "当前数据目录的标签观察代理已运行");
    ConfigStore store(root);
    auto config=store.load();
    const bool restore_on_login=config.features.at("restore").at("automatic").get<bool>();
    if(from_login&&!config.autostart&&!restore_on_login)return 0;
    ShellTabEnumerator enumerator;
    auto runtime_id = new_runtime_id();
    ShellTabTracker tracker(runtime_id, config);
    LayoutTracker layouts;
    TabFeatures features;
    ExplorerShortcuts shortcuts;
    FolderClickListener folder_click;
    FolderSizes folder_sizes;
    SelectionSummary selection_summary;
    DirectoryShortcuts directory_shortcuts;
    DirectoryShortcuts global_actions;
    Json last_directory_open=Json::object();
    Json last_action=Json::object();
    std::map<std::wstring,uint64_t> pending_directory_opens;
    struct ReopenPending {
        ClosedTab entry;
        std::set<std::string> existing;
        uint64_t deadline;
    };
    std::optional<ReopenPending> reopen_pending;
    SystemNotifications notifications;
    ArchiveJobs archives;
    TrayIcon tray;
    Json feature_state=Json::object();
    Json automatic_restore=Json::object();
    std::map<std::string,uint64_t> close_counts,navigation_counts,redirect_counts;
    PipeServer server(root);
    bool stopping = false;
    const bool login_customization=from_login&&config.enabled&&native_build_supported();
    bool startup_restore_pending=config.enabled&&restore_on_login&&(!from_login||login_customization);
    bool active=login_customization&&!startup_restore_pending;
    Json application{{"applied_count", 0}, {"failed_count", 0}, {"runtime_applied", false}};
    Json layout_state{{"mode","system"},{"complete",true},{"layout_applied",false},{"threads",Json::array()}};
    Json search_state{{"active",false},{"processes",Json::array()}};
    Json external_tab_state{{"active",false}};
    std::map<HWND,NativeTab> external_frames;
    GUID external_token{};
    uint64_t external_cookie{};
    if(SUCCEEDED(CoCreateGuid(&external_token)))std::memcpy(&external_cookie,&external_token,sizeof(external_cookie));
    bool external_activation_attached=false;
    auto synchronize_external_tabs=[&](bool enabled) {
        if(!enabled&&external_frames.empty()&&!external_activation_attached)return;
        DWORD shell_pid{};
        GetWindowThreadProcessId(GetShellWindow(),&shell_pid);
        std::map<HWND,NativeTab> targets;
        for(const auto& tab:tracker.shortcut_windows())if(tab.process_id==shell_pid)targets[tab.window]=tab;
        if(!enabled)for(const auto& [window,tab]:external_frames)targets.try_emplace(window,tab);
        external_tab_state={{"active",false},{"pending",enabled&&targets.empty()},{"ready_windows",0}};
        external_frames.clear();
        for(const auto& [window,tab]:targets) {
            if(!enabled&&!IsWindow(window))continue;
            try {
                auto response=send_feature_request(tab,FeatureAction::external_tabs,{{"enabled",enabled},{"cookie",external_cookie}});
                if(response.value("active",false)||!external_tab_state.value("active",false))
                    external_tab_state=std::move(response);
                if(enabled&&external_tab_state.value("active",false))external_frames[window]=tab;
            } catch(const std::exception& error) {
                external_tab_state["error"]=error.what();
            }
        }
        const bool activate=enabled&&external_cookie&&external_tab_state.value("active",false);
        if(activate||external_activation_attached)try {
            external_tab_state["activation"]=send_external_activation_request(activate,external_cookie);
            external_activation_attached=external_tab_state.at("activation").value("active",false);
        }catch(const std::exception& error){
            external_tab_state["activation"]={{"active",false},{"error",error.what()}};
        }
    };
    std::map<std::pair<DWORD,uint64_t>,NativeTab> search_processes;
    auto synchronize_search=[&](bool enabled) {
        std::map<std::pair<DWORD,uint64_t>,NativeTab> targets;
        if(enabled)for(const auto& tab:tracker.shortcut_windows())targets[{tab.process_id,tab.process_started}]=tab;
        else {
            for(const auto& tab:tracker.shortcut_windows())
                if(search_processes.contains({tab.process_id,tab.process_started}))targets[{tab.process_id,tab.process_started}]=tab;
            search_processes.clear();
        }
        Json processes=Json::array();size_t active_count=0;
        for(const auto& [identity,tab]:targets) {
            Json report;
            try {
                if(enabled)search_processes[identity]=tab;
                report=send_layout_payload(tab,enabled?LayoutAction::search_enable:LayoutAction::search_disable,{},
                    {{"all_drives",config.everything_search_all_drives}});
            }catch(const std::exception& error){report={{"active",false},{"error",error.what()}};}
            if(report.value("active",false))++active_count;
            report["pid"]=tab.process_id;
            processes.push_back(std::move(report));
        }
        std::erase_if(search_processes,[&](const auto& entry){return process_lifetime(entry.first.first,entry.first.second)==ProcessLifetime::ended;});
        search_state={{"active",active_count>0},{"active_processes",active_count},{"processes",std::move(processes)}};
    };
    struct RestoreOnExit {
        ShellTabTracker& tracker;
        LayoutTracker& layouts;
        TabFeatures& features;
        ~RestoreOnExit() { try {features.stop();}catch(...){} try { layouts.synchronize(tracker.shortcut_windows(),{}); } catch (...) {} try { tracker.synchronize_titles(false); } catch (...) {} }
    } restore_on_exit{tracker,layouts,features};
    const auto bin=executable_path().parent_path();
    const auto panel_executable=bin/L"sinan-panel.exe";
    const auto cli_executable=bin/L"sinan.exe";
    auto has_panel=[&] {
        std::error_code error;
        return fs::is_regular_file(panel_executable,error);
    };
    auto chinese_ui=[&] {
        return config.language=="zh-CN"||(config.language=="auto"&&PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_CHINESE);
    };
    auto panel_missing=[&] {
        return chinese_ui()?"此动作需要与代理同目录的 sinan-panel.exe。":"This action requires sinan-panel.exe next to the agent.";
    };
    auto launch_panel=[&](std::wstring arguments) {
        if(!has_panel())throw Error(3,panel_missing());
        UserProcessLauncher launcher;
        auto command=quote_argument(panel_executable.wstring())+arguments+L" --lang "+(chinese_ui()?L"zh-CN":L"en");
        auto process=launcher.launch(panel_executable,std::move(command),CREATE_SUSPENDED);
        const auto pid=GetProcessId(process.process.get());
        if(pid)AllowSetForegroundWindow(pid);
        if(ResumeThread(process.thread.get())==DWORD(-1)) {
            const auto error=GetLastError();
            TerminateProcess(process.process.get(),1);
            throw Error(4,system_error("Start Sinan panel",error));
        }
        return pid;
    };
    auto update_shortcuts=[&] {
        folder_click.update(config.double_click_parent);
        Json foreground=Json::object(),directories=Json::array(),global=Json::array();
        const auto& configured=config.features.at("shortcuts");
        for(auto key:explorer_shortcut_keys) {
            const std::string_view name=key;
            const auto& chord=configured.at(key);
            if(chord=="")continue;
            if((name=="new_file"||name=="transfer_selection")&&!has_panel())continue;
            if(name=="locate_clipboard_global")global.push_back({{"id",key},{"shortcut",chord}});
            else if(active||!shell_shortcut_action(name).empty())foreground[key]=chord;
        }
        for(const auto& entry:entries_from_json(config.features.at("directories").at("entries"))) {
            if(!entry.enabled||entry.shortcut.empty())continue;
            if(entry.scope=="explorer")foreground["entry:"+entry.id]=entry.shortcut;
            else directories.push_back(entry_json(entry));
        }
        for(const auto& script:config.features.at("scripts").at("entries")) {
            if(script.at("shortcut")=="")continue;
            const auto id="script:"+script.at("id").get<std::string>();
            if(script.at("scope")=="explorer")foreground[id]=script.at("shortcut");
            else global.push_back({{"id",id},{"shortcut",script.at("shortcut")}});
        }
        directory_shortcuts.update(directories);
        global_actions.update(global);
        shortcuts.update(foreground,tracker.shortcut_windows(),true);
    };
    std::string scan_error, config_error;
    bool scan_complete = false;
    auto runtime_applied=[&] {
        return scan_complete&&scan_error.empty()&&config_error.empty()&&application.value("runtime_applied",false);
    };
    auto update_tray=[&] {
        if(!config.show_tray_icon){
            tray.update(false);
            return;
        }
        bool tui_available=false;
#ifdef SINAN_BUILD_TUI
        std::error_code error;
        tui_available=fs::is_regular_file(cli_executable,error);
#endif
        tray.update(true,{chinese_ui(),active,runtime_applied(),config.autostart,tui_available,native_build_supported()});
    };
    auto synchronize=[&](bool restore_search=false) {
        feature_state=features.synchronize(tracker,config,active);
        update_shortcuts();
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
        layout_state=layouts.synchronize(tracker.shortcut_windows(),
            {active?config.tabs:TabAppearance{},!restore_search&&config.search_box_hidden});
        layout_state["requested_mode"]=config.tabs.width_mode;
        synchronize_search(!restore_search&&config.everything_search&&!config.search_box_hidden);
        synchronize_external_tabs(!restore_search&&config.external_tabs);
        folder_sizes.synchronize(restore_search ? FolderSizeSettings{} : config.folder_sizes,
            config.folder_sizes.enabled ? tracker.shortcut_windows() : std::vector<NativeTab>{},
            config.folder_sizes.enabled ? tracker.list() : Json::array(), enumerator, chinese_ui());
        if (restore_search) selection_summary.stop();
        else selection_summary.synchronize(tracker.shortcut_windows(), config, chinese_ui());
        update_tray();
    };
    std::vector<std::string> warnings;
    uint64_t last_scan = 0;
    auto scan = [&] {
        try {
            auto observation = enumerator.scan(true);
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
    if(active || config.folder_sizes.enabled)synchronize();
    else {
        update_shortcuts();
        update_tray();
        selection_summary.synchronize(tracker.shortcut_windows(), config, chinese_ui());
    }
    auto shortcut_status=[&] {
        auto result=shortcuts.status();
        auto global=global_actions.status();
        result["global"]=global;
        result["errors"].update(global.at("errors"));
        if(!has_panel())for(auto key:{"new_file","transfer_selection"})
            if(config.features.at("shortcuts").at(key)!="")result["errors"][key]=panel_missing();
        return result;
    };
    auto directory_shortcut_status=[&] {
        auto result=directory_shortcuts.status();
        const auto errors=shortcuts.status().at("errors");
        for(auto it=errors.begin();it!=errors.end();++it)
            if(it.key().starts_with("entry:"))result["errors"][it.key().substr(6)]=it.value();
        return result;
    };
    auto everything_search_status=[&] {
        auto result=search_state;
        result["search_box_hidden"]=config.search_box_hidden;
        return result;
    };
    auto status = [&]() {
        return Json{{"running", true}, {"pid", GetCurrentProcessId()}, {"executable",utf8(executable_path().wstring())}, {"runtime_id", runtime_id}, {"mode", active ? "customize" : "observe"},
            {"version", SINAN_VERSION}, {"data_directory", utf8(root.wstring())}, {"config_revision", config.revision},
            {"double_click_parent",folder_click.status()},
            {"everything_search",everything_search_status()},
            {"folder_sizes",folder_sizes.status()},
            {"selection_summary",selection_summary.status()},
            {"external_tabs",external_tab_state},
            {"directory_shortcuts",directory_shortcut_status()},{"last_directory_open",last_directory_open},{"automatic_restore",automatic_restore},{"features",feature_state},{"shortcuts",shortcut_status()},{"last_action",last_action},{"notifications",notifications.status()},{"layout",layout_state},
            {"hook_available", native_build_supported()}, {"runtime_applied", runtime_applied()}, {"application", application}, {"scan_complete", scan_complete},
            {"tray",tray.status()},
            {"scan_error", scan_error}, {"config_error", config_error}, {"warnings", warnings},
        {"scan_age_ms", GetTickCount64() - last_scan}, {"tab_count", tracker.list().size()},
        {"closed_tab_count",tracker.closed_tabs().size()}};
    };
    auto open_path=[&](const std::wstring& directory,HWND foreground,const std::wstring& file=std::wstring{},
                       bool use_transfer_window=false,bool stay_in_foreground=false)->Json {
        scan();
        if(!scan_error.empty())throw Error(5,"Explorer scan failed: "+scan_error);
        const bool en=config.language=="en";
        const auto path=utf8(directory);
        const auto key=directory_key(directory);
        const auto now=GetTickCount64();
        std::erase_if(pending_directory_opens,[&](const auto& item){return item.second<=now;});
        const auto focus_changed=[&]{return stay_in_foreground&&GetForegroundWindow()!=foreground;};
        if(focus_changed())return {{"deferred",true}};
        auto target=tracker.directory_target(directory,use_transfer_window||foreground==GetForegroundWindow()?foreground:nullptr,
            stay_in_foreground);
        if(!target.tab_id.empty()) {
            pending_directory_opens.erase(key);
            if(stay_in_foreground) {
                const auto tab=tracker.native_tab(target.tab_id);
                if(focus_changed())return {{"deferred",true}};
                auto result=send_feature_request(tab,FeatureAction::locate,{{"expected_path",path},{"foreground_only",true}});
                result["tab_id"]=target.tab_id;
                result["selection_confirmed"]=result.value("active_known",false)&&result.value("active",false)&&!result.value("pending",false);
                return result;
            }
            auto result=features.change(tracker,config,{{"command","tabs.locate"},{"tab_id",target.tab_id},{"expected_path",path}});
            result["reused"]=result.at("selection_confirmed");result["opened"]=false;
            if(!file.empty()&&result.value("selection_confirmed",false)) {
                auto [frame,child]=tracker.shell_target(target.tab_id);
                auto view=enumerator.view(frame,child);
                reveal_file(file,view.Get());
                result["file_selected"]=true;
            }
            return result;
        }
        if(pending_directory_opens.contains(key))
            return {{"opened",false},{"reused",false},{"pending",true},{"partial",true},
                {"message",en?"Previous open request is awaiting observation.":"上一次打开请求仍在等待扫描确认。"}};
        if(use_transfer_window&&!target.foreground_anchor)
            throw Error(3,en?"The source window has no verified tab for opening the destination.":"来源窗口没有可用于打开目标目录的已验证标签。");
        if(!use_transfer_window&&(!scan_complete||!target.locations_resolved))
            throw Error(5,en?"Explorer scan could not establish that the directory is unopened.":"本次扫描无法确认目录是否已打开，已取消打开。");
        if(!workspace_directory_available(directory))
            throw Error(3,en?"Directory is missing, offline or did not respond within 800 ms":"目录不存在、已离线或未在 800 毫秒内响应");
        if(focus_changed())return {{"deferred",true}};
        // A launch or a timed-out Explorer request can take effect after returning.
        struct ObservationGracePeriod {
            uint64_t& deadline;
            ~ObservationGracePeriod(){deadline=GetTickCount64()+3000;}
        } grace{pending_directory_opens[key]};
        if(!file.empty()&&!use_transfer_window) {
            reveal_file(file);
            return {{"confirmation","selection_requested"},{"path",utf8(file)}};
        }
        if(target.foreground_anchor&&(use_transfer_window||foreground==GetForegroundWindow())) {
            if(use_transfer_window&&!stay_in_foreground)AllowSetForegroundWindow(target.foreground_anchor->process_id);
            auto result=send_feature_request(*target.foreground_anchor,FeatureAction::open,
                {{"path",path},{"select",true},{"foreground_only",stay_in_foreground}});
            result["opened"]=result.contains("opened_guid");result["reused"]=false;result["target"]="current_window";
            if(use_transfer_window&&!stay_in_foreground&&result.value("selection_confirmed",false)) {
                SetForegroundWindow(foreground);
                result["foreground_activated"]=GetForegroundWindow()==foreground;
            }
            result["partial"]=!result.value("selection_confirmed",false);
            if(result.at("partial")==true)result["message"]=en?"Tab opened, but Explorer did not confirm selection.":"标签已打开，资源管理器尚未确认已选中。";
            return result;
        }
        open_workspace_window(directory);
        return {{"opened",false},{"reused",false},{"target","new_window"},{"confirmation","launch_requested"},
            {"message",en?"Explorer launch requested. Waiting for the directory to appear.":"已请求资源管理器打开目录，等待扫描确认。"}};
    };
    auto locate_archive=[&](ArchiveJobs::Job& job,bool requested) {
        if(!job.result)throw Error(3,"Archive output is not available");
        const auto& result=*job.result;
        if(!workspace_directory_available(result.directory.wstring()))throw Error(3,"Archive output folder is unavailable");
        DWORD pid{};
        GetWindowThreadProcessId(job.frame,&pid);
        const bool same_frame=pid&&pid==job.process_id&&job.process_started&&
            process_lifetime(pid,job.process_started)==ProcessLifetime::running;
        const auto current=GetForegroundWindow();
        if(!requested&&(!same_frame||current!=job.frame)) {
            job.location="deferred";
            return;
        }
        const auto directory=result.directory.wstring();
        const auto file=result.file.wstring();
        const auto open_with_shell=[&]{
            if(file.empty())open_workspace_window(directory);
            else reveal_file(file);
            job.location="requested";
        };
        const bool use_source=same_frame&&(!requested||!IsIconic(job.frame));
        Json opened;
        try {
            opened=open_path(directory,use_source?job.frame:current,requested&&!use_source?file:std::wstring{},use_source,!requested);
        } catch(const Error&) {
            if(!requested&&GetForegroundWindow()!=job.frame) {
                job.location="deferred";
                return;
            }
            if(!requested)throw;
            open_with_shell();
            return;
        }
        if(opened.value("deferred",false)||(!requested&&GetForegroundWindow()!=job.frame)) {
            job.location="deferred";
            return;
        }
        if(opened.value("selection_confirmed",false)) {
            if(!file.empty()) {
                const auto guid=opened.value("opened_guid",opened.value("native_guid",std::string{}));
                const auto deadline=GetTickCount64()+1000;
                bool selected=opened.value("file_selected",false);
                HWND frame=use_source?job.frame:current;
                if(opened.contains("tab_id"))frame=tracker.native_tab(opened.at("tab_id").get<std::string>()).window;
                DWORD target_pid{};
                GetWindowThreadProcessId(frame,&target_pid);
                Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,target_pid));
                FILETIME created{},exited{},kernel{},user{};
                const bool known=process&&GetProcessTimes(process.get(),&created,&exited,&kernel,&user);
                const auto started=(static_cast<uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
                while(!selected&&known) {
                    if(!requested&&GetForegroundWindow()!=job.frame) {
                        job.location="deferred";
                        return;
                    }
                    for(const auto& tab:read_native_tabs(frame,target_pid,started)) {
                        if(guid_text(tab.guid)!=guid)continue;
                        try {
                            const auto response=send_feature_request(tab,FeatureAction::locate,
                                {{"expected_path",utf8(directory)},{"select_file",utf8(file)},{"foreground_only",!requested}},!requested);
                            selected=response.value("file_selected",false);
                        } catch(const Error&) {}
                        break;
                    }
                    if(selected||GetTickCount64()>=deadline)break;
                    Sleep(50);
                }
                if(!selected&&requested) {
                    open_with_shell();
                    return;
                }
                if(!selected)throw Error(5,"Explorer did not confirm the archive output file");
            }
            if(opened.value("foreground_activated",true))job.location="located";
            else if(requested)open_with_shell();
            else job.location="unconfirmed";
        } else if(requested&&(opened.value("confirmation",std::string{})=="launch_requested"||
                             opened.value("confirmation",std::string{})=="selection_requested")) {
            job.location="requested";
        } else if(requested&&!opened.value("pending",false))open_with_shell();
        else job.location="unconfirmed";
    };
    auto run_directory_open=[&](const std::string& id,HWND foreground,const Json* binding=nullptr)->Json {
        try {
            reload();
            if(!config_error.empty())throw Error(5,config_error);
            auto entries=entries_from_json(config.features.at("directories").at("entries"));
            auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.id==id&&e.enabled;});
            if(entry==entries.end())throw Error(2,"Directory entry is missing or disabled");
            if(binding&&*binding!=entry_json(*entry))throw Error(2,"Directory binding changed before the shortcut was handled");
            auto result=open_path(entry->path,foreground);
            result["entry_id"]=id;
            return last_directory_open=std::move(result);
        }
        catch(const std::exception& ex){last_directory_open={{"entry_id",id},{"error",ex.what()}};throw;}
    };
    auto confirm_reopen=[&] {
        if(!reopen_pending)return;
        const auto key=directory_key(reopen_pending->entry.path);
        for (const auto& row : tracker.list()) {
            if (!row.at("observed").get<bool>()||!row.at("path").is_string()||
                reopen_pending->existing.contains(row.at("tab_id").get<std::string>()))continue;
            try {
                if (directory_key(wide(row.at("path").get<std::string>()))!=key)continue;
            } catch (const PolicyError&) { continue; }
            tracker.forget_closed_tab(reopen_pending->entry.sequence);
            reopen_pending.reset();
            return;
        }
        if (GetTickCount64()>=reopen_pending->deadline) reopen_pending.reset();
    };
    auto reopen=[&](HWND foreground,HWND explicit_window)->Json {
        scan();
        confirm_reopen();
        if (reopen_pending)return {{"opened",false},{"pending",true},{"confirmation","awaiting_observation"}};
        if (scan_complete)scan();
        if (tracker.closed_tabs().empty())throw Error(3,"No recently closed directory tab was observed");
        const auto entry=tracker.closed_tabs().back();
        if (!workspace_directory_available(entry.path))throw Error(3,"Recently closed directory is unavailable");
        const auto native=tracker.native_tabs();
        auto anchor=std::find_if(native.begin(),native.end(),[&](const auto& tab) {
            return tab.window==entry.window&&tab.process_id==entry.process_id&&tab.process_started==entry.process_started;
        });
        if (explicit_window||anchor==native.end()) {
            const auto frame=explicit_window?explicit_window:foreground==GetForegroundWindow()?foreground:nullptr;
            anchor=std::find_if(native.begin(),native.end(),[&](const auto& tab){return tab.window==frame;});
        }
        std::set<std::string> existing;
        for (const auto& row : tracker.list())existing.insert(row.at("tab_id").get<std::string>());
        auto begin_pending=[&] { reopen_pending=ReopenPending{entry,std::move(existing),GetTickCount64()+5000}; };
        if (anchor!=native.end()) {
            Json report;
            try {
                report=send_feature_request(*anchor,FeatureAction::open,{{"path",utf8(entry.path)},{"select",true}});
            } catch (const Error& error) {
                if (error.code==5) begin_pending();
                throw;
            }
            const bool opened=report.contains("opened_guid");
            if (opened)tracker.forget_closed_tab(entry.sequence);
            else begin_pending();
            return {{"opened",opened},{"pending",!opened},{"path",utf8(entry.path)},{"target","tab"},
                {"opened_guid",report.value("opened_guid",std::string{})},
                {"selection_confirmed",report.value("selection_confirmed",false)}};
        }
        if (explicit_window)throw Error(3,"The target window has no verified tab");
        open_workspace_window(entry.path);
        begin_pending();
        return {{"opened",false},{"pending",true},{"path",utf8(entry.path)},{"confirmation","launch_requested"}};
    };
    auto merge_duplicates=[&](HWND frame,HWND expected_child)->Json {
        if (!scan_complete)throw Error(5,"Incomplete Explorer scan; merge cancelled");
        auto native=tracker.native_tabs();
        std::erase_if(native,[&](const auto& tab){return tab.window!=frame;});
        if (native.empty())throw Error(3,"The target window has no verified tabs");
        const auto selected=read_active_tab(native.front());
        if (!selected||*selected==GUID_NULL)throw Error(3,"Could not verify the selected tab");
        const auto active_tab=std::find_if(native.begin(),native.end(),[&](const auto& tab){return tab.guid==*selected;});
        if (active_tab==native.end()||(expected_child&&active_tab->browser_window!=expected_child))
            throw Error(5,"Explorer selected tab changed before merging");
        struct Candidate {NativeTab tab;std::string path;bool keep;};
        std::map<std::wstring,std::vector<Candidate>> groups;
        for (const auto& row : features.list(tracker,config)) {
            if (row.at("window")!=reinterpret_cast<uintptr_t>(frame)||!row.at("observed").get<bool>()||
                !row.at("path").is_string()||!row.at("native_guid").is_string())continue;
            auto tab=tracker.native_tab(row.at("tab_id").get<std::string>());
            const auto& flags=row.at("behavior");
            const bool keep=tab.guid==*selected||flags.value("pinned",false)||flags.value("protected",false)||flags.value("locked",false);
            auto path=row.at("path").get<std::string>();
            try { groups[directory_key(wide(path))].push_back({tab,std::move(path),keep}); }
            catch (const PolicyError&) {}
        }
        size_t closed=0;
        auto errors=Json::array();
        const auto deadline=GetTickCount64()+10000;
        for (const auto& [key,group] : groups) {
            if (group.size()<2)continue;
            auto keeper=std::find_if(group.begin(),group.end(),[](const auto& candidate){return candidate.keep;});
            if (keeper==group.end())keeper=group.begin();
            for (const auto& candidate : group) {
                if (&candidate==&*keeper||candidate.keep)continue;
                if (GetTickCount64()>=deadline) {
                    errors.push_back("Merge time limit reached");
                    return {{"closed",closed},{"partial",true},{"errors",errors}};
                }
                try {
                    const auto report=send_feature_request(candidate.tab,FeatureAction::close_duplicate,{
                        {"path",candidate.path},{"keep_guid",guid_text(keeper->tab.guid)},{"active_guid",guid_text(*selected)}});
                    if (report.value("closed",false))++closed;
                    else errors.push_back("Explorer did not confirm closing the duplicate");
                } catch (const std::exception& ex) {
                    errors.push_back(ex.what());
                    return {{"closed",closed},{"partial",true},{"errors",errors},{"message",ex.what()}};
                }
            }
        }
        return {{"closed",closed},{"partial",!errors.empty()},{"errors",errors}};
    };
    auto run_action=[&](const Json& request,HWND foreground,HWND exact_child=nullptr)->Json {
        const auto action=request.at("command").get<std::string>();
        try {
            Json result;
            if(action=="explorer.toggle_file_extensions") {
                result=toggle_file_extensions();
            }else if(action=="explorer.locate_clipboard") {
                auto path=clipboard_path();
                auto attributes=workspace_path_attributes(path);
                if(!attributes||*attributes==INVALID_FILE_ATTRIBUTES)
                    throw Error(3,"Clipboard path was not found, is offline or did not respond within 800 ms");
                const bool is_directory=(*attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
                result=is_directory?open_path(path,foreground):open_path(fs::path(path).parent_path().wstring(),foreground,path);
            }else {
                const bool reopening=action=="explorer.reopen_closed_tab",merging=action=="explorer.merge_duplicate_tabs",
                    parent_tab=action=="explorer.open_parent_tab";
                const bool new_file=action=="explorer.new_file";
                const bool transfer=action=="explorer.transfer_selection";
                const bool extract=action=="explorer.extract_archive";
                if(!reopening&&!merging&&!parent_tab&&!new_file&&!transfer&&!extract&&action!="explorer.open_terminal"&&action!="explorer.copy_directory"&&action!="explorer.copy_selection"&&action!="explorer.new_folder"&&action!="explorer.search_everything")
                    throw Error(2,"Unknown Explorer action");
                HWND frame=foreground, child=exact_child;
                const bool explicit_tab=request.contains("tab_id");
                if(merging||parent_tab||explicit_tab)scan();
                if(request.contains("tab_id")) {
                    const auto id=request.at("tab_id").get<std::string>();
                    if(!id.starts_with(runtime_id+"."))throw Error(2,"Runtime changed; refresh tabs");
                    std::tie(frame,child)=tracker.shell_target(id);
                }
                const bool interactive=action=="explorer.new_folder"&&!request.contains("name");
                if(!reopening&&(!child||interactive)) {
                    if(!frame||frame!=GetForegroundWindow())throw Error(3,"Bring Explorer to the foreground or supply a tab_id");
                }
                if(reopening)result=reopen(foreground,explicit_tab?frame:nullptr);
                else if(merging)result=merge_duplicates(frame,exact_child);
                else if(parent_tab) {
                    auto native=tracker.native_tabs();
                    auto target=std::find_if(native.begin(),native.end(),[&](const auto& tab){
                        if(tab.window!=frame||(child&&tab.browser_window!=child))return false;
                        return explicit_tab||read_active_tab(tab)==tab.guid;
                    });
                    if(target==native.end())throw Error(3,"Could not verify the source tab");
                    const auto tabs=tracker.list();
                    const auto source=std::find_if(tabs.begin(),tabs.end(),[&](const auto& row){
                        return row.at("observed").get<bool>()&&
                            row.at("browser_window")==reinterpret_cast<uintptr_t>(target->browser_window)&&
                            row.at("native_guid")==guid_text(target->guid);
                    });
                    if(source==tabs.end()||!source->at("path").is_string())
                        throw Error(3,"Explorer tab has no filesystem directory");
                    const auto path=source->at("path").get<std::string>();
                    const auto parent=directory_parent(wide(path));
                    if(!parent)throw Error(3,"This filesystem root has no parent directory");
                    auto report=send_feature_request(*target,FeatureAction::open,
                        {{"path",utf8(*parent)},{"expected_path",path},{"select",true}},!explicit_tab);
                    const bool selected=report.value("selection_confirmed",false);
                    result={{"opened",report.contains("opened_guid")},{"path",utf8(*parent)},
                        {"selection_confirmed",selected},{"partial",!selected}};
                    if(report.contains("opened_guid"))result["opened_guid"]=report.at("opened_guid");
                } else if(extract) {
                    auto view=enumerator.view(frame,child,!child||exact_child);
                    auto source=archive_source(selected_paths(*view.Get()));
                    result=ArchiveJobs::describe(archives.enqueue(std::move(source),config.features.at("archives"),frame));
                } else if(new_file||transfer) {
                    if(!has_panel())throw Error(3,panel_missing());
                    Microsoft::WRL::ComPtr<IShellBrowser> browser;
                    auto view=enumerator.view(frame,child,!child||exact_child,&browser);
                    HWND source_tab{};
                    TabLocation location;
                    if(FAILED(browser->GetWindow(&source_tab))||!source_tab||
                        FAILED(shell_view_location(*view.Get(),location))||!location.filesystem_path)
                        throw Error(3,"Explorer tab has no filesystem directory");
                    auto arguments=(transfer?L" --transfer ":L" --new-file ")+quote_argument(*location.filesystem_path)+
                        L" --source-tab "+std::to_wstring(reinterpret_cast<uintptr_t>(frame))+L" "+
                        std::to_wstring(reinterpret_cast<uintptr_t>(source_tab));
                    if(transfer) {
                        auto paths=selected_paths(*view.Get());
                        if(paths.empty())throw Error(3,chinese_ui()?"请先在资源管理器选中文件或文件夹。":"Select files or folders in Explorer first.");
                        arguments+=L" --data-dir "+quote_argument(root.wstring())+L" --selection-check "+wide(selection_fingerprint(std::move(paths)));
                    }
                    const auto pid=launch_panel(std::move(arguments));
                    result={{"dialog_launched",true},{"path",utf8(*location.filesystem_path)},{"process_id",pid}};
                } else {
                    auto view=enumerator.view(frame,child,interactive||!child||exact_child);
                    auto parameters=request;
                    if(interactive)parameters["language"]=config.language;
                    result=run_shell_view_action(*view.Get(),frame,action,parameters);
                }
            }
            last_action={{"action",action},{"result",result}};
            return result;
        } catch(const std::exception& ex) {
            last_action={{"action",action},{"error",ex.what()}};
            throw;
        }
    };
    auto run_script=[&](const std::string& id,HWND foreground,HWND child, const Json& request,
                        const char* shortcut_scope=nullptr,Shortcut chord={})->Json {
        const auto action="script."+id;
        try {
            reload();
            if(!config_error.empty())throw Error(5,config_error);
            const auto& scripts=config.features.at("scripts").at("entries");
            const auto script=std::find_if(scripts.begin(),scripts.end(),[&](const auto& row){return row.at("id")==id;});
            if(script==scripts.end())throw Error(2,"Unknown PowerShell action");
            if(shortcut_scope&&(script->at("scope")!=shortcut_scope||
                parse_shortcut(script->at("shortcut").get<std::string>())!=chord))throw Error(3,"PowerShell shortcut changed");
            Microsoft::WRL::ComPtr<IShellView> view;
            if(request.contains("tab_id")) {
                scan();
                auto [frame,browser]=tracker.shell_target(request.at("tab_id").get<std::string>());
                if(!browser)throw Error(3,"The selected tab has no verified Explorer view");
                view=enumerator.view(frame,browser,false);
            } else {
                wchar_t window_class[64]{};
                if(foreground&&foreground==GetForegroundWindow())GetClassNameW(foreground,window_class,64);
                if(wcscmp(window_class,L"CabinetWClass")==0)view=enumerator.view(foreground,child,true);
                else if(script->at("scope")=="explorer")throw Error(3,"Bring Explorer to the foreground or supply a tab_id");
            }
            auto result=run_powershell_action(wide(script->at("path").get<std::string>()),view.Get());
            last_action={{"action",action},{"result",result}};
            return result;
        } catch(const std::exception& ex) {
            last_action={{"action",action},{"error",ex.what()}};
            throw;
        }
    };
    auto handler = [&](const Json& request) -> Json {
        auto command = request.at("command").get<std::string>();
        if (command == "status") return status();
        if(command=="archives.jobs")return archives.list();
        if(command=="archives.cancel"||command=="archives.open") {
            auto& job=archives.get(request.at("id").get<std::string>());
            if(command=="archives.cancel")archives.cancel(job);
            else locate_archive(job,true);
            return ArchiveJobs::describe(job);
        }
        if(command=="transfer.complete") {
            const auto path=wide(request.at("path").get<std::string>());
            directory_key(path);
            const auto count=request.at("count").get<size_t>();
            const bool moving=request.at("moving").get<bool>();
            const bool open=request.at("open_target").get<bool>();
            auto frame=reinterpret_cast<HWND>(request.at("window").get<uintptr_t>());
            const bool zh=chinese_ui();
            std::string message=zh?(moving?"已移动 ":"已复制 "):(moving?"Moved ":"Copied ");
            message+=std::to_string(count)+(zh?" 项到 ":count==1?" item to ":" items to ");
            const auto label=fs::path(path).filename().wstring();
            message+=utf8(label.empty()?path:label);
            Json result={{"completed",true},{"path",utf8(path)},{"count",count},{"moving",moving},
                {"open_requested",open},{"destination_selected",false}};
            if(open) {
                try {
                    auto opened=open_path(path,frame,{},true);
                    result["destination_selected"]=opened.value("selection_confirmed",false);
                    result["destination"]=std::move(opened);
                } catch(const std::exception& error) {
                    result["open_error"]=error.what();
                }
            }
            const bool open_failed=open&&!result.at("destination_selected").get<bool>();
            if(open_failed)message=(zh?"未能打开目标标签。":"Could not open the destination tab. ")+message;
            const auto& mode=config.features.at("notifications").at("mode").get_ref<const std::string&>();
            result["notification_sent"]=notifications.show(mode,message,"",open_failed);
            result["notification_expected"]=mode=="all"||(mode=="errors"&&open_failed);
            result["message"]=std::move(message);
            last_action={{"action","explorer.transfer_selection"},{"result",result}};
            return result;
        }
        if(command=="directories.open")return run_directory_open(request.at("id").get<std::string>(),reinterpret_cast<HWND>(request.value("foreground",uint64_t{})));
        if(command=="scripts.run")return run_script(request.at("id").get<std::string>(),
            reinterpret_cast<HWND>(request.value("foreground",uint64_t{})),nullptr,request);
        if(command.starts_with("explorer."))return run_action(request,reinterpret_cast<HWND>(request.value("foreground",uint64_t{})));
        if (command == "stop") {
            active = false; synchronize(true); stopping = true;
            return {{"stopping", true},{"partial",!feature_state.value("complete",false)||!application.value("complete",false)||!layout_state.value("complete",false)}, {"pid", GetCurrentProcessId()}, {"executable",utf8(executable_path().wstring())}, {"runtime_id", runtime_id}, {"restoration", application},{"feature_restoration",feature_state},{"layout_restoration",layout_state}};
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
            return {{"directory_shortcuts",directory_shortcut_status()},{"shortcuts",shortcut_status()},{"config_received", true}, {"mode",active?"customize":"observe"}, {"revision", config.revision}, {"title_applied", active && application.value("complete", false)},
                {"double_click_parent",folder_click.status()},
                {"everything_search",everything_search_status()},
                {"folder_sizes",folder_sizes.status()},
                {"external_tabs",external_tab_state},
                {"tray",tray.status()},
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
            if(command=="tabs.locate"&&request.at("runtime_id").get<std::string>()!=runtime_id)
                throw Error(2,"代理运行期已变化，请重新执行 tabs list");
            reload(); scan();
            if (active&&command!="tabs.locate") synchronize();
            if (!scan_error.empty()) throw Error(5, "本次标签扫描失败：" + scan_error);
            if (command == "tabs.list") {
                auto result = status(); result["tabs"] = features.list(tracker,config); return result;
            }
            if (request.at("runtime_id").get<std::string>() != runtime_id) throw Error(2, "代理运行期已变化，请重新执行 tabs list");
            auto id = request.at("tab_id").get<std::string>();
            const auto known = tracker.list();
            if (std::none_of(known.begin(), known.end(), [&](const auto& tab) { return tab.at("tab_id") == id; }))
                throw Error(2, "标签不存在、已关闭或属于其他代理运行期");
            if (!scan_complete&&command!="tabs.locate") throw Error(5, "本次标签扫描不完整，未修改策略；请重新执行 tabs list 后重试。");
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
    auto run_tray_action=[&](TrayAction action) {
        const bool zh=chinese_ui();
        try {
            switch(action){
            case TrayAction::open_tui: {
                const auto command=quote_argument(cli_executable.wstring())+L" --data-dir "+quote_argument(root.wstring())+L" tui";
                UserProcessLauncher{}.launch(cli_executable,command,CREATE_NEW_CONSOLE);
                break;
            }
            case TrayAction::toggle_customization: {
                const auto result=handler({{"command",active?"disable":"enable"}});
                if(result.value("partial",false))
                    tray.report(zh?L"标签定制已停止，部分 Explorer 恢复尚未确认。":L"Customization stopped. Some Explorer restoration is pending.",true);
                break;
            }
            case TrayAction::toggle_startup: {
                const bool enabled=!config.autostart;
                config.autostart=store.update([enabled](Config& current){current.autostart=enabled;}).autostart;
                update_tray();
                try {
                    const auto result=sync_login_startup(root,cli_executable);
                    if(!enabled&&result.value("workspace_restore",false)&&result.value("login_registered",false))
                        tray.report(zh?L"登录自启已关闭。登录项仍用于恢复工作区。":L"Start at sign-in is off. The login entry is kept for workspace restoration.");
                } catch(const std::exception& e) {
                    const auto message=zh?L"设置已保存，登录项更新失败。":L"Setting saved. Could not update the login entry.";
                    tray.report(message+std::wstring(L"\n")+wide(e.what()),true);
                }
                break;
            }
            case TrayAction::exit:
                if(!archives.pending()||tray.confirm_exit()){
                    const auto result=handler({{"command","stop"}});
                    if(result.value("partial",false))
                        tray.report(zh?L"部分 Explorer 恢复尚未确认。":L"Some Explorer restoration is pending.",true);
                }
                break;
            }
        } catch(const std::exception& e) {
            tray.report(wide(e.what()),true);
        }
    };
    auto startup_started=GetTickCount64();
    while (!stopping) {
        const auto now = GetTickCount64();
        const auto deadline = startup_restore_pending
            ? std::min(last_scan + scan_interval_ms, startup_started + scan_interval_ms)
            : last_scan + scan_interval_ms;
        auto wait_ms = static_cast<DWORD>(deadline > now ? deadline - now : 0);
        if (archives.pending()) wait_ms = std::min<DWORD>(wait_ms, 100);
        wait_ms = selection_summary.wait_time(wait_ms);
        auto served=server.poll(handler, wait_ms, shortcuts.action_ready());
        if(stopping)break;
        if(startup_restore_pending&&(served||GetTickCount64()-startup_started>=2000)&&!stopping){startup_restore_pending=false;active=true;try{automatic_restore=handler({{"command","workspace.restore"}});}catch(const std::exception& ex){automatic_restore={{"error",ex.what()}};}synchronize();}
        // Pump STA messages for COM callbacks without stealing focus or navigating Explorer.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if(auto action=tray.take())run_tray_action(*action);
        if(stopping)break;
        selection_summary.poll(enumerator);
        if(auto* job=archives.poll()) {
            const bool zh=chinese_ui();
            const bool success=job->result.has_value();
            bool failure=job->state=="failed";
            std::string notification;
            if(success) {
                try { locate_archive(*job,false); }
                catch(const std::exception& error) {job->location="unconfirmed";job->error=error.what();}
                notification=(zh?"解压完成  ":"Extracted  ")+utf8(job->result->output.filename().wstring());
                if(job->location!="located")notification+=zh?"。点击打开结果。":". Click to open the result.";
                if(job->result->recycle=="recycled")notification+=zh?" 原包已移入回收站。":" Source moved to the Recycle Bin.";
                else if(!job->result->recycle.empty()) {
                    const bool part=job->result->recycle=="partial";
                    notification+=zh?(part?" 部分分卷仍在原目录。":" 原包已保留。"):
                        (part?" Some volumes were retained.":" Source was retained.");
                }
                failure=job->location=="unconfirmed"||(!job->result->recycle.empty()&&job->result->recycle!="recycled");
            } else notification=job->state=="canceled"?(zh?"解压已取消。":"Extraction canceled."):
                (zh?"解压失败  ":"Extraction failed  ")+job->error;
            const auto sent=notifications.show(config.features.at("notifications").at("mode"),notification,
                success?"archive:"+job->id:"",failure);
            last_action={{"action","explorer.extract_archive"},{"result",ArchiveJobs::describe(*job)}};
            last_action["result"]["notification_sent"]=sent;
        }
        if (const auto click=folder_click.take()) {
            try {
                Microsoft::WRL::ComPtr<IShellBrowser> browser;
                auto view=enumerator.view(click->frame,nullptr,true,&browser);
                HWND child{};
                if (SUCCEEDED(browser->GetWindow(&child))&&folder_click.empty_items_view(*click,*view.Get())) {
                    auto tabs=features.list(tracker,config);
                    const auto locked=std::any_of(tabs.begin(),tabs.end(),[&](const auto& tab) {
                        return tab.at("browser_window")==reinterpret_cast<uintptr_t>(child)&&
                            tab.at("behavior").value("locked",false);
                    });
                    if (!locked&&GetForegroundWindow()==click->frame) {
                        TabLocation location;
                        if (SUCCEEDED(shell_view_location(*view.Get(),location))&&location.filesystem_path&&
                            directory_parent(*location.filesystem_path)) {
                            enumerator.view(click->frame,child,true);
                            const auto hr=browser->BrowseObject(nullptr,SBSP_PARENT|SBSP_SAMEBROWSER);
                            if (SUCCEEDED(hr))last_action={{"action","explorer.double_click_parent"},{"navigation_requested",true}};
                        }
                    }
                }
            } catch (const std::exception& error) {
                last_action={{"action","explorer.double_click_parent"},{"error",error.what()}};
            }
        }
        for(auto& id:notifications.clicks())try {
            if(id.empty())continue;
            if(id.starts_with("archive:")) {
                locate_archive(archives.get(id.substr(8)),true);
                continue;
            }
            auto result=handler({{"command","tabs.locate"},{"runtime_id",runtime_id},{"tab_id",id}});
            if(result.value("partial",false))notifications.show(config.features.at("notifications").at("mode"),result.at("message"),"",true);
        }catch(const std::exception& ex){notifications.show(config.features.at("notifications").at("mode"),ex.what(),"",true);}
        for(auto& event:directory_shortcuts.take()) {
            const auto& id=event.entry.at("id").get_ref<const std::string&>();
            try {
                auto result=run_directory_open(id,event.window,&event.entry);
                if(result.value("partial",false)&&!result.value("pending",false))
                    notifications.show(config.features.at("notifications").at("mode"),result.at("message"),"",true,id);
            }catch(const std::exception& ex){notifications.show(config.features.at("notifications").at("mode"),ex.what(),"",true,id);}
        }
        for(auto& event:global_actions.take())try {
            const auto id=event.entry.at("id").get<std::string>();
            if(id.starts_with("script:")) {
                run_script(id.substr(7),event.window,nullptr,Json::object(),"global",parse_shortcut(event.entry.at("shortcut").get<std::string>()));
                continue;
            }
            reload();
            if(!config_error.empty())throw Error(5,config_error);
            if(event.entry.at("shortcut")!=config.features.at("shortcuts").at("locate_clipboard_global"))continue;
            auto result=run_action({{"command","explorer.locate_clipboard"}},event.window);
            if(result.value("partial",false))notifications.show(config.features.at("notifications").at("mode"),result.value("message","Location unconfirmed"),"",true);
        }catch(const std::exception& ex){notifications.show(config.features.at("notifications").at("mode"),ex.what(),"",true,event.entry.at("id"));}
        for(auto& event:shortcuts.take()) {
            if(event.window!=GetForegroundWindow())continue;
            const auto shell_action=shell_shortcut_action(event.action);
            if(!shell_action.empty()||event.action.starts_with("entry:")||event.action.starts_with("script:")) {
                try {
                    DWORD pid{};
                    GetWindowThreadProcessId(event.window,&pid);
                    if(pid!=event.process_id||process_lifetime(pid,event.process_started)!=ProcessLifetime::running)continue;
                    HWND child{};
                    if(event.expected_guid!=GUID_NULL) {
                        auto native=read_native_tabs(event.window,pid,event.process_started);
                        auto tab=std::find_if(native.begin(),native.end(),[&](const auto& item){return item.guid==event.expected_guid;});
                        if(tab==native.end()||!IsWindowVisible(tab->browser_window)||GetForegroundWindow()!=event.window)continue;
                        child=tab->browser_window;
                    }
                    Json result;
                    if(event.action.starts_with("entry:"))result=run_directory_open(event.action.substr(6),event.window);
                    else if(event.action.starts_with("script:"))result=run_script(event.action.substr(7),event.window,child,Json::object(),"explorer",event.chord);
                    else {
                        Json request{{"command",shell_action}};
                        const auto& name=config.features.at("shortcuts").at("new_folder_name");
                        if(shell_action=="explorer.new_folder"&&name!="")request["name"]=name;
                        result=run_action(request,event.window,child);
                    }
                    if(result.value("partial",false))notifications.show(config.features.at("notifications").at("mode"),result.value("message","Action unconfirmed"),"",true);
                }catch(const std::exception& ex){notifications.show(config.features.at("notifications").at("mode"),ex.what(),"",true,event.action);}
                continue;
            }
            if(!active)continue;
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
        if (!stopping && GetTickCount64() - last_scan >= scan_interval_ms) { reload(); scan(); confirm_reopen(); synchronize(); }
    }
    return 0;
}
}
