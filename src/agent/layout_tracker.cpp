#include "agent/layout_tracker.hpp"

namespace sinan {
Json LayoutTracker::synchronize(const std::vector<NativeTab>& observed,bool enabled,const TabAppearance& appearance,
    const Transport& transport,const Thread& thread,const Probe& probe) {
    validate_appearance(appearance);
    const bool customize=enabled&&appearance.width_mode!="system";
    std::map<Key,NativeTab> targets;
    for(const auto& tab:observed) {
        auto tid=thread(tab.window);if(tid)targets[{tab.process_id,tab.process_started,tid}]=tab;
    }
    // Confirmed process exits retire leases without sending to a reused HWND/PID.
    for(auto it=controlled_.begin();it!=controlled_.end();) {
        const auto& tab=it->second;
        if(probe(tab.process_id,tab.process_started)==ProcessLifetime::ended)it=controlled_.erase(it);else ++it;
    }
    Json results=Json::array();size_t confirmed=0,failed=0,pending=0;
    if(!customize)for(const auto& [key,tab]:controlled_)if(!targets.contains(key)) {
        // No current observation: let the in-process lease expire and restore.
        ++pending;results.push_back({{"pid",tab.process_id},{"pending",true},{"message","Waiting for lease restoration"}});
    }
    for(const auto& [key,tab]:targets) {
        if(!customize&&!controlled_.contains(key))continue; // Default/observe never injects.
        try {
            if(customize)controlled_[key]=tab; // Keep for restoration even after a partial apply.
            auto report=transport(tab,customize?LayoutAction::apply:LayoutAction::restore,appearance);
            report["pid"]=tab.process_id;report["thread"]=std::get<2>(key);
            if(report.value("pending",false)||!report.value("layout_applied",false))++pending;
            else {++confirmed;if(!customize)controlled_.erase(key);}
            results.push_back(std::move(report));
        }catch(const std::exception& error){++failed;results.push_back({{"pid",tab.process_id},{"error",error.what()},{"layout_applied",false}});}
    }
    return {{"mode",customize?appearance.width_mode:"system"},{"requested_mode",appearance.width_mode},
        {"complete",failed==0&&pending==0},{"layout_applied",customize&&confirmed>0&&failed==0&&pending==0},
        {"confirmed_threads",confirmed},{"failed_threads",failed},{"pending_threads",pending},{"threads",results}};
}
}
