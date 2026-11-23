#pragma once
#include "hook/features.hpp"
#include "compat/symbols.hpp"

namespace sinan {
// Shared by sender and injected receiver. No whole-feature SHA gate.
inline std::string feature_admission_error(const compat::Profile& p,FeatureAction action,const Json& payload,bool require_active){
    if(!p.inventory)return "Native inventory unavailable: "+p.reason;
    auto failure=[&](const char* key){return std::string("Native capability unavailable: ")+key+"; "+p.failures.value(key,std::string("layout contract not verified"));};
    if(require_active&&!p.selection)return failure("selection");
    if(action==FeatureAction::locate&&!p.selection)return failure("selection");
    if(action==FeatureAction::open){if(!p.opening)return failure("opening");if(payload.value("select",false)&&!p.selection)return failure("selection");}
    if(action==FeatureAction::reorder&&!p.reorder)return failure("reorder");
    if(action==FeatureAction::close_duplicate){if(!p.close_guard)return failure("close_guard");if(!p.selection)return failure("selection");}
    if((action==FeatureAction::external_tabs||action==FeatureAction::external_activation)&&(p.identity.value("sha256",std::string{})!=supported_frame_sha256||
        !p.selection||!p.opening||!p.close_guard))
        return "External folder routing requires the verified ExplorerFrame 26100.8457 adapter";
    if(action==FeatureAction::apply){
        if((payload.value("pinned",false)||payload.value("protected",false))&&!p.close_guard)return failure("close_guard");
        if(payload.value("locked",false)&&!p.navigation_guard)return failure("navigation_guard");
    }
    return {};
}
}
