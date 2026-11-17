#pragma once
#include "agent/tab_tracker.hpp"
#include "hook/features.hpp"
#include "hook/style.hpp"
#include <map>

namespace sinan {
class TabFeatures {
public:
    using FeatureTransport=std::function<Json(const NativeTab&,FeatureAction,const Json&,bool)>;
    using StyleTransport=std::function<Json(const NativeTab&,StyleAction,const Json&,bool)>;
private:
    FeatureTransport native_transport_;
    StyleTransport style_transport_;
    ShellTabTracker::ProcessProbe process_probe_;
    Json request_feature(const NativeTab& tab,FeatureAction action,const Json& value=Json::object(),bool active=false){return native_transport_(tab,action,value,active);}
    Json request_style(const NativeTab& tab,StyleAction action,const Json& value=Json::object(),bool active=false){return style_transport_(tab,action,value,active);}
    Json apply_style(const NativeTab& tab,const Json& value);
    struct Entry { TabFlags flags;Json temporary=Json::object(),report=Json::object();std::optional<NativeTab> controlled;std::string error; };
    std::map<std::string,Entry,std::less<>> entries_;
    std::map<HWND,std::vector<std::string>> unpinned_;
    Json dock(ShellTabTracker& tracker,const Config& config,HWND window);
public:
    explicit TabFeatures(FeatureTransport native=send_feature_request,StyleTransport style=send_style_request,ShellTabTracker::ProcessProbe probe=process_lifetime)
        :native_transport_(std::move(native)),style_transport_(std::move(style)),process_probe_(std::move(probe)){}
    Json list(const ShellTabTracker& tracker,const Config& config) const;
    Json change(ShellTabTracker& tracker,const Config& config,const Json& request,bool active_target=false);
    Json synchronize(ShellTabTracker& tracker,const Config& config,bool enabled);
    Json stop();
};
}
