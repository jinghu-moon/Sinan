#include "agent/features.hpp"
#include "hook/style.hpp"
#include <set>
#include <algorithm>
#include "policy/entries.hpp"

namespace tabtail {
namespace {
std::string state_description(const TabFlags& flags,const std::wstring& path,const std::string& language) {
    bool zh=language!="en";std::string out;
    auto add=[&](const char* text){if(!out.empty())out+=" | ";out+=text;};
    if(flags.pinned)add(zh?"固定标签":"Pinned tab");if(flags.protected_tab)add(zh?"独立关闭保护":"Independent close protection");
    if(flags.locked)add(zh?"目录已锁定":"Directory locked");
    if(!out.empty()&&!path.empty())out+="\n"+utf8(path);return out;
}
}
Json TabFeatures::apply_style(const NativeTab& tab,const Json& value) {
    auto reply=request_style(tab,StyleAction::apply,value);auto deadline=GetTickCount64()+1200;
    while(!reply.value("style_applied",false)&&(reply.value("pending",false)||reply.value("error",std::string{}).starts_with("Waiting for"))&&GetTickCount64()<deadline) {
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(100);
        reply=request_style(tab,StyleAction::apply,value);
    }
    return reply;
}

Json TabFeatures::list(const ShellTabTracker& tracker,const Config& config) const {
    auto tabs=tracker.list();
    for(auto& tab:tabs) {
        auto it=entries_.find(tab.at("tab_id").get<std::string>());Entry empty;auto& e=it==entries_.end()?empty:it->second;
        tab["behavior"]=flags_json(e.flags);tab["behavior"]["actual"]=e.report;tab["behavior"]["error"]=e.error;
        auto path=tab.at("path").is_string()?wide(tab.at("path").get<std::string>()):L"";
        tab["style"]=resolve_style(config.features,e.flags,path,e.temporary);tab["temporary_style"]=e.temporary;
    }
    return tabs;
}
Json TabFeatures::dock(ShellTabTracker& tracker,const Config& config,HWND window) {
    auto side=config.features.at("tab_behavior").value("pinned_position",std::string("keep"));
    if(side=="keep"){unpinned_.erase(window);return {{"position",side},{"order_confirmed",true},{"moved",0}};}
    std::vector<std::string> pins;std::optional<NativeTab> anchor;
    for(auto& t:tracker.list())try {
        auto id=t.at("tab_id").get<std::string>();auto native=tracker.native_tab(id);if(native.window!=window)continue;anchor=native;
        if(entries_.contains(id)&&entries_.at(id).flags.pinned)pins.push_back(guid_text(native.guid));
    }catch(const Error&){}
    if(pins.empty()&&unpinned_[window].empty()){unpinned_.erase(window);return {{"position",side},{"order_confirmed",true},{"moved",0}};}
    if(!anchor)throw Error(5,"Docking window is no longer observed");
    auto report=request_style(*anchor,StyleAction::order);auto current=report.at("order").get<std::vector<std::string>>();
    auto desired=dock_order(current,pins,side,unpinned_[window]);
    if(current==desired){unpinned_.erase(window);return {{"position",side},{"order",current},{"order_confirmed",true},{"moved",0}};}
    auto result=request_feature(*anchor,FeatureAction::reorder,{{"expected",current},{"order",desired}}).at("docking");
    bool confirmed=false;
    for(int retry=0;retry<5&&!confirmed;++retry) {
        auto actual=request_style(*anchor,StyleAction::order);confirmed=actual.contains("order")&&actual.at("order")==Json(desired);
        if(!confirmed){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(50);}
    }
    if(!confirmed)throw Error(5,"Explorer TabItems order did not confirm docking");
    result["order_confirmed"]=true;result["order"]=desired;result["verification"]="TabView.TabItems / native GUID bijection";
    unpinned_.erase(window);result["position"]=side;return result;
}
Json TabFeatures::change(ShellTabTracker& tracker,const Config& config,const Json& request,bool active_target) {
    auto id=request.at("tab_id").get<std::string>();auto tab=tracker.native_tab(id);
    auto command=request.at("command").get<std::string>();
    if(command=="tabs.locate") {
        AllowSetForegroundWindow(tab.process_id);
        auto report=request_feature(tab,FeatureAction::locate);
        if(IsIconic(tab.window))ShowWindowAsync(tab.window,SW_RESTORE);
        report["foreground_activated"]=SetForegroundWindow(tab.window)!=FALSE;return report;
    }
    if(command=="tabs.visual-probe")return request_style(tab,StyleAction::inspect);
    if(command=="tabs.capabilities") {auto report=request_feature(tab,FeatureAction::inspect);try{auto appearance=request_style(tab,StyleAction::inspect);report["appearance"]=appearance;report["icon_applied"]=appearance.value("icon_applied",false);}catch(const std::exception& ex){report["appearance"]={{"error",ex.what()}};}return report;}
    Entry next=entries_[id];
    if(command=="tabs.pin")next.flags.pinned=request.at("enabled").get<bool>();
    else if(command=="tabs.protect")next.flags.protected_tab=request.at("enabled").get<bool>();
    else if(command=="tabs.lock")next.flags.locked=request.at("enabled").get<bool>();
    else if(command=="tabs.style"){next.temporary=request.at("style");validate_style(next.temporary);}
    else if(command=="tabs.style-reset")next.temporary=Json::object();
    else throw Error(2,"Unknown tab feature command");
    std::wstring path;
    for(auto& t:tracker.list())if(t.at("tab_id")==id&&t.at("path").is_string())path=wide(t.at("path").get<std::string>());
    auto payload=flags_json(next.flags);payload["language"]=config.language;payload["style"]=resolve_style(config.features,next.flags,path,next.temporary);
    // An unconfirmed request can have executed. Keep the target for stop/lease cleanup,
    // but commit the requested flags only after a response from the target UI thread.
    entries_[id].controlled=tab;
    auto report=request_feature(tab,FeatureAction::apply,payload,active_target);
    if(!report.value("protection_confirmed",false))throw Error(5,"Target did not confirm native protection state");
    try {report["appearance"]=apply_style(tab,{{"style",payload.at("style")},{"description",state_description(next.flags,path,config.language)}});}
    catch(const std::exception& ex){report["appearance"]={{"style_applied",false},{"error",ex.what()}};}
    report["icon_applied"]=report.at("appearance").value("icon_applied",false);
    if(entries_[id].flags.pinned&&!next.flags.pinned)unpinned_[tab.window].push_back(guid_text(tab.guid));
    next.controlled=tab;next.report=report;next.error.clear();entries_[id]=next;
    if(command=="tabs.pin")try{report["docking"]=dock(tracker,config,tab.window);}catch(const std::exception& ex){report["docking"]={{"order_confirmed",false},{"error",ex.what()}};}
    report["tab_id"]=id;report["state_confirmed"]=true;
    auto icon=payload.at("style").value("icon",Json::object());bool needs_icon=icon.value("source",std::string("system"))!="system";
    report["appearance_confirmed"]=report.at("appearance").value("style_applied",false)&&(!needs_icon||report.value("icon_applied",false));
    report["partial"]=!report.at("appearance_confirmed").get<bool>()||(report.contains("docking")&&!report.at("docking").value("order_confirmed",false));
    report["appearance_pending"]=report.at("appearance").value("pending",false)||report.at("appearance").value("error",std::string{}).starts_with("Waiting for");
    report["message"]=report.at("partial").get<bool>()?"Behavior confirmed; appearance is pending or failed. See appearance.error.":"Behavior and appearance confirmed by target adapters; visual acceptance remains separate.";
    return report;
}
Json TabFeatures::synchronize(ShellTabTracker& tracker,const Config& config,bool enabled) {
    if(!enabled)return stop();
    // Dragging may replace the Shell observation token while retaining the
    // native GUID. Transfer only within the identical Explorer lifetime.
    auto live=tracker.list();
    for(auto& t:live){auto id=t.at("tab_id").get<std::string>();if(entries_.contains(id))continue;
        try{auto native=tracker.native_tab(id);auto old=std::find_if(entries_.begin(),entries_.end(),[&](auto& entry){auto& control=entry.second.controlled;return control&&control->guid==native.guid&&control->process_id==native.process_id&&control->process_started==native.process_started&&std::none_of(live.begin(),live.end(),[&](auto& tab){return tab.at("tab_id")==entry.first&&tab.value("observed",false);});});
            if(old!=entries_.end()){auto moved=old->second;try{request_feature(*moved.controlled,FeatureAction::restore);}catch(...){}try{request_style(*moved.controlled,StyleAction::restore);}catch(...){}entries_.erase(old);moved.controlled=native;entries_[id]=std::move(moved);}
        }catch(...){}
    }
    std::set<std::string> observed;size_t failed=0,applied=0;
    for(auto& t:tracker.list()) {
        auto id=t.at("tab_id").get<std::string>();observed.insert(id);
        auto& e=entries_[id];auto path=t.at("path").is_string()?wide(t.at("path").get<std::string>()):L"";
        auto style=resolve_style(config.features,e.flags,path,e.temporary);
        if(!e.flags.pinned&&!e.flags.protected_tab&&!e.flags.locked&&style.empty()&&!e.controlled)continue;
        try {
            auto tab=tracker.native_tab(id);auto payload=flags_json(e.flags);payload["language"]=config.language;payload["style"]=style;e.controlled=tab;
            e.report=request_feature(tab,FeatureAction::apply,payload);
            try{e.report["appearance"]=request_style(tab,StyleAction::apply,{{"style",style},{"description",state_description(e.flags,path,config.language)}});}catch(const std::exception& ex){e.report["appearance"]={{"style_applied",false},{"error",ex.what()}};}
            e.report["icon_applied"]=e.report.at("appearance").value("icon_applied",false);
            auto icon=style.value("icon",Json::object());
            if(!e.report.at("appearance").value("style_applied",false)||(icon.value("source",std::string("system"))!="system"&&!e.report.value("icon_applied",false))){e.error="Behavior confirmed; appearance is incomplete";++failed;}else e.error.clear();++applied;
        }catch(const std::exception& ex){e.error=ex.what();++failed;}
    }
    // Do not renew a lease from a stale observation. Explorer will release it in 10s.
    for(auto it=entries_.begin();it!=entries_.end();) {
        if(!observed.contains(it->first)){if(it->second.controlled){try{request_feature(*it->second.controlled,FeatureAction::restore);}catch(...){}try{request_style(*it->second.controlled,StyleAction::restore);}catch(...){}}it=entries_.erase(it);}else ++it;
    }
    Json docking=Json::array();std::set<HWND> windows;
    for(auto& native:tracker.native_tabs())windows.insert(native.window);
    for(auto window:windows)try{auto result=dock(tracker,config,window);result["window"]=reinterpret_cast<uint64_t>(window);docking.push_back(result);}catch(const std::exception& ex){++failed;docking.push_back({{"window",reinterpret_cast<uint64_t>(window)},{"order_confirmed",false},{"error",ex.what()}});}
    return {{"applied",applied},{"failed",failed},{"complete",failed==0},{"docking",docking}};
}
Json TabFeatures::stop() {
    size_t failed=0,restored=0;Json results=Json::array();
    for(auto& [id,e]:entries_) {
        if(!e.controlled)continue;
        Json result{{"tab_id",id},{"behavior_restored",false},{"appearance_restored",false},{"icon_restored",false}};
        auto ended=process_probe_(e.controlled->process_id,e.controlled->process_started)==ProcessLifetime::ended;
        if(ended){result["process_ended"]=true;result["icon_restored"]=true;result["behavior_restored"]=true;result["appearance_restored"]=true;}
        else {
            try{auto reply=request_feature(*e.controlled,FeatureAction::restore);result["behavior_restored"]=true;result["icon_restored"]=reply.value("icon_restored",false);if(!result["icon_restored"].get<bool>())result["icon_error"]=reply.value("icon_error",std::string("Original icon readback not confirmed"));}catch(const std::exception& ex){result["behavior_error"]=ex.what();}
            try{auto response=request_style(*e.controlled,StyleAction::restore);result["appearance_restored"]=response.value("restoration_confirmed",false);if(!result["appearance_restored"].get<bool>())result["appearance_error"]=response.value("error",std::string("Appearance restoration is not confirmed"));}catch(const std::exception& ex){result["appearance_error"]=ex.what();}
        }
        result["icon_restored"]=result.at("icon_restored").get<bool>()&&result.at("appearance_restored").get<bool>();
        if(result.at("behavior_restored").get<bool>())e.flags={};
        if(result.at("appearance_restored").get<bool>())e.temporary=Json::object();
        if(result.at("behavior_restored").get<bool>()&&result.at("appearance_restored").get<bool>()&&result.at("icon_restored").get<bool>()){e.controlled.reset();e.report=Json::object();e.error.clear();++restored;}
        else {e.report=result;e.error="Restoration incomplete; expired leases also release remaining native state";++failed;}
        results.push_back(result);
    }
    return {{"restored",restored},{"failed",failed},{"complete",failed==0},{"tabs",results}};
}
}
