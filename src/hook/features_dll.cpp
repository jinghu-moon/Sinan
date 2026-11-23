#include "hook/features.hpp"
#include "hook/external_tabs.hpp"
#include "hook/feature_admission.hpp"
#include "hook/identity.hpp"
#include "platform/close_dialog.hpp"
#include "policy/entries.hpp"
#include <MinHook.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <deque>
#include <cstring>
#include <atomic>
#include <thread>
#include <map>
#include <future>
#include <servprov.h>
#include <array>

namespace sinan::features {
using Microsoft::WRL::ComPtr;
HMODULE self{};
using Remove=HRESULT(WINAPI*)(void*,GUID);
using Close=HRESULT(WINAPI*)(void*,bool);
using Navigate=HRESULT(WINAPI*)(void*,PCIDLIST_ABSOLUTE,DWORD,DWORD);
Remove original_remove{};Close original_close{};Navigate original_navigate{};
std::mutex setup;
template<class T>T read(uintptr_t address) {
    T value{};SIZE_T n{};
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(value),&n)||n!=sizeof(value))throw Error(3,"Native field unavailable");
    return value;
}
struct DirectoryHealth {std::atomic<int> status{1};std::atomic<bool> pending{false};std::atomic<uint64_t> checked{0};};
struct State {
    NativeTab tab;TabFlags flags;DWORD owner{};uint64_t owner_started{},expires{};
    uintptr_t browser{};std::wstring locked_path;ComPtr<IShellItem> locked_item;
    std::string directory_identity;std::shared_ptr<DirectoryHealth> directory_health;Json style=Json::object();
    std::string language="auto";
    uint64_t blocked_close=0,blocked_navigation=0,redirected=0;std::string error;bool restoring=false;
};
thread_local std::vector<State> states;
thread_local UINT_PTR timer{};
thread_local bool opening=false;
thread_local std::map<HWND,uint64_t> explicit_windows;
bool explicit_close_allowed(HWND window){auto it=explicit_windows.find(window);return it!=explicit_windows.end()&&it->second>GetTickCount64();}
thread_local bool in_prompt=false;
thread_local CloseDialogState close_dialog;
struct PendingNavigation {NativeTab source;PIDLIST_ABSOLUTE target{};};
thread_local std::deque<PendingNavigation> pending;
bool valid(const State& s);
State* state_for(uintptr_t host) {
    auto it=std::find_if(states.begin(),states.end(),[&](auto& s){return s.tab.host==host&&s.expires>GetTickCount64();});
    return it==states.end()||!valid(*it)?nullptr:&*it;
}
NativeTab locate(const FeaturePacket& p) {
    auto window=reinterpret_cast<HWND>(p.window);DWORD pid{};
    if(GetWindowThreadProcessId(window,&pid)!=GetCurrentThreadId()||pid!=GetCurrentProcessId()||bridge_process_started(GetCurrentProcess())!=p.target_started)throw Error(4,"Target UI thread or process identity changed");
    auto tabs=read_native_tabs(window,pid,p.target_started);
    auto found=std::find_if(tabs.begin(),tabs.end(),[&](auto& t){return t.guid==p.guid&&reinterpret_cast<uintptr_t>(t.browser_window)==p.browser_window;});
    if(found==tabs.end())throw Error(3,"Target tab identity no longer exists");
    if(p.require_active&&(!native_selection_supported()||GetForegroundWindow()!=window||read<GUID>(found->frame+native_field("active_guid"))!=found->guid))throw Error(3,"Foreground active tab changed; operation cancelled");
    return *found;
}
ComPtr<IShellItem> location(const NativeTab& tab) {
    using Get=HRESULT(WINAPI*)(void*,REFIID,void**);ComPtr<IShellItem> item;
    auto hr=reinterpret_cast<Get>(tab.module_base+native_rva("location"))(reinterpret_cast<void*>(tab.host+0x10),IID_PPV_ARGS(&item));
    if(FAILED(hr)||!item)throw Error(3,"Tab location is unavailable");return item;
}
void* actions_object(const NativeTab& tab,size_t slot,const char* function) {
    auto object=tab.frame+native_field("actions");
    auto vt=read<uintptr_t>(object);
    if(vt!=tab.module_base+native_rva("actions_vtable")||read<uintptr_t>(vt+slot*sizeof(uintptr_t))!=tab.module_base+native_rva(function))throw Error(3,"Live frame action vtable changed");
    return reinterpret_cast<void*>(object);
}
HRESULT select_tab(const NativeTab& tab,GUID guid) {
    auto object=actions_object(tab,5,"select");
    constexpr std::array<BYTE,11> expected{0x4c,0x8b,0xdc,0x49,0x89,0x5b,0x18,0x49,0x89,0x73,0x20};
    if(read<std::array<BYTE,11>>(tab.module_base+native_rva("select"))!=expected)throw Error(3,"Live selection function changed or is hooked");
    using Select=HRESULT(WINAPI*)(void*,GUID);return reinterpret_cast<Select>(tab.module_base+native_rva("select"))(object,guid);
}
HRESULT add_tab(const NativeTab& tab,PCIDLIST_ABSOLUTE target,GUID* guid) {
    auto object=actions_object(tab,3,"add");actions_object(tab,9,"add_index");
    constexpr std::array<BYTE,16> expected{0x48,0x83,0xec,0x38,0x48,0x8b,0x01,0x4c,0x89,0x4c,0x24,0x20,0x41,0x83,0xc9,0xff};
    if(read<std::array<BYTE,16>>(tab.module_base+native_rva("add"))!=expected)throw Error(3,"Live AddTab function changed or is hooked");
    using Add=HRESULT(WINAPI*)(void*,PCIDLIST_ABSOLUTE,int,GUID*);return reinterpret_cast<Add>(tab.module_base+native_rva("add"))(object,target,0,guid);
}
ComPtr<IShellView> external_tab_view(const NativeTab& tab) {
    const auto host=tab.host+0x10;
    auto vtable=read<uintptr_t>(host);
    const auto getter=tab.module_base+native_rva("get_browser");
    if(vtable!=tab.module_base+native_rva("host_interface")||read<uintptr_t>(vtable+5*sizeof(uintptr_t))!=getter)
        return {};
    using Get=HRESULT(WINAPI*)(void*,REFIID,void**);
    ComPtr<IServiceProvider> provider;
    if(FAILED(reinterpret_cast<Get>(getter)(reinterpret_cast<void*>(host),IID_PPV_ARGS(&provider)))||!provider)return {};
    ComPtr<IShellBrowser> browser;
    if(FAILED(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser)))||!browser)return {};
    HWND child{};
    if(FAILED(browser->GetWindow(&child))||child!=tab.browser_window)return {};
    ComPtr<IShellView> view;
    if(FAILED(browser->QueryActiveShellView(&view)))return {};
    return view;
}
bool discard_external_tab(const NativeTab& tab) {
    const auto address=tab.module_base+native_rva("remove");
    constexpr std::array<BYTE,11> expected{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55};
    {
        std::scoped_lock lock(setup);
        if(!original_remove&&read<std::array<BYTE,11>>(address)!=expected)return false;
    }
    return SUCCEEDED(reinterpret_cast<Remove>(address)(actions_object(tab,4,"remove"),tab.guid));
}
uintptr_t navigation_object(const NativeTab& tab) {
    auto object=tab.host+0x10;auto vt=read<uintptr_t>(object);
    if(vt!=tab.module_base+native_rva("host_interface")||read<uintptr_t>(vt+5*sizeof(uintptr_t))!=tab.module_base+native_rva("get_browser"))throw Error(3,"Live browser getter vtable changed");
    constexpr std::array<BYTE,8> expected{0x48,0x83,0xec,0x28,0x49,0x83,0x20,0x00};
    if(read<std::array<BYTE,8>>(tab.module_base+native_rva("get_browser"))!=expected)throw Error(3,"Live browser getter changed or is hooked");
    using Get=HRESULT(WINAPI*)(void*,REFIID,void**);ComPtr<IShellBrowser> browser;
    if(FAILED(reinterpret_cast<Get>(tab.module_base+native_rva("get_browser"))(reinterpret_cast<void*>(object),IID_PPV_ARGS(&browser)))||!browser)throw Error(3,"Native browser interface unavailable");
    auto iface=reinterpret_cast<uintptr_t>(browser.Get());auto offset=native_field("browser_interface");
    if(iface<offset||read<uintptr_t>(iface)!=tab.module_base+native_rva("browser_interface")||read<uintptr_t>(iface-offset)!=tab.module_base+native_rva("browser_vtable"))throw Error(3,"Live navigation object identity mismatch");
    return iface-offset;
}
std::wstring path_of(IShellItem* item) {
    PWSTR text{};if(FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&text))||!text)throw Error(3,"Only filesystem directories can be locked");
    std::wstring path(text);CoTaskMemFree(text);return path;
}
std::string directory_id(const std::wstring& path) {
    Handle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if(!h||!GetFileInformationByHandle(h.get(),&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))throw Error(3,"Directory identity is unavailable; re-lock the directory");
    return std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);
}
std::string initial_directory_id(const std::wstring& path) {
    static std::atomic<unsigned> workers{};
    if(workers.fetch_add(1)>=4){--workers;throw Error(5,"Directory identity checks are busy; retry locking");}
    std::promise<std::string> promise;auto result=promise.get_future();
    try{std::thread([path,p=std::move(promise)]()mutable{try{p.set_value(directory_id(path));}catch(...){p.set_exception(std::current_exception());}--workers;}).detach();}
    catch(...){--workers;throw;}
    if(result.wait_for(std::chrono::milliseconds(800))!=std::future_status::ready)throw Error(5,"Directory identity check timed out; no lock was committed");
    return result.get();
}
bool valid(const State& s) {
    try {auto tabs=read_native_tabs(s.tab.window,s.tab.process_id,s.tab.process_started);return std::any_of(tabs.begin(),tabs.end(),[&](auto& t){return t.guid==s.tab.guid&&t.host==s.tab.host&&t.browser_window==s.tab.browser_window;});}catch(...){return false;}
}
// Native guards never modify WM_SETICON: that affects taskbar previews, not tabs.
bool restore_icon(State&) {return true;}
HRESULT WINAPI remove_detour(void* frame,GUID guid) {
    for(auto& s:states)if(!explicit_close_allowed(s.tab.window)&&s.tab.frame==reinterpret_cast<uintptr_t>(frame)-native_field("actions")&&s.tab.guid==guid&&s.expires>GetTickCount64()&&s.flags.close_protected()&&valid(s)) {
        ++s.blocked_close;return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    return original_remove(frame,guid);
}
HRESULT WINAPI close_detour(void* host,bool close_window) {
    if(auto s=state_for(reinterpret_cast<uintptr_t>(host));s&&!explicit_close_allowed(s->tab.window)&&s->flags.close_protected()) {
        ++s->blocked_close;return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    return original_close(host,close_window);
}
HRESULT WINAPI navigate_detour(void* browser,PCIDLIST_ABSOLUTE target,DWORD flags,DWORD extra) {
    if(!opening)for(auto& s:states)if(s.browser==reinterpret_cast<uintptr_t>(browser)&&s.flags.locked&&s.expires>GetTickCount64()&&valid(s)) {
        try {
            if(!s.directory_health||s.directory_health->status.load()!=1||GetTickCount64()-s.directory_health->checked.load()>15000)throw Error(3,"Locked directory identity is unavailable or changed; unlock and re-lock to confirm");
            ComPtr<IShellItem> item;auto hr=SHCreateItemFromIDList(target,IID_PPV_ARGS(&item));int order=1;
            if(FAILED(hr)||FAILED(s.locked_item->Compare(item.Get(),SICHINT_CANONICAL,&order)))throw Error(3,"Cannot identify navigation target");
            if(order==0)return original_navigate(browser,target,flags,extra);
            auto copy=ILCloneFull(target);if(!copy)throw Error(1,"Cannot copy navigation target");
            // A nested request for the same destination is queued only once per dispatch.
            bool duplicate=std::any_of(pending.begin(),pending.end(),[&](auto& p){return p.source.guid==s.tab.guid&&ILIsEqual(p.target,copy);});
            if(duplicate)CoTaskMemFree(copy);else pending.push_back({s.tab,copy});
            ++s.blocked_navigation;s.error.clear();
        }catch(const std::exception& e){s.error=e.what();++s.blocked_navigation;}
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    return original_navigate(browser,target,flags,extra);
}
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_NCDESTROY){explicit_windows.erase(window);RemoveWindowSubclass(window,window_proc,1);return DefSubclassProc(window,message,w,l);}
    if(message==WM_QUERYENDSESSION||message==WM_ENDSESSION){explicit_windows[window]=GetTickCount64()+10000;if(in_prompt)cancel_close_dialog_for_session_end(close_dialog);return DefSubclassProc(window,message,w,l);}
    if(message==WM_CLOSE||(message==WM_SYSCOMMAND&&(w&0xfff0)==SC_CLOSE)) {
        auto protected_tab=std::find_if(states.begin(),states.end(),[&](auto& s){return s.tab.window==window&&s.expires>GetTickCount64()&&s.flags.close_protected()&&valid(s);});
        if(protected_tab!=states.end()&&!explicit_close_allowed(window)) {
            if(in_prompt)return 0;in_prompt=true;
            // The nested dialog loop can renew/retire states. Copy everything
            // needed before displaying it; never retain the vector iterator.
            auto language=protected_tab->language;auto started=protected_tab->tab.process_started;
            auto answer=confirm_window_close(window,self,language,close_dialog);
            in_prompt=false;if(!answer)return 0;
            DWORD pid{};if(!IsWindow(window)||GetWindowThreadProcessId(window,&pid)!=GetCurrentThreadId()||pid!=GetCurrentProcessId()||bridge_process_started(GetCurrentProcess())!=started)return 0;
            explicit_windows[window]=GetTickCount64()+3000;return DefSubclassProc(window,message,w,l);
        }
    }
    return DefSubclassProc(window,message,w,l);
}
void ensure_hooks(const NativeTab& tab,bool protect,bool lock_directory) {
    std::lock_guard lock(setup);
    struct Entry{uintptr_t rva;const char* bytes;size_t count;void* detour;void** original;};
    std::vector<Entry> entries;
    if(protect&&!original_remove){entries.push_back({native_rva("remove"),"\x48\x89\x5c\x24\x18\x48\x89\x74\x24\x20\x55",11,reinterpret_cast<void*>(remove_detour),reinterpret_cast<void**>(&original_remove)});
        entries.push_back({native_rva("close"),"\x48\x89\x5c\x24\x10\x48\x89\x74\x24\x20\x57",11,reinterpret_cast<void*>(close_detour),reinterpret_cast<void**>(&original_close)});}
    if(lock_directory&&!original_navigate)entries.push_back({native_rva("navigate"),"\x48\x89\x5c\x24\x18\x55\x56\x57\x41\x54",10,reinterpret_cast<void*>(navigate_detour),reinterpret_cast<void**>(&original_navigate)});
    if(entries.empty())return;
    for(auto& e:entries)if(std::memcmp(reinterpret_cast<void*>(tab.module_base+e.rva),e.bytes,e.count))throw Error(3,"Native feature entry changed or is already hooked");
    auto status=MH_Initialize();if(status!=MH_OK&&status!=MH_ERROR_ALREADY_INITIALIZED)throw Error(3,"Cannot initialize feature hooks");
    std::vector<void*> created;
    try {
        for(auto& e:entries){auto target=reinterpret_cast<void*>(tab.module_base+e.rva);if(MH_CreateHook(target,e.detour,e.original)!=MH_OK)throw Error(3,"Cannot create native feature hook");created.push_back(target);}
        HMODULE pinned{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<PCWSTR>(ensure_hooks),&pinned))throw Error(3,"Cannot pin feature DLL");
        for(auto target:created)if(MH_EnableHook(target)!=MH_OK)throw Error(3,"Cannot enable native feature hook");
    }catch(...){for(auto target:created){MH_DisableHook(target);MH_RemoveHook(target);}for(auto& e:entries)*e.original=nullptr;throw;}
}
void check_directory_later(State& s) {
    if(!s.flags.locked||!s.directory_health)return;
    auto health=s.directory_health;if(health->pending||GetTickCount64()-health->checked.load()<2000)return;
    static std::atomic<unsigned> workers{};if(workers.fetch_add(1)>=4){--workers;return;}
    health->pending=true;auto path=s.locked_path;auto expected=s.directory_identity;
    try{std::thread([health,path,expected]{try{health->status=directory_id(path)==expected?1:0;}catch(...){health->status=0;}health->checked=GetTickCount64();health->pending=false;--workers;}).detach();}catch(...){health->pending=false;--workers;}
}
void CALLBACK tick(HWND,UINT,UINT_PTR id,DWORD) {
    try {
        std::erase_if(explicit_windows,[](auto& entry){return !IsWindow(entry.first)||entry.second<=GetTickCount64();});
        for(auto it=states.begin();it!=states.end();) {
            if(it->restoring||it->expires<=GetTickCount64()||!valid(*it)) {
                // Release guards immediately when the agent lease ends.
                it->flags={};it->style=Json::object();it->expires=0;it->restoring=true;
                bool restored=false;try{restored=restore_icon(*it);}catch(...){}
                if(restored)it=states.erase(it);else ++it;
            }else {check_directory_later(*it);++it;}
        }
        while(!pending.empty()) {
            auto p=pending.front();pending.pop_front();
            auto s=state_for(p.source.host);
            if(s&&s->flags.locked&&valid(*s)) {
                using Add=HRESULT(WINAPI*)(void*,PCIDLIST_ABSOLUTE,int,GUID*);GUID guid{};opening=true;
                auto hr=add_tab(p.source,p.target,&guid);opening=false;
                // AddTab is reentrant: locate the original state again after Explorer callbacks.
                if(auto current=state_for(p.source.host)){if(SUCCEEDED(hr))++current->redirected;else current->error="Navigation was blocked; opening a new tab failed";}
            }
            CoTaskMemFree(p.target);
        }
        if(states.empty()){KillTimer(nullptr,id);timer=0;}
    }catch(...) {opening=false;}
}
Json capabilities() {
    auto p=compat::current_profile();
    return {{"native_reorder",p->reorder&&compat::current_xaml_profile(compat::Module::ui)->reorder_extension},{"order_verification","separate extension identity and TabView.TabItems readback required"},{"pin_state",p->close_guard},{"close_guard",p->close_guard},{"navigation_guard",p->navigation_guard},{"tab_selection",p->selection},{"open_tab",p->opening},{"icon_style","independent XAML module check"},
        {"text_style","separate XAML adapter; actual readback required"},{"background_style","separate XAML adapter; actual brush readback required"},{"window_close_guard",true},
        {"adapter",p->source},{"contracts",compat::profile_report(*p)},{"validation","native entries verified; real UI routes remain experimental"},{"acceptance",{{"close_button","pending"},{"middle_click","pending"},{"ctrl_w","pending"},{"context_close","pending"},{"bulk_close","pending"},{"window_close_routes","pending"},{"navigation_routes","pending"},{"dpi_theme_visuals","pending"}}}};
}
Json native_order(const NativeTab& tab) {
    Json order=Json::array();for(auto& t:read_native_tabs(tab.window,tab.process_id,tab.process_started))order.push_back(guid_text(t.guid));return order;
}
Json reorder(const NativeTab& tab,const Json& payload,DWORD owner,uint64_t owner_started) {
    auto before=native_order(tab);auto expected=payload.at("expected");auto desired=payload.at("order");
    auto sorted=[](Json j){if(!j.is_array())throw Error(2,"Tab order must be an array");std::sort(j.begin(),j.end());return j;};
    if(desired.size()!=before.size()||sorted(expected)!=sorted(before)||sorted(desired)!=sorted(before))throw Error(2,"Docking must preserve all tabs in this window");
    for(auto& state:states)if(state.tab.frame==tab.frame&&state.expires>GetTickCount64()&&(state.owner!=owner||state.owner_started!=owner_started))throw Error(4,"Window contains tabs owned by another agent");

    ComPtr<IServiceProvider> provider;
    auto source=reinterpret_cast<IUnknown*>(read<uintptr_t>(tab.frame+native_field("reorder_provider")));
    if(!source||FAILED(source->QueryInterface(IID_PPV_ARGS(&provider))))throw Error(3,"Tab reorder service unavailable");
    // QueryService route used by CExplorerFrame::MoveSelectedTabToPosition,
    // validated by symbol references and field contracts, with an explicit position.
    auto service=read<GUID>(tab.module_base+native_rva("reorder_service")),iid=read<GUID>(tab.module_base+native_rva("reorder_iid"));
    ComPtr<IUnknown> extension;auto hr=provider->QueryService(service,iid,reinterpret_cast<void**>(extension.GetAddressOf()));
    if(FAILED(hr))throw Error(3,"Tab reorder extension unavailable");
    auto vtable=read<uintptr_t>(reinterpret_cast<uintptr_t>(extension.Get()));auto function=read<uintptr_t>(vtable+native_field("reorder_method")*sizeof(uintptr_t));
    HMODULE method_module{};wchar_t method_path[32768]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<PCWSTR>(function),&method_module)||!GetModuleFileNameW(method_module,method_path,32768))throw Error(3,"Reorder implementation identity unavailable");
    if(payload.value("probe",false))return {{"module",utf8(method_path)},{"method_rva",function-reinterpret_cast<uintptr_t>(method_module)},{"service",guid_text(service)},{"interface",guid_text(iid)}};
    using Move=HRESULT(WINAPI*)(void*,GUID,UINT);
    auto extension_profile=compat::current_xaml_profile(compat::Module::ui);
    if(!extension_profile->reorder_extension)throw Error(3,"Reorder extension contract unavailable: "+extension_profile->reason);
    compat::verify_loaded_module(method_module,*extension_profile,compat::module_path(compat::Module::ui));
    if(function-reinterpret_cast<uintptr_t>(method_module)!=extension_profile->rva("move_tab"))throw Error(3,"Native reorder service method differs from verified symbol");
    constexpr std::array<BYTE,15> expected_entry{0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x57,0x48,0x83,0xec,0x30,0x41,0x8b,0xf8};
    if(read<std::array<BYTE,15>>(function)!=expected_entry)throw Error(3,"Live reorder method changed or is hooked");
    auto active=read<GUID>(tab.frame+native_field("active_guid"));int moved=0;
    auto restore_selection=[&]{if(read<GUID>(tab.frame+native_field("active_guid"))==active)return;using Select=HRESULT(WINAPI*)(void*,GUID);auto hr=select_tab(tab,active);if(FAILED(hr)||read<GUID>(tab.frame+native_field("active_guid"))!=active)throw Error(5,"Could not restore selection after moving tabs");};
    auto working=expected;
    try {
    for(size_t i=0;i<desired.size();++i) {
        if(working[i]==desired[i])continue;
        if(sorted(native_order(tab))!=sorted(before))throw Error(5,"Tabs changed during docking");
        GUID guid{};if(FAILED(CLSIDFromString(wide(desired[i].get<std::string>()).c_str(),&guid)))throw Error(2,"Invalid reorder GUID");
        hr=reinterpret_cast<Move>(function)(extension.Get(),guid,static_cast<UINT>(i));
        if(FAILED(hr))throw Error(5,"Native tab move failed");++moved;
        auto position=std::find(working.begin(),working.end(),desired[i]);working.erase(position);working.insert(working.begin()+i,desired[i]);
        restore_selection();
    }
    }catch(...){try{restore_selection();}catch(...){}throw;}
    restore_selection();
    return {{"requested_order",desired},{"move_requested",true},{"selection_preserved",read<GUID>(tab.frame+native_field("active_guid"))==active},{"moved",moved}};
}
void execute(FeaturePacket& p) {
    verify_layout_sender(self,p.sender_pid,p.sender_started,p.action==FeatureAction::inspect);
    auto payload=Json::parse(p.payload);
    if(auto reason=feature_admission_error(*compat::current_profile(),p.action,payload,p.require_active);!reason.empty())throw Error(3,reason);
    if(p.action==FeatureAction::external_activation) {
        if(bridge_process_started(GetCurrentProcess())!=p.target_started||!is_shell_windows_broker(GetCurrentProcess()))
            throw Error(4,"Could not verify the ShellWindows broker");
        auto report=external_tabs::configure_activation(self,p.sender_pid,p.sender_started,
            payload.at("enabled").get<bool>(),payload.at("cookie").get<uint64_t>()).dump();
        strcpy_s(p.report,report.c_str());
        p.result=S_OK;
        return;
    }
    auto tab=locate(p);
    if(p.action==FeatureAction::external_tabs) {
        const auto report=external_tabs::configure(self,tab,p.sender_pid,p.sender_started,
            payload.at("enabled").get<bool>(),payload.value("cookie",uint64_t{})).dump();
        strcpy_s(p.report,report.c_str());
        p.result=S_OK;
        return;
    }
    auto found=std::find_if(states.begin(),states.end(),[&](auto& s){return s.tab.guid==tab.guid&&s.tab.frame==tab.frame;});
    if(p.action!=FeatureAction::inspect&&found!=states.end()&&found->expires>GetTickCount64()&&(found->owner!=p.sender_pid||found->owner_started!=p.sender_started))throw Error(4,"Tab is owned by another feature agent");
    if(p.action==FeatureAction::apply) {
        ensure_hooks(tab,payload.value("pinned",false)||payload.value("protected",false),payload.value("locked",false));State next=found==states.end()?State{}:*found;next.tab=tab;
        if(next.restoring||next.expires<=GetTickCount64()){next.locked_item.Reset();next.locked_path.clear();next.directory_identity.clear();next.directory_health.reset();}
        next.flags={payload.value("pinned",false),payload.value("protected",false),payload.value("locked",false)};
        next.language=payload.value("language",std::string("auto"));
        next.style=payload.value("style",Json::object());auto check=next.style;check.erase("rule_id");validate_style(check);
        if(next.flags.locked&&!next.locked_item) {
            next.locked_item=location(tab);next.locked_path=path_of(next.locked_item.Get());next.directory_identity=initial_directory_id(next.locked_path);next.directory_health=std::make_shared<DirectoryHealth>();next.directory_health->checked=GetTickCount64();
        }else if(!next.flags.locked){next.locked_item.Reset();next.locked_path.clear();next.directory_identity.clear();next.directory_health.reset();}
        next.browser=next.flags.locked?navigation_object(tab):0;
        if(next.flags.locked&&!next.browser)throw Error(3,"Native navigation host unavailable");
        if(!timer){timer=SetTimer(nullptr,0,100,tick);if(!timer)throw Error(3,"Cannot create protection lease timer");}
        if(!SetWindowSubclass(tab.window,window_proc,1,0))throw Error(3,"Cannot enable window close protection");
        next.owner=p.sender_pid;next.owner_started=p.sender_started;next.expires=GetTickCount64()+10000;next.restoring=false;
        if(found==states.end()){states.push_back(next);found=std::prev(states.end());}else *found=next;
    }else if(p.action==FeatureAction::restore) {
        if(found!=states.end()) {
            found->flags={};found->style=Json::object();found->expires=0;found->restoring=true;
            try{payload["icon_restored"]=restore_icon(*found);}catch(const std::exception& ex){payload["icon_restored"]=false;payload["icon_error"]=ex.what();}
            if(payload.value("icon_restored",false)){states.erase(found);found=states.end();}
        }else payload["icon_restored"]=true;
    }else if(p.action==FeatureAction::locate) {
        if(payload.contains("expected_path")) {
            auto item=location(tab);
            PWSTR name{};
            auto hr=item->GetDisplayName(SIGDN_FILESYSPATH,&name);
            std::wstring actual=name?name:L"";
            CoTaskMemFree(name);
            if(FAILED(hr)||actual.empty()||directory_key(actual)!=directory_key(wide(payload.at("expected_path").get<std::string>())))
                throw Error(3,"Directory tab changed before selection");
        }
        if(payload.value("foreground_only",false)&&GetForegroundWindow()!=tab.window)
            throw Error(3,"Foreground window changed before tab selection");
        if(read<GUID>(tab.frame+native_field("active_guid"))!=tab.guid&&FAILED(select_tab(tab,tab.guid)))
            throw Error(3,"Explorer rejected tab selection");
        if(payload.contains("select_file")&&payload.contains("expected_path"))
            payload["file_selected"]=external_tabs::select_file(tab,wide(payload.at("expected_path").get<std::string>()),
                wide(payload.at("select_file").get<std::string>()));
    }else if(p.action==FeatureAction::open) {
        if(payload.contains("expected_path")&&directory_key(path_of(location(tab).Get()))!=
            directory_key(wide(payload.at("expected_path").get<std::string>())))
            throw Error(3,"Source tab navigated before opening its parent");
        auto path=wide(payload.at("path").get<std::string>());PIDLIST_ABSOLUTE target{};auto hr=SHParseDisplayName(path.c_str(),nullptr,&target,0,nullptr);
        if(FAILED(hr)||!target)throw Error(3,"Restore directory cannot be resolved");
        if(payload.value("foreground_only",false)&&GetForegroundWindow()!=tab.window) {
            CoTaskMemFree(target);
            throw Error(3,"Foreground window changed before opening a tab");
        }
        using Add=HRESULT(WINAPI*)(void*,PCIDLIST_ABSOLUTE,int,GUID*);GUID guid{};
        hr=add_tab(tab,target,&guid);CoTaskMemFree(target);
        if(FAILED(hr))throw Error(3,"Opening the directory failed");payload["opened_guid"]=guid_text(guid);
        if(payload.value("select",false)&&(!payload.value("foreground_only",false)||GetForegroundWindow()==tab.window)) {
            if(read<GUID>(tab.frame+native_field("active_guid"))!=guid)hr=select_tab(tab,guid);
            payload["selection_confirmed"]=SUCCEEDED(hr)&&read<GUID>(tab.frame+native_field("active_guid"))==guid;
        }
    }else if(p.action==FeatureAction::close_duplicate) {
        const auto active_guid = read<GUID>(tab.frame+native_field("active_guid"));
        if (active_guid==tab.guid||guid_text(active_guid)!=payload.at("active_guid").get<std::string>())
            throw Error(5,"Selected tab changed before merging");
        if (found!=states.end()&&found->expires>GetTickCount64()&&
            (found->flags.close_protected()||found->flags.locked))
            throw Error(5,"Duplicate tab is protected");
        const auto tabs=read_native_tabs(tab.window,tab.process_id,tab.process_started);
        const auto keeper=std::find_if(tabs.begin(),tabs.end(),[&](const auto& candidate){
            return candidate.guid!=tab.guid&&guid_text(candidate.guid)==payload.at("keep_guid").get<std::string>();
        });
        const auto path=directory_key(wide(payload.at("path").get<std::string>()));
        if (keeper==tabs.end()||directory_key(path_of(location(*keeper).Get()))!=path||
            directory_key(path_of(location(tab).Get()))!=path)
            throw Error(5,"Duplicate or retained tab changed directory before merging");
        auto object=actions_object(tab,4,"remove");
        const auto address=tab.module_base+native_rva("remove");
        {
            std::scoped_lock lock(setup);
            constexpr std::array<BYTE,11> expected{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55};
            if (!original_remove&&read<std::array<BYTE,11>>(address)!=expected)
                throw Error(3,"Live RemoveTab function changed or is hooked");
        }
        if (FAILED(reinterpret_cast<Remove>(address)(object,tab.guid)))
            throw Error(5,"Explorer rejected the duplicate tab close");
        const auto after=native_order(tab);
        p.result=S_OK;
        const auto closed=std::find(after.begin(),after.end(),Json(guid_text(tab.guid)))==after.end();
        const auto report=Json{{"closed",closed}}.dump();
        strcpy_s(p.report,report.c_str());
        return;
    }else if(p.action==FeatureAction::reorder)payload["docking"]=reorder(tab,payload,p.sender_pid,p.sender_started);
    else if(p.action!=FeatureAction::inspect)throw Error(2,"Unknown feature action");
    found=std::find_if(states.begin(),states.end(),[&](auto& s){return s.tab.guid==tab.guid&&s.tab.frame==tab.frame;});
    Json report{{"capabilities",capabilities()},{"active",native_selection_supported()&&read<GUID>(tab.frame+native_field("active_guid"))==tab.guid},{"active_known",native_selection_supported()},
        {"native_guid",guid_text(tab.guid)},{"protection_confirmed",false},{"icon_applied",false},{"native_window_icon_untouched",true}};
    if(found!=states.end()&&found->expires>GetTickCount64()) {
        report.update(flags_json(found->flags));report["protection_confirmed"]=true;report["lease_ms"]=found->expires-GetTickCount64();
        report["locked_path"]=utf8(found->locked_path);
        if(found->flags.locked){bool healthy=found->directory_health&&found->directory_health->status.load()==1&&GetTickCount64()-found->directory_health->checked.load()<=15000;report["locked_identity_valid"]=healthy;if(!healthy)found->error="Locked directory identity needs confirmation; unlock and re-lock";}report["directory_identity"]=found->directory_identity;report["navigation_pending"]=std::any_of(pending.begin(),pending.end(),[&](auto& n){return n.source.guid==found->tab.guid;});
        report["blocked_close"]=found->blocked_close;report["blocked_navigation"]=found->blocked_navigation;report["redirected"]=found->redirected;report["error"]=found->error;
        report["icon_verification"]="rendered tab requires XAML readback; native window icon is untouched";
    }
    if(p.action==FeatureAction::restore){report["behavior_restored"]=true;report["icon_restored"]=payload.value("icon_restored",false);report["icon_error"]=payload.value("icon_error",std::string{});}
    if(payload.contains("opened_guid"))report["opened_guid"]=payload.at("opened_guid");
    if(payload.contains("selection_confirmed"))report["selection_confirmed"]=payload.at("selection_confirmed");
    if(payload.contains("file_selected"))report["file_selected"]=payload.at("file_selected");
    if(payload.value("inventory",false)) {
        report["tabs"]=Json::array();
        for(auto& native:read_native_tabs(tab.window,tab.process_id,tab.process_started)) {
            Json row{{"guid",guid_text(native.guid)},{"path",nullptr},{"original_title",""},{"location_resolved",false}};
            try{auto item=location(native);PWSTR name{};auto hr=item->GetDisplayName(SIGDN_NORMALDISPLAY,&name);if(SUCCEEDED(hr)&&name){row["original_title"]=utf8(name);CoTaskMemFree(name);}PWSTR filesystem{};auto filesystem_hr=item->GetDisplayName(SIGDN_FILESYSPATH,&filesystem);if(SUCCEEDED(filesystem_hr)&&filesystem)row["path"]=utf8(filesystem);CoTaskMemFree(filesystem);row["location_resolved"]=true;}catch(const std::exception& ex){row["error"]=ex.what();}
            report["tabs"].push_back(row);
        }
    }
    report["native_membership"]=native_order(tab);
    if(payload.contains("docking"))report["docking"]=payload.at("docking");
    auto text=report.dump();if(text.size()>=sizeof(p.report))throw Error(5,"Native inventory exceeds report limit");strcpy_s(p.report,text.c_str());p.result=S_OK;
}
void process(FeaturePacket* packet) {
    try {
        if(packet->magic!=0x54544654||packet->version!=1||packet->size!=sizeof(FeaturePacket)||!memchr(packet->payload,0,sizeof(packet->payload)))throw Error(2,"Invalid feature packet");
        auto copy=std::make_unique<FeaturePacket>(*packet);execute(*copy);packet->result=copy->result;strcpy_s(packet->report,copy->report);
    }catch(const std::exception& e){packet->result=E_FAIL;auto text=Json{{"error",e.what()}}.dump();strcpy_s(packet->report,text.c_str());}
    InterlockedExchange(&packet->completed,1);
}
void protected_process(FeaturePacket* packet) {
    __try{process(packet);}__except(EXCEPTION_EXECUTE_HANDLER){packet->result=E_UNEXPECTED;InterlockedExchange(&packet->completed,1);}
}
}
extern "C" __declspec(dllexport) LRESULT CALLBACK SinanFeatureHook(int code,WPARAM w,LPARAM l) {
    using namespace sinan;
    if(code>=0){auto m=reinterpret_cast<CWPSTRUCT*>(l);if(m&&m->message==RegisterWindowMessageW(feature_message)) {
        auto sender=static_cast<DWORD>(m->wParam);auto nonce=static_cast<uint64_t>(m->lParam);
        Handle mapping(OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,feature_mapping(sender,nonce).c_str()));
        if(mapping){auto p=static_cast<FeaturePacket*>(MapViewOfFile(mapping.get(),FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(FeaturePacket)));
            if(p){if(p->nonce==nonce&&p->sender_pid==sender&&p->window==reinterpret_cast<uintptr_t>(m->hwnd))features::protected_process(p);UnmapViewOfFile(p);}}
    }}
    return CallNextHookEx(nullptr,code,w,l);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH)sinan::features::self=module;return TRUE;}
