#include "feature_exercise.hpp"
#include "hook/style.hpp"
#include "agent/features.hpp"
#include "hook/controller.hpp"
#include "formatter/formatter.hpp"
#include "policy/entries.hpp"
#include "hook/identity.hpp"
#include "platform/user_process.hpp"
#include "platform/shell_tabs.hpp"
#include <sddl.h>
#include <iostream>
#include <set>

namespace tabtail {
void pump(DWORD duration){auto end=GetTickCount64()+duration;while(GetTickCount64()<end){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}}

HRESULT exercise(const NativeTab& tab,uint32_t operation,const fs::path& target={}) {
    auto module=LoadLibraryExW((executable_path().parent_path()/L"tabtail-exercise.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module)throw Error(1,"Missing exercise DLL");
    auto thread=GetWindowThreadProcessId(tab.window,nullptr);auto hook=SetWindowsHookExW(WH_CALLWNDPROC,reinterpret_cast<HOOKPROC>(GetProcAddress(module,"TabTailExerciseHook")),module,thread);
    if(!hook){FreeLibrary(module);throw Error(1,"Cannot attach exercise hook");}
    auto nonce=GetTickCount64();auto sender=GetCurrentProcessId();
    PSECURITY_DESCRIPTOR descriptor{};auto sddl=L"D:P(A;;GA;;;"+current_sid()+L")";ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa),descriptor,FALSE};Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,sizeof(ExercisePacket),exercise_mapping(sender,nonce).c_str()));LocalFree(descriptor);
    auto p=static_cast<ExercisePacket*>(MapViewOfFile(mapping.get(),FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(ExercisePacket)));if(!p)throw Error(1,"Exercise mapping failed");
    new(p)ExercisePacket;p->tab=tab;p->nonce=nonce;p->sender_pid=sender;p->sender_started=bridge_process_started(GetCurrentProcess());p->operation=operation;wcscpy_s(p->target,target.c_str());
    DWORD_PTR ignored{};SendMessageTimeoutW(tab.window,RegisterWindowMessageW(exercise_message),sender,nonce,SMTO_ABORTIFHUNG|SMTO_BLOCK,4000,&ignored);
    auto result=p->completed?p->result:E_PENDING;UnmapViewOfFile(p);UnhookWindowsHookEx(hook);FreeLibrary(module);return result;
}
}
int main(int argc,char** argv) {
    using namespace tabtail;Json report=Json::object();std::vector<NativeTab> created;
    try {
        ShellTabEnumerator enumerator;auto root=executable_path().parent_path();auto scan=enumerator.scan();
        if(argc>1&&std::string(argv[1])=="title-diagnostics") {
            auto target=std::find_if(scan.tabs.begin(),scan.tabs.end(),[](auto& t){return t.native.has_value();});
            if(target==scan.tabs.end())throw Error(5,"No observed native tab for read-only title diagnostics");auto tab=*target->native;
            auto conflicts=conflicting_title_modules(tab.process_id,root/L"tabtail-hook.dll");report["conflicting_modules"]=Json::array();for(auto& path:conflicts)report["conflicting_modules"].push_back(utf8(path.wstring()));
            if(conflicts.empty())throw Error(5,"No foreign module present to exercise conflict diagnostic");
            try{send_title_request(tab,BridgeAction::apply);throw Error(5,"Foreign title module was not rejected");}catch(const Error& ex){if(std::string(ex.what()).find(utf8(conflicts.front().wstring()))==std::string::npos)throw;report["apply_rejected_before_injection"]=ex.what();}
            report["probe_title"]=utf8(send_title_request(tab,BridgeAction::probe).title);report["probe_protocol"]=BridgePacket{}.version;
            report["readonly_checks_passed"]=true;std::cout<<report.dump(2)<<'\n';return 0;
        }
        bool docking=argc>1&&std::string(argv[1])=="docking";
        bool isolated=argc>1&&(std::string(argv[1])=="isolated"||docking);std::optional<NativeTab> isolated_anchor;
        if(isolated){auto folder=root/L"TabTail-acceptance-anchor";fs::create_directories(folder);wchar_t windows[32768]{};GetWindowsDirectoryW(windows,32768);fs::path exe=fs::path(windows)/L"explorer.exe";UserProcessLauncher launcher;auto child=launcher.launch(exe,quote_argument(exe.wstring())+L" /separate,/n,"+quote_argument(folder.wstring()));
            for(int retry=0;retry<80&&!isolated_anchor;++retry){pump(100);scan=enumerator.scan();for(auto& t:scan.tabs)if(t.native&&t.location.filesystem_path&&*t.location.filesystem_path==folder.wstring()){isolated_anchor=*t.native;break;}}
            if(!isolated_anchor)throw Error(5,"Isolated acceptance window was not observed");report["isolated_pid"]=isolated_anchor->process_id;
        }
        auto it=std::find_if(scan.tabs.begin(),scan.tabs.end(),[](auto& t){return t.native.has_value();});
        if(it==scan.tabs.end())throw Error(3,"Open an Explorer window first");auto anchor=isolated_anchor?*isolated_anchor:*it->native;
        pump(500);for(auto& t:enumerator.scan().tabs)if(t.native&&t.location.filesystem_path) {
            fs::path path(*t.location.filesystem_path);if((!isolated_anchor||t.native->guid!=isolated_anchor->guid)&&path.parent_path()==root&&path.filename().wstring().starts_with(L"TabTail-acceptance-"))exercise(*t.native,1);
        }
        if(argc>1&&std::string(argv[1])=="cleanup"){std::cout<<"{}\n";return 0;}
        auto first=root/(L"TabTail-acceptance-"+std::to_wstring(GetCurrentProcessId()));auto second=root/(first.filename().wstring()+L"-destination");
        fs::create_directories(first);fs::create_directories(second);
        auto opened=send_feature_request(anchor,FeatureAction::open,{{"path",utf8(first.wstring())}});
        auto find=[&](std::string guid){for(int i=0;i<20;++i){auto s=read_native_tabs(anchor.window,anchor.process_id,anchor.process_started);for(auto& t:s)if(guid_text(t.guid)==guid)return t;MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(100);}throw Error(5,"Test tab not observed");};
        report["opened"]=opened;report["observed"]=Json::array();for(auto& t:enumerator.scan().tabs)if(t.native)report["observed"].push_back({{"guid",guid_text(t.native->guid)},{"path",t.location.filesystem_path?utf8(*t.location.filesystem_path):""}});
        pump(1000);auto tab=find(opened.at("opened_guid"));created.push_back(tab);
        report["test_path"]=utf8(first.wstring());report["native_guid"]=guid_text(tab.guid);
        if(docking) {
            if(argc>2&&std::string(argv[2])=="--alias") {
            RuntimePolicy alias_policy{{2,L"\\",{{"alias",L"TT Alias",first.wstring(),"",true,false,"alias_depth"}}},std::make_shared<RuleSet>(),{}};
            auto alias=send_title_request(tab,BridgeAction::apply,alias_policy);report["alias_applied"]=utf8(alias.title);
            if(alias.title!=L"TT Alias")throw Error(5,"Alias native title readback mismatch");
            auto navigation=exercise(tab,3,second);if(FAILED(navigation))throw Error(5,"Acceptance alias navigation failed");pump(500);
            auto normal=send_title_request(tab,BridgeAction::apply,alias_policy);report["title_after_leaving_alias"]=utf8(normal.title);
            if(normal.title!=format_title(second.wstring(),{2,L"\\"}).text)throw Error(5,"Leaving alias scope did not restore regular title");
            exercise(tab,3,first);pump(300);auto back=send_title_request(tab,BridgeAction::apply,alias_policy);report["alias_after_return"]=utf8(back.title);
            if(back.title!=L"TT Alias")throw Error(5,"Returning to alias scope did not restore alias");send_title_request(tab,BridgeAction::restore);
            }
            auto third=send_feature_request(anchor,FeatureAction::open,{{"path",utf8(second.wstring())},{"select",true}});auto other=find(third.at("opened_guid"));created.push_back(other);pump(300);
            auto order=[&]{for(int retry=0;retry<30;++retry){auto result=send_style_request(anchor,StyleAction::order);if(result.contains("order"))return result.at("order");pump(100);}throw Error(5,"XAML order initialization timed out");};
            auto before=order();auto reversed=before;std::reverse(reversed.begin(),reversed.end());report["before"]=before;
            report["route"]=send_feature_request(anchor,FeatureAction::reorder,{{"expected",before},{"order",reversed},{"probe",true}});
            report["reverse"]=send_feature_request(anchor,FeatureAction::reorder,{{"expected",before},{"order",reversed}});
            pump(300);report["after"]=order();
            report["restore_order"]=send_feature_request(anchor,FeatureAction::reorder,{{"expected",reversed},{"order",before}});
            auto extra_path=root/L"TabTail-acceptance-extra";fs::create_directories(extra_path);auto fourth=send_feature_request(anchor,FeatureAction::open,{{"path",utf8(extra_path.wstring())}});auto extra=find(fourth.at("opened_guid"));created.push_back(extra);pump(300);
            Config c;c.enabled=true;c.features["tab_style"]["pinned"]=Json::object();c.features["tab_style"]["locked"]=Json::object();c.features["tab_behavior"]["pinned_position"]="left";
            ShellTabScan observation;
            for(int retry=0;retry<40;++retry){observation=enumerator.scan(true);std::erase_if(observation.tabs,[&](auto& t){return !t.native||t.native->window!=anchor.window;});std::set<std::string> identities;for(auto& t:observation.tabs)identities.insert(guid_text(t.native->guid));if(identities.contains(guid_text(anchor.guid))&&identities.contains(guid_text(tab.guid))&&identities.contains(guid_text(other.guid))&&identities.contains(guid_text(extra.guid)))break;pump(100);}
            observation.complete=true;ShellTabTracker tracker("docking-acceptance",c);tracker.reconcile(observation);report["product_observed"]=tracker.list();TabFeatures features;
            auto id_for=[&](const NativeTab& native){for(auto& t:tracker.list())if(t.at("native_guid")==guid_text(native.guid))return t.at("tab_id").get<std::string>();throw Error(5,"Acceptance tracker identity missing");};
            auto check_order=[&](const Json& expected){pump(100);auto actual=order();if(actual!=expected)throw Error(5,"Product docking order mismatch");return actual;};
            auto initial=order().get<std::vector<std::string>>();std::vector<std::string> pins{guid_text(tab.guid),guid_text(extra.guid)};
            report["product_pin_first"]=features.change(tracker,c,{{"command","tabs.pin"},{"tab_id",id_for(tab)},{"enabled",true}});
            report["product_pin_second"]=features.change(tracker,c,{{"command","tabs.pin"},{"tab_id",id_for(extra)},{"enabled",true}});
            auto left=dock_order(initial,pins,"left");report["product_left"]=check_order(left);
            c.features["tab_behavior"]["pinned_position"]="right";report["product_right_sync"]=features.synchronize(tracker,c,true);auto right=dock_order(left,pins,"right");report["product_right"]=check_order(right);
            c.features["tab_behavior"]["pinned_position"]="left";features.synchronize(tracker,c,true);check_order(left);
            report["product_unpin"]=features.change(tracker,c,{{"command","tabs.pin"},{"tab_id",id_for(tab)},{"enabled",false}});
            auto unpinned=dock_order(left,{guid_text(extra.guid)},"left",{guid_text(tab.guid)});report["product_unpin_order"]=check_order(unpinned);
            report["product_lock_only"]=features.change(tracker,c,{{"command","tabs.lock"},{"tab_id",id_for(other)},{"enabled",true}});report["product_lock_order"]=check_order(unpinned);
            report["product_stop"]=features.stop();
            for(auto& t:created)exercise(t,1);pump(300);exercise(anchor,1);
            report["checks_passed"]=report.at("after")==reversed&&report.at("reverse").at("docking").value("selection_preserved",false);std::cout<<report.dump(2)<<'\n';return report.at("checks_passed").get<bool>()?0:1;
        }
        auto payload=Json{{"pinned",true},{"protected",true},{"locked",true},{"style",{{"icon",{{"source","builtin:pin-rounded"},{"color","#FF5A36"}}}}}};
        auto native_icon=SendMessageW(tab.window,WM_GETICON,ICON_SMALL,0);
        report["apply"]=send_feature_request(tab,FeatureAction::apply,payload);
        report["native_window_icon_unchanged"]=native_icon==SendMessageW(tab.window,WM_GETICON,ICON_SMALL,0);
        auto peer_opened=send_feature_request(anchor,FeatureAction::open,{{"path",utf8(first.wstring())}});
        auto peer=find(peer_opened.at("opened_guid"));created.push_back(peer);pump(300);
        report["same_path_peer_guid"]=guid_text(peer.guid);
        report["same_path_peer"]=send_feature_request(peer,FeatureAction::inspect);
        report["same_path_peer_close"]=static_cast<int64_t>(exercise(peer,1));
        pump(300); // Explorer rebuilds its tab table asynchronously after removal.
        report["renew_after_peer"]=send_feature_request(tab,FeatureAction::apply,payload);
        report["remove_result"]=static_cast<int64_t>(exercise(tab,1));report["host_close_result"]=static_cast<int64_t>(exercise(tab,2));
        report["still_present"]=guid_text(find(guid_text(tab.guid)).guid)==guid_text(tab.guid);
        report["renew_before_navigation"]=send_feature_request(tab,FeatureAction::apply,payload);
        report["navigation_result"]=static_cast<int64_t>(exercise(tab,3,second));
        report["navigation_immediate"]=send_feature_request(tab,FeatureAction::inspect);pump(300);
        report["after_navigation"]=send_feature_request(tab,FeatureAction::inspect);
        payload["pinned"]=false;report["unpin"]=send_feature_request(tab,FeatureAction::apply,payload);
        report["protected_after_unpin"]=static_cast<int64_t>(exercise(tab,1));
        send_feature_request(tab,FeatureAction::locate);pump(500);
        Json style{{"text",{{"font_family","Segoe UI"},{"font_color","#F04444"},{"bold",true}}},
            {"icon",{{"source","builtin:pin-rounded"},{"color","#FF5A36"}}},
            {"light",{{"background",{{"normal","#FDE8D2"},{"hover","#FFE0A0"},{"selected","#FFC060"}}}}},
            {"dark",{{"background",{{"normal","#4A3830"},{"hover","#604830"},{"selected","#705030"}}}}}};
        Json samples=Json::array();for(int i=0;i<5;++i){try{samples.push_back(send_style_request(tab,StyleAction::apply,{{"style",style},{"description","Pinned / protected / directory locked | 固定 / 保护 / 目录锁定"}}));}catch(const std::exception& ex){samples.push_back({{"error",ex.what()}});}pump(300);}
        report["style_samples"]=samples;report["visual_readback"]=send_style_request(tab,StyleAction::inspect);
        auto lock_style=style;lock_style["icon"]={{"source","builtin:lock-rounded"},{"color","#00AAFF"}};
        Json lock_samples=Json::array();for(int i=0;i<4;++i){lock_samples.push_back(send_style_request(tab,StyleAction::apply,{{"style",lock_style},{"description","Directory locked"}}));pump(200);}
        report["lock_style_samples"]=lock_samples;report["lock_visual_readback"]=send_style_request(tab,StyleAction::inspect);
        Json system_samples=Json::array();for(int i=0;i<3;++i){system_samples.push_back(send_style_request(tab,StyleAction::apply,{{"style",{{"icon",{{"source","system"}}}}}}));pump(200);}
        report["system_style_samples"]=system_samples;report["system_visual_readback"]=send_style_request(tab,StyleAction::inspect);
        // Optional hold allows a human to inspect only this dedicated test tab.
        if(argc>1&&std::string(argv[1])=="hold")for(int i=0;i<30;++i){send_feature_request(tab,FeatureAction::apply,payload);send_style_request(tab,StyleAction::apply,{{"style",style},{"description","Pinned / protected / directory locked | 固定 / 保护 / 目录锁定"}});pump(1000);}
        report["style_restore"]=send_style_request(tab,StyleAction::restore);pump(200);report["restored_visual_readback"]=send_style_request(tab,StyleAction::inspect);
        report["restore"]=send_feature_request(tab,FeatureAction::restore);
        report["lease_apply"]=send_feature_request(tab,FeatureAction::apply,payload);
        report["lease_style_apply"]=send_style_request(tab,StyleAction::apply,{{"style",style},{"description","Lease expiry acceptance"}});
        // Pump the probe STA without renewing either DLL lease. Explorer's own UI
        // timers must release guards and restore values independently of this process.
        pump(11200);
        report["after_lease_expiry"]=send_feature_request(tab,FeatureAction::inspect);
        report["visual_after_lease_expiry"]=send_style_request(tab,StyleAction::inspect);
        report["close_after_restore"]=static_cast<int64_t>(exercise(tab,1));
        // Clean only identities created by this probe, including the redirected tab.
        for(int retry=0;retry<3;++retry){pump(200);scan=enumerator.scan();for(auto& t:scan.tabs)if(t.native&&t.location.filesystem_path&&*t.location.filesystem_path==second.wstring())exercise(*t.native,1);}
        if(isolated_anchor)exercise(*isolated_anchor,1);
        auto cancelled=static_cast<int64_t>(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        report["native_checks_passed"]=report.at("apply").value("protection_confirmed",false)&&report.at("apply").value("native_window_icon_untouched",false)&&report.at("native_window_icon_unchanged")==true
            &&report.at("remove_result")==cancelled&&report.at("host_close_result")==cancelled&&report.at("navigation_result")==cancelled
            &&report.at("still_present")==true&&report.at("protected_after_unpin")==cancelled
            &&report.at("same_path_peer_close")==0&&!report.at("same_path_peer").value("protection_confirmed",false)
            &&report.at("after_navigation").value("redirected",0)==1&&report.at("restore").value("icon_restored",false)
            &&!report.at("after_lease_expiry").value("protection_confirmed",false)&&report.at("close_after_restore")==0;
        report["rendered_icon_checks_passed"]=report.at("visual_readback").value("icon_applied",false)&&report.at("lock_visual_readback").value("icon_applied",false);
        std::cout<<report.dump(2)<<'\n';return report.at("native_checks_passed").get<bool>()&&report.at("rendered_icon_checks_passed").get<bool>()?0:1;
    }catch(const std::exception& e){for(auto& tab:created)try{send_feature_request(tab,FeatureAction::restore);send_style_request(tab,StyleAction::restore);}catch(...){}report["fatal"]=e.what();std::cout<<report.dump(2)<<'\n';return 1;}
}
