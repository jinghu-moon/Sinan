#include "platform/shell_tabs.hpp"
#include "platform/explorer.hpp"
#include "platform/shell_collection.hpp"
#include <exdisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <servprov.h>
#include <map>
#include <atomic>
#include <set>
#include <algorithm>
#include "hook/features.hpp"

namespace sinan {
using Microsoft::WRL::ComPtr;
namespace {
class NativeObservationIdentity final:public IUnknown {
    std::atomic<ULONG> refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override{auto count=--refs_;if(!count)delete this;return count;}
};
std::string hr_error(std::string_view action, HRESULT hr) {
    return system_error(action, static_cast<DWORD>(hr));
}
struct TaskMemory {
    void* value = nullptr;
    ~TaskMemory() { CoTaskMemFree(value); }
};
HRESULT resolve_browser(IWebBrowser2& web, HWND top, DWORD pid, ComPtr<IShellBrowser>& browser, HWND& child) {
    ComPtr<IServiceProvider> services;
    auto hr=web.QueryInterface(IID_PPV_ARGS(&services));
    if(FAILED(hr))return hr;
    hr=services->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser));
    if(FAILED(hr))hr=services->QueryService(IID_IShellBrowser,IID_PPV_ARGS(&browser));
    if(FAILED(hr)||!browser)return FAILED(hr)?hr:E_NOINTERFACE;
    hr=browser->GetWindow(&child);
    DWORD child_pid{};
    GetWindowThreadProcessId(child,&child_pid);
    if(FAILED(hr)||!child||child==top||child_pid!=pid||!IsChild(top,child))return E_FAIL;
    return S_OK;
}
HRESULT read_location(IShellBrowser* browser, TabLocation& location) {
    ComPtr<IShellView> view;
    auto hr=browser->QueryActiveShellView(&view);
    return FAILED(hr)||!view?(FAILED(hr)?hr:E_NOINTERFACE):shell_view_location(*view.Get(),location);
}
}
HRESULT shell_view_location(IShellView& view, TabLocation& location) {
    ComPtr<IFolderView> folder_view;
    auto hr = view.QueryInterface(IID_PPV_ARGS(&folder_view));
    if (FAILED(hr)) return hr;
    ComPtr<IPersistFolder2> folder;
    hr = folder_view->GetFolder(IID_PPV_ARGS(&folder));
    if (FAILED(hr)) return hr;
    PIDLIST_ABSOLUTE pidl = nullptr;
    hr = folder->GetCurFolder(&pidl);
    TaskMemory pidl_owner{pidl};
    if (hr != S_OK || !pidl) return FAILED(hr) ? hr : E_FAIL;
    ComPtr<IShellItem> item;
    hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    PWSTR text = nullptr;
    hr = item->GetDisplayName(SIGDN_NORMALDISPLAY, &text);
    TaskMemory name_owner{text};
    if (FAILED(hr) || !text) return FAILED(hr) ? hr : E_FAIL;
    location.original_title = text;
    text = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &text);
    TaskMemory path_owner{text};
    if (SUCCEEDED(hr) && text && *text) location.filesystem_path = text;
    // A virtual folder is successfully resolved even though no filesystem path exists.
    return S_OK;
}
ShellTabEnumerator::ShellTabEnumerator() {
    auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) throw Error(1, hr_error("初始化 Shell COM apartment", hr));
    initialized_ = true;
}
ShellTabEnumerator::~ShellTabEnumerator() { if (initialized_) CoUninitialize(); }
ComPtr<IShellView> ShellTabEnumerator::view(HWND frame, HWND browser_window, bool require_foreground,
    ComPtr<IShellBrowser>* owner, ComPtr<IUnknown>* identity) {
    DWORD pid{};
    GetWindowThreadProcessId(frame,&pid);
    const auto processes=explorer_processes();
    const auto process=std::find_if(processes.begin(),processes.end(),[&](const auto& item){return item.pid==pid;});
    if(process==processes.end())
        throw Error(3,"No verified Explorer window");
    const bool active_only=!browser_window||require_foreground;
    auto selected_child=[&] {
        if(native_selection_supported())try {
            auto tabs=read_native_tabs(frame,pid,process->created);
            if(!tabs.empty())if(auto guid=read_active_tab(tabs.front())) {
                auto tab=std::find_if(tabs.begin(),tabs.end(),[&](const auto& item){return item.guid==*guid;});
                if(tab!=tabs.end())return tab->browser_window;
            }
        }catch(const Error&){}
        return FindWindowExW(frame,nullptr,L"ShellTabWindowClass",nullptr);
    };
    const auto active_child=active_only?selected_child():nullptr;
    if(active_child&&browser_window&&active_child!=browser_window)
        throw Error(5,"Explorer selected tab changed");
    ComPtr<IShellWindows> windows;
    const auto hr=CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows));
    if(FAILED(hr))throw Error(5,hr_error("Read Explorer views",hr));
    auto collection=read_shell_collection(*windows.Get());
    ComPtr<IShellView> result;
    ComPtr<IShellBrowser> matched_browser;
    ComPtr<IUnknown> matched_identity;
    HWND matched{};
    for(const auto& dispatch:collection.windows) {
        ComPtr<IWebBrowser2> web;
        SHANDLE_PTR top{};
        if(FAILED(dispatch.As(&web))||FAILED(web->get_HWND(&top))||reinterpret_cast<HWND>(top)!=frame)continue;
        ComPtr<IShellBrowser> browser;
        HWND child{};
        if(FAILED(resolve_browser(*web.Get(),frame,pid,browser,child)))continue;
        if(browser_window&&child!=browser_window)continue;
        if(active_child&&child!=active_child)continue;
        if(active_only&&!active_child&&!IsWindowVisible(child))continue;
        if(matched==child)continue;
        if(matched)throw Error(5,"More than one Explorer tab is visible");
        if(FAILED(browser->QueryActiveShellView(&result))||!result)throw Error(5,"Explorer tab view is unavailable");
        if(identity&&FAILED(web.As(&matched_identity)))throw Error(5,"Explorer tab identity is unavailable");
        matched_browser=browser;
        matched=child;
    }
    if(!result||(active_only&&(GetForegroundWindow()!=frame||
        (active_child?selected_child()!=matched:!IsWindowVisible(matched)))))
        throw Error(5,"Explorer tab view changed or is unavailable");
    if(owner)*owner=std::move(matched_browser);
    if(identity)*identity=std::move(matched_identity);
    return result;
}
ShellTabScan ShellTabEnumerator::scan(bool include_native) {
    ShellTabScan result;
    std::map<HWND, std::vector<NativeTab>> native_windows;
    std::map<DWORD, uint64_t> allowed;
    for (auto process : explorer_processes()) allowed.emplace(process.pid, process.created);
    ComPtr<IShellWindows> windows;
    auto hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    if (FAILED(hr)) throw Error(1, hr_error("枚举 Shell 标签", hr));
    auto collection=read_shell_collection(*windows.Get());
    result.complete=collection.complete;result.warnings=std::move(collection.warnings);
    for (const auto& dispatch:collection.windows) {
        ComPtr<IWebBrowser2> web;
        if (FAILED(dispatch.As(&web))) continue;
        SHANDLE_PTR handle = 0;
        hr = web->get_HWND(&handle);
        if (FAILED(hr)) { result.complete = false; result.warnings.push_back(hr_error("读取 Shell 窗口", hr)); continue; }
        auto top = reinterpret_cast<HWND>(handle);
        DWORD pid = 0; GetWindowThreadProcessId(top, &pid);
        if (!IsWindow(top) || !allowed.contains(pid)) continue;
        ComPtr<IShellBrowser> browser;
        HWND child = nullptr;
        hr=resolve_browser(*web.Get(),top,pid,browser,child);
        if (FAILED(hr)) {
            result.complete = false; result.warnings.push_back("未能验证独立标签子窗口，未将顶层窗口当成标签"); continue;
        }
        ShellTabObservation tab;
        tab.window = top; tab.browser_window = child; tab.process_id = pid; tab.process_started = allowed.at(pid);
        hr = web.As(&tab.com_identity);
        if (FAILED(hr)) { result.complete = false; result.warnings.push_back(hr_error("取得标签 COM 身份", hr)); continue; }
        hr = read_location(browser.Get(), tab.location);
        tab.location_resolved = SUCCEEDED(hr);
        if (FAILED(hr)) {
            tab.warning = hr_error("标签位置暂不可读取", hr);
            BSTR name = nullptr;
            if (SUCCEEDED(web->get_LocationName(&name)) && name) tab.location.original_title.assign(name, SysStringLen(name));
            SysFreeString(name);
        }
        try {
            if (!native_windows.contains(top)) native_windows.emplace(top, read_native_tabs(top, pid, tab.process_started));
            for (const auto& native : native_windows.at(top)) if (native.browser_window == child) tab.native = native;
        } catch (const std::exception& e) {
            if (tab.warning.empty()) tab.warning = e.what();
        }
        result.tabs.push_back(std::move(tab));
    }
    long after=0;
    if(windows->get_Count(&after)!=S_OK||after!=collection.registration_count) {
        result.complete=false;result.warnings.push_back("Shell 集合在读取标签位置期间变化，保留上次未确认关闭的标签");
    }
    if(include_native&&native_inventory_supported()) {
        result.native_inventory=true;
        std::vector<HWND> frames;EnumWindows([](HWND window,LPARAM value)->BOOL{wchar_t name[64]{};GetClassNameW(window,name,64);if(wcscmp(name,L"CabinetWClass")==0)reinterpret_cast<std::vector<HWND>*>(value)->push_back(window);return TRUE;},reinterpret_cast<LPARAM>(&frames));
        std::set<std::string> seen;
        for(auto window:frames) {
            DWORD pid{};GetWindowThreadProcessId(window,&pid);if(!allowed.contains(pid))continue;
            try {
                auto native=read_native_tabs(window,pid,allowed.at(pid));if(native.empty())continue;
                auto inventory=send_feature_request(native.front(),FeatureAction::inspect,{{"inventory",true}}).at("tabs");
                for(auto& tab:native) {
                    auto guid=guid_text(tab.guid);auto row=std::find_if(inventory.begin(),inventory.end(),[&](auto& r){return r.at("guid")==guid;});
                    if(row==inventory.end())throw Error(5,"Native inventory changed during reading");
                    auto existing=std::find_if(result.tabs.begin(),result.tabs.end(),[&](auto& t){return t.process_id==pid&&t.process_started==tab.process_started&&t.browser_window==tab.browser_window;});
                    if(existing==result.tabs.end()) {
                        auto key=std::to_string(pid)+":"+std::to_string(tab.process_started)+":"+std::to_string(reinterpret_cast<uintptr_t>(window))+":"+guid;seen.insert(key);
                        auto& identity=native_identities_[key];if(!identity)identity.Attach(new NativeObservationIdentity);
                        ShellTabObservation observation;observation.window=window;observation.browser_window=tab.browser_window;observation.process_id=pid;observation.process_started=tab.process_started;observation.com_identity=identity;observation.native=tab;observation.native_only=true;
                        result.tabs.push_back(std::move(observation));existing=std::prev(result.tabs.end());
                    }
                    existing->native=tab;existing->location_resolved=row->value("location_resolved",false);existing->location.filesystem_path.reset();
                    if(row->at("path").is_string())existing->location.filesystem_path=wide(row->at("path").get<std::string>());
                    existing->location.original_title=wide(row->value("original_title",std::string{}));existing->warning=row->value("error",std::string{});
                    if(!existing->location_resolved){result.complete=false;result.warnings.push_back("Native tab location is unresolved: "+guid);}
                }
            }catch(const std::exception& ex){result.complete=false;result.warnings.push_back(std::string("Native tab inventory: ")+ex.what());}
        }
        if(result.complete)std::erase_if(native_identities_,[&](auto& item){return !seen.contains(item.first);});
    }
    return result;
}
}
