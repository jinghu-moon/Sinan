#pragma once
#include "hook/layout.hpp"
#include <map>
#include <functional>
#include <tuple>

namespace sinan {
// Width is global, so one lease per Explorer UI thread controls its tab strip(s).
class LayoutTracker {
    using Key = std::tuple<DWORD,uint64_t,DWORD>;
    std::map<Key,NativeTab> controlled_;
public:
    using Transport = std::function<Json(const NativeTab&,LayoutAction,const TabAppearance&)>;
    using Thread = std::function<DWORD(HWND)>;
    using Probe = std::function<ProcessLifetime(DWORD,uint64_t)>;
    Json synchronize(const std::vector<NativeTab>& observed, bool enabled, const TabAppearance& appearance,
        const Transport& transport = send_layout_request,
        const Thread& thread = [](HWND window){return GetWindowThreadProcessId(window,nullptr);},
        const Probe& probe = process_lifetime);
};
}
