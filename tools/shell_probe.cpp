#include "platform/shell_tabs.hpp"
#include "config/config.hpp"
#include <iostream>
#include <map>
#include <exdisp.h>

int main() {
    using namespace sinan;
    try {
        ShellTabEnumerator enumerator;
        Microsoft::WRL::ComPtr<IShellWindows> collection;
        auto collection_hr=CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&collection));
        Json collection_info{{"create_hr",collection_hr}};
        if (SUCCEEDED(collection_hr)) {
            long count=0;collection_info["count_hr"]=collection->get_Count(&count);collection_info["count"]=count;
            collection_info["items"]=Json::array();
            for(long i=0;i<count;++i) {
                VARIANT v{};v.vt=VT_I4;v.lVal=i;Microsoft::WRL::ComPtr<IDispatch> item;
                auto hr=collection->Item(v,&item);Json entry{{"index",i},{"hr",hr},{"has_dispatch",bool(item)}};
                Microsoft::WRL::ComPtr<IWebBrowser2> web;
                if(item&&SUCCEEDED(item.As(&web))) {SHANDLE_PTR handle=0;entry["hwnd_hr"]=web->get_HWND(&handle);entry["hwnd"]=handle;}
                collection_info["items"].push_back(entry);
            }
            Microsoft::WRL::ComPtr<IUnknown> unknown;Microsoft::WRL::ComPtr<IEnumVARIANT> values;
            auto hr=collection->_NewEnum(&unknown);collection_info["enum_hr"]=hr;
            if(SUCCEEDED(hr)&&unknown&&SUCCEEDED(unknown.As(&values))) {
                collection_info["enum"]=Json::array();
                for(int i=0;i<10000;++i) {
                    VARIANT v{};ULONG fetched=0;hr=values->Next(1,&v,&fetched);
                    Json entry{{"hr",hr},{"fetched",fetched},{"type",v.vt}};
                    if(v.vt==VT_DISPATCH&&v.pdispVal) {Microsoft::WRL::ComPtr<IWebBrowser2> web;if(SUCCEEDED(v.pdispVal->QueryInterface(IID_PPV_ARGS(&web)))) {SHANDLE_PTR handle=0;entry["hwnd_hr"]=web->get_HWND(&handle);entry["hwnd"]=handle;}}
                    VariantClear(&v);collection_info["enum"].push_back(entry);if(hr!=S_OK)break;
                }
            }
        }
        auto first = enumerator.scan();
        auto second = enumerator.scan();
        size_t stable = 0;
        Json tabs = Json::array();
        for (const auto& tab : second.tabs) {
            bool matched = false;
            for (const auto& old : first.tabs)
                if (old.com_identity == tab.com_identity && old.browser_window == tab.browser_window) matched = true;
            if (matched) ++stable;
            tabs.push_back({{"window", reinterpret_cast<uintptr_t>(tab.window)}, {"browser", reinterpret_cast<uintptr_t>(tab.browser_window)},
                {"pid", tab.process_id}, {"resolved", tab.location_resolved}, {"filesystem", tab.location.filesystem_path.has_value()},
                {"identity_stable", matched}, {"warning", tab.warning}});
        }
        std::cout << Json{{"complete", first.complete && second.complete}, {"tabs", tabs}, {"stable", stable},
            {"warnings", second.warnings},{"collection",collection_info}}.dump(2) << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
