#include "hook/external_tabs.hpp"
#include <commctrl.h>
#include <dwmapi.h>
#include <exdisp.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace sinan::features::external_tabs {
namespace {
using Microsoft::WRL::ComPtr;
using Show = HRESULT(WINAPI*)(void*, PCIDLIST_ABSOLUTE, DWORD, POINT, int, HWND, IUnknown*, IUnknown*);
using CreateThread = BOOL(WINAPI*)(LPTHREAD_START_ROUTINE, void*, DWORD, LPTHREAD_START_ROUTINE);
using FindWindow = HRESULT(WINAPI*)(void*, VARIANT*, VARIANT*, int, long*, int, IDispatch**);
struct PidlDeleter {
    void operator()(PIDLIST_ABSOLUTE item) const noexcept { CoTaskMemFree(item); }
};
using Pidl = std::unique_ptr<ITEMIDLIST_ABSOLUTE, PidlDeleter>;
using Selection = std::vector<Pidl>;
struct CallbackScope {
    bool& running;
    explicit CallbackScope(bool& flag) : running(flag) { running=true; }
    ~CallbackScope() { running=false; }
};

// ExplorerFrame 26100.8457. Admission also checks the complete module hash.
constexpr uintptr_t host_vtable = 0x24bc50;
constexpr uintptr_t show_window = 0x131250;
constexpr uintptr_t create_thread_import = 0x2543a0;
constexpr uintptr_t run_browser_thread = 0x11ab70;
constexpr uintptr_t find_shell_window = 0xb9050;
constexpr uintptr_t find_shell_slot = 0x244508;
constexpr UINT_PTR subclass_id = 1;
constexpr DWORD sample_ms = 40;
constexpr uint64_t selection_settle_ms = 440;
constexpr uint64_t request_ms = 2600;
constexpr uint64_t lease_ms = 10000;
constexpr unsigned max_requests = 8;
enum class Phase { source, queued, opening, ready, closing, committed, cancelled };

struct Frame {
    NativeTab anchor;
    DWORD thread;
    uint64_t expiry;
};
struct Request {
    Pidl folder;
    Frame destination;
    uint64_t epoch{};
    uint64_t deadline{};
    HWND source{};
    UINT_PTR source_timer{};
    GUID source_tab{};
    GUID opened{};
    Selection selection;
    uint64_t selection_since{};
    std::atomic<Phase> phase{Phase::source};
    UINT_PTR id{};
    bool opening_native=false;
    ~Request();
};
struct BrowserLaunch {
    LPTHREAD_START_ROUTINE start;
    void* argument;
    std::shared_ptr<Request> request;
};
std::mutex state;
std::map<HWND, Frame> frames;
std::map<UINT_PTR, std::shared_ptr<Request>> queued;

thread_local std::map<UINT_PTR, std::shared_ptr<Request>> transfers;
thread_local std::shared_ptr<Request> launch_request;
thread_local std::shared_ptr<Request> source_request;
thread_local HHOOK source_observer{};
thread_local bool reading_source=false, reading_destination=false;
DWORD owner_pid{};
uint64_t owner_creation{};
uint64_t owner_expiry{};
uint64_t owner_epoch{};
uint64_t activation_cookie{};
UINT_PTR next_request{};
std::atomic<Show> original_show{};
std::atomic<CreateThread> original_create_thread{};
std::atomic<LPTHREAD_START_ROUTINE> browser_entry{};
std::atomic<FindWindow> original_find_window{};
struct BrokerLease {
    DWORD owner{};
    uint64_t started{}, expiry{}, cookie{};
    UINT_PTR timer{};
    void* volatile* slot{};
};
BrokerLease broker;
std::atomic<unsigned> requests{};
std::atomic<uint64_t> redirected{}, fallback{}, rollback_failed{}, reused{};

Request::~Request() { --requests; }

UINT open_message() {
    static const auto message=RegisterWindowMessageW(L"Sinan.ExternalFolder.Transfer.1");
    return message;
}
UINT activate_message() {
    static const auto message=RegisterWindowMessageW(L"Sinan.ExternalFolder.Activate.1");
    return message;
}

bool cloak(HWND window, BOOL value) {
    return SUCCEEDED(DwmSetWindowAttribute(window,DWMWA_CLOAK,&value,sizeof(value)));
}

bool current_epoch(uint64_t epoch) {
    std::lock_guard lock(state);
    return epoch==owner_epoch&&GetTickCount64()<owner_expiry;
}

std::vector<NativeTab> live_tabs(const Frame& frame) {
    DWORD pid{};
    if(!frame.anchor.window||GetWindowThreadProcessId(frame.anchor.window,&pid)!=frame.thread||
        pid!=frame.anchor.process_id)return {};
    auto tabs=read_native_tabs(frame.anchor.window,pid,frame.anchor.process_started);
    if(tabs.empty()||tabs.front().frame!=frame.anchor.frame)return {};
    return tabs;
}

ComPtr<IFolderView2> folder_view(const NativeTab& tab, PCIDLIST_ABSOLUTE expected) {
    auto view=external_tab_view(tab);
    ComPtr<IFolderView2> result;
    ComPtr<IPersistFolder2> folder;
    if(!view||FAILED(view.As(&result))||FAILED(result->GetFolder(IID_PPV_ARGS(&folder))))return {};
    PIDLIST_ABSOLUTE current{};
    const auto hr=folder->GetCurFolder(&current);
    Pidl location(reinterpret_cast<Pidl::pointer>(current));
    if(hr!=S_OK||!location||!ILIsEqual(location.get(),expected))return {};
    return result;
}

std::optional<Selection> read_selection(IFolderView2& view, PCIDLIST_ABSOLUTE parent) {
    int selected_count{};
    if(FAILED(view.ItemCount(SVGIO_SELECTION,&selected_count))||selected_count<0||selected_count>10000)return {};
    Selection selection;
    if(!selected_count)return selection;
    ComPtr<IShellItemArray> items;
    const auto hr=view.GetSelection(FALSE,&items);
    DWORD count{};
    if(FAILED(hr)||!items||FAILED(items->GetCount(&count))||count!=static_cast<DWORD>(selected_count))return {};
    selection.reserve(count);
    for(DWORD index=0;index<count;++index) {
        ComPtr<IShellItem> item;
        PIDLIST_ABSOLUTE raw{};
        if(FAILED(items->GetItemAt(index,&item)))return {};
        const auto result=SHGetIDListFromObject(item.Get(),&raw);
        Pidl absolute(reinterpret_cast<Pidl::pointer>(raw));
        if(FAILED(result)||!absolute||!ILIsParent(parent,absolute.get(),TRUE))return {};
        selection.push_back(std::move(absolute));
    }
    const auto less=[](const Pidl& a, const Pidl& b) {
        const auto a_size=ILGetSize(a.get()), b_size=ILGetSize(b.get());
        return a_size==b_size?memcmp(a.get(),b.get(),a_size)<0:a_size<b_size;
    };
    std::sort(selection.begin(),selection.end(),less);
    return selection;
}

bool same_selection(const Selection& left, const Selection& right) {
    return left.size()==right.size()&&std::equal(left.begin(),left.end(),right.begin(),
        [](const Pidl& a, const Pidl& b){return ILIsEqual(a.get(),b.get());});
}

bool verify_destination(const NativeTab& tab, const Request& request) {
    auto view=folder_view(tab,request.folder.get());
    if(!view)return false;
    auto selection=read_selection(*view.Get(),request.folder.get());
    if(selection&&same_selection(*selection,request.selection))return true;
    ComPtr<IShellView> shell;
    if(FAILED(view.As(&shell)))return false;
    auto hr=S_OK;
    if(request.selection.empty())hr=shell->SelectItem(nullptr,SVSI_DESELECTOTHERS);
    for(size_t index=0;index<request.selection.size()&&SUCCEEDED(hr);++index) {
        const auto flags=index?SVSI_SELECT:SVSI_SELECT|SVSI_DESELECTOTHERS|SVSI_FOCUSED|SVSI_ENSUREVISIBLE;
        hr=shell->SelectItem(ILFindLastID(request.selection[index].get()),flags);
    }
    if(FAILED(hr))return false;
    selection=read_selection(*view.Get(),request.folder.get());
    return selection&&same_selection(*selection,request.selection);
}

bool destination_active(const NativeTab& tab) {
    return read_active_tab(tab)==tab.guid&&IsWindowVisible(tab.browser_window)&&
        FindWindowExW(tab.window,nullptr,L"ShellTabWindowClass",nullptr)==tab.browser_window;
}

void CALLBACK transfer_tick(HWND,UINT,UINT_PTR timer,DWORD) {
    if(reading_destination)return;
    CallbackScope reading(reading_destination);
    const auto found=transfers.find(timer);
    if(found==transfers.end())return;
    const auto request=found->second;
    if(request->opening_native)return;
    const auto phase=request->phase.load();
    if(phase==Phase::ready||phase==Phase::closing)return;
    try {
        if(phase==Phase::committed)SetForegroundWindow(request->destination.anchor.window);
        if(phase==Phase::cancelled||phase==Phase::opening||phase==Phase::committed) {
            const auto tabs=live_tabs(request->destination);
            const auto tab=std::find_if(tabs.begin(),tabs.end(),[&](const auto& value){return value.guid==request->opened;});
            if(phase==Phase::opening) {
                if(tab!=tabs.end()&&verify_destination(*tab,*request)) {
                    // AddTab may return before the Explorer view can accept selection.
                    if(!destination_active(*tab)) {
                        select_tab(*tab,tab->guid);
                        return;
                    }
                    auto opening=Phase::opening;
                    request->phase.compare_exchange_strong(opening,Phase::ready);
                }
                return;
            }
            if(tab!=tabs.end()) {
                if(phase==Phase::committed) {
                    if(!destination_active(*tab))select_tab(*tab,tab->guid);
                } else if(!discard_external_tab(*tab)) {
                    ++rollback_failed;
                }
            }
        }
    } catch (...) {
        if(phase==Phase::opening)return;
        if(phase==Phase::cancelled)++rollback_failed;
    }
    KillTimer(nullptr,timer);
    transfers.erase(timer);
}

void open_in_frame(const std::shared_ptr<Request>& request) {
    auto pending=Phase::queued;
    if(!request->phase.compare_exchange_strong(pending,Phase::opening))return;
    UINT_PTR timer{};
    try {
        const auto tabs=live_tabs(request->destination);
        if(!tabs.empty()) {
            const auto active=read_active_tab(tabs.front());
            const auto tab=std::find_if(tabs.begin(),tabs.end(),[&](const auto& value){return active==value.guid;});
            if(tab!=tabs.end()&&current_epoch(request->epoch)&&request->phase==Phase::opening) {
                timer=SetTimer(nullptr,0,sample_ms,transfer_tick);
                if(timer) {
                    transfers.emplace(timer,request);
                    CallbackScope opening(request->opening_native);
                    add_tab(*tab,request->folder.get(),&request->opened);
                    if(request->opened!=GUID{})return;
                }
            }
        }
    } catch (...) {
        if(request->opened!=GUID{})return;
    }
    if(timer) {
        KillTimer(nullptr,timer);
        transfers.erase(timer);
    }
    request->phase=Phase::cancelled;
}

LRESULT CALLBACK frame_proc(HWND window, UINT message, WPARAM id, LPARAM data, UINT_PTR, DWORD_PTR) {
    if(message==WM_NCDESTROY) {
        std::lock_guard lock(state);
        frames.erase(window);

        RemoveWindowSubclass(window,frame_proc,subclass_id);
    } else if(message==activate_message()) {
        Frame destination{};
        {
            std::lock_guard lock(state);
            const auto it=frames.find(window);
            if(!activation_cookie||static_cast<uint64_t>(data)!=activation_cookie||it==frames.end()||
                GetTickCount64()>=it->second.expiry)return 0;
            destination=it->second;
        }
        try {
            const auto tabs=live_tabs(destination);
            const auto tab=std::find_if(tabs.begin(),tabs.end(),[&](const auto& item){
                return item.browser_window==reinterpret_cast<HWND>(id);
            });
            if(tab!=tabs.end()&&!destination_active(*tab)&&SUCCEEDED(select_tab(*tab,tab->guid))) {
                if(destination_active(*tab))++reused;
                SetForegroundWindow(window);
            }
        } catch(...) {}
        return 0;
    } else if(message==open_message()) {
        std::shared_ptr<Request> request;
        {
            std::lock_guard lock(state);
            const auto it=queued.find(id);
            if(it==queued.end()||it->second->destination.anchor.window!=window)return 0;
            request=std::move(it->second);
            queued.erase(it);
        }
        open_in_frame(request);
        return 0;
    }
    return DefSubclassProc(window,message,id,data);
}

bool queue_transfer(const std::shared_ptr<Request>& request) {
    const auto destination=request->destination.anchor.window;
    {
        std::lock_guard lock(state);
        if(!frames.contains(destination)||owner_epoch!=request->epoch||owner_expiry<=GetTickCount64())return false;
        request->id=++next_request;
        queued.emplace(request->id,request);
        request->phase=Phase::queued;
    }
    return PostMessageW(destination,open_message(),request->id,0)!=FALSE;
}

void cancel_source(Request& request) {
    request.phase=Phase::cancelled;
    if(request.id) {
        std::lock_guard lock(state);
        queued.erase(request.id);
    }
    if(GetWindowThreadProcessId(request.source,nullptr)==GetCurrentThreadId()&&!cloak(request.source,FALSE))
        ++rollback_failed;
    ++fallback;
}

void finish_source(const std::shared_ptr<Request>& request) {
    if(request->phase==Phase::closing&&!IsWindow(request->source)) {
        request->phase=Phase::committed;
        ++redirected;
    } else {
        cancel_source(*request);
    }
    KillTimer(nullptr,request->source_timer);
    source_request.reset();
}

void CALLBACK source_tick(HWND,UINT,UINT_PTR timer,DWORD) {
    if(reading_source)return;
    CallbackScope reading(reading_source);
    const auto request=source_request;
    if(!request||request->source_timer!=timer)return;
    const auto now=GetTickCount64();
    const auto phase=request->phase.load();
    if(phase==Phase::closing) {
        if(!IsWindow(request->source)||now>=request->deadline)finish_source(request);
        return;
    }
    try {
        if(phase!=Phase::cancelled&&now<request->deadline&&IsWindow(request->source)&&current_epoch(request->epoch)) {
            if(phase==Phase::queued||phase==Phase::opening)return;
            const auto& anchor=request->destination.anchor;
            const auto tabs=read_native_tabs(request->source,anchor.process_id,anchor.process_started);
            if(tabs.size()==1) {
                const auto& tab=tabs.front();
                if(request->source_tab==GUID{}||request->source_tab==tab.guid) {
                    auto view=folder_view(tab,request->folder.get());
                    auto selection=view?read_selection(*view.Get(),request->folder.get()):std::nullopt;
                    if(selection&&phase==Phase::source) {
                        request->source_tab=tab.guid;
                        if(!request->selection_since||!same_selection(request->selection,*selection)) {
                            request->selection=std::move(*selection);
                            request->selection_since=now;
                        }
                        if(now-request->selection_since<selection_settle_ms||queue_transfer(request))return;
                    } else if(selection&&phase==Phase::ready&&same_selection(request->selection,*selection)) {
                        request->phase=Phase::closing;
                        if(PostMessageW(request->source,WM_CLOSE,0,0))return;
                    }
                }
            }
            if(phase==Phase::source&&!request->id) {
                request->selection_since=0;
                return;
            }
        }
    } catch (...) {
        if(phase==Phase::source&&!request->id&&now<request->deadline) {
            request->selection_since=0;
            return;
        }
    }
    finish_source(request);
}

LRESULT CALLBACK observe_source(int code, WPARAM from, LPARAM message) {
    if(code==HC_ACTION&&source_request&&!source_request->source) {
        const auto& call=*reinterpret_cast<const CWPSTRUCT*>(message);
        wchar_t name[32]{};
        if(call.message==WM_CREATE&&GetClassNameW(call.hwnd,name,static_cast<int>(std::size(name)))&&
            wcscmp(name,L"CabinetWClass")==0&&GetAncestor(call.hwnd,GA_ROOT)==call.hwnd) {
            UnhookWindowsHookEx(std::exchange(source_observer,nullptr));
            const auto timer=SetTimer(nullptr,0,sample_ms,source_tick);
            if(timer&&cloak(call.hwnd,TRUE)) {
                source_request->source=call.hwnd;
                source_request->source_timer=timer;
                source_request->deadline=GetTickCount64()+request_ms;
            } else {
                if(timer)KillTimer(nullptr,timer);
                source_request.reset();
            }
        }
    }
    return CallNextHookEx(nullptr,code,from,message);
}

DWORD WINAPI browser_thread(void* argument) {
    std::unique_ptr<BrowserLaunch> launch(static_cast<BrowserLaunch*>(argument));
    const auto start=launch->start;
    const auto original_argument=launch->argument;
    source_request=std::move(launch->request);
    launch.reset();
    if(current_epoch(source_request->epoch))
        source_observer=SetWindowsHookExW(WH_CALLWNDPROC,observe_source,nullptr,GetCurrentThreadId());
    if(!source_observer)source_request.reset();
    const auto result=start(original_argument);
    if(source_observer)UnhookWindowsHookEx(std::exchange(source_observer,nullptr));
    auto request=std::move(source_request);
    if(request&&request->source)finish_source(request);
    return result;
}

BOOL WINAPI create_browser_thread(LPTHREAD_START_ROUTINE start, void* argument, DWORD flags,
    LPTHREAD_START_ROUTINE callback) {
    const auto native=original_create_thread.load();
    if(launch_request&&start==browser_entry.load()&&!callback) {
        auto* launch=new (std::nothrow) BrowserLaunch{start,argument,std::move(launch_request)};
        if(launch) {
            if(native(browser_thread,launch,flags,nullptr))return TRUE;
            delete launch;
            return FALSE;
        }
    }
    return native(start,argument,flags,callback);
}

std::shared_ptr<Request> prepare(PCIDLIST_ABSOLUTE folder, DWORD flags, POINT point, int show,
    HWND owner, IUnknown* handshake) {
    if(!folder||flags||point.x||point.y||show!=SW_SHOWNORMAL||owner||handshake)return {};
    Frame destination{};
    uint64_t epoch{};
    {
        std::lock_guard lock(state);
        const auto now=GetTickCount64();
        if(owner_expiry<=now)return {};
        for(auto window=GetTopWindow(nullptr);window;window=GetWindow(window,GW_HWNDNEXT)) {
            const auto it=frames.find(window);
            if(it!=frames.end()&&it->second.expiry>now&&IsWindowVisible(window)&&IsWindowEnabled(window)&&!IsIconic(window)) {
                DWORD cloaked{};
                if(FAILED(DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))||cloaked)continue;
                destination=it->second;
                epoch=owner_epoch;
                break;
            }
        }
    }
    if(!destination.anchor.window)return {};
    wchar_t path[32768]{};
    if(!SHGetPathFromIDListEx(folder,path,static_cast<DWORD>(std::size(path)),GPFIDL_DEFAULT)||
        path[1]!=L':'||path[2]!=L'\\')return {};
    const wchar_t root[]{path[0],L':',L'\\',L'\0'};
    if(GetDriveTypeW(root)!=DRIVE_FIXED)return {};
    const auto attributes=GetFileAttributesW(path);
    if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||
        (attributes&FILE_ATTRIBUTE_REPARSE_POINT))return {};
    if(requests.fetch_add(1)>=max_requests) {
        --requests;
        return {};
    }
    std::shared_ptr<Request> request;
    try { request=std::make_shared<Request>(); }
    catch (...) { --requests; throw; }
    request->folder.reset(reinterpret_cast<Pidl::pointer>(ILCloneFull(folder)));
    if(!request->folder)return {};
    request->destination=destination;
    request->epoch=epoch;
    return request;
}

HRESULT WINAPI show_hook(void* self, PCIDLIST_ABSOLUTE folder, DWORD flags, POINT point,
    int show, HWND owner, IUnknown* site, IUnknown* handshake) {
    // Keep the token scoped to this invocation, including when Shell reenters.
    auto previous=std::move(launch_request);
    try { launch_request=prepare(folder,flags,point,show,owner,handshake); }
    catch (...) {}
    const auto hr=original_show.load()(self,folder,flags,point,show,owner,site,handshake);
    launch_request=std::move(previous);
    return hr;
}

bool replace_slot(void* volatile* slot, void* expected, void* replacement) {
    if(*slot!=expected)return false;
    DWORD protection{};
    if(!VirtualProtect(const_cast<void**>(slot),sizeof(void*),PAGE_READWRITE,&protection))return false;
    const auto old=InterlockedCompareExchangePointer(slot,replacement,expected);
    DWORD ignored{};
    VirtualProtect(const_cast<void**>(slot),sizeof(void*),protection,&ignored);
    return old==expected;
}

void detach(uintptr_t base) {
    auto host=reinterpret_cast<void* volatile*>(base+host_vtable+3*sizeof(void*));
    auto importer=reinterpret_cast<void* volatile*>(base+create_thread_import);


    replace_slot(host,reinterpret_cast<void*>(show_hook),reinterpret_cast<void*>(original_show.load()));
    replace_slot(importer,reinterpret_cast<void*>(create_browser_thread),reinterpret_cast<void*>(original_create_thread.load()));
}

bool attach(HMODULE self, uintptr_t base) {
    auto host=reinterpret_cast<void* volatile*>(base+host_vtable+3*sizeof(void*));
    auto importer=reinterpret_cast<void* volatile*>(base+create_thread_import);

    if(*host==reinterpret_cast<void*>(show_hook)&&*importer==reinterpret_cast<void*>(create_browser_thread))return true;
    const auto native_show=reinterpret_cast<void*>(base+show_window);
    const auto native_create=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"shcore.dll"),"SHCreateThread"));

    if(!native_create||*host!=native_show||*importer!=native_create||!open_message())return false;
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<PCWSTR>(show_hook),&pinned)||pinned!=self)return false;
    original_show=reinterpret_cast<Show>(native_show);
    original_create_thread=reinterpret_cast<CreateThread>(native_create);
    browser_entry=reinterpret_cast<LPTHREAD_START_ROUTINE>(base+run_browser_thread);

    if(!replace_slot(importer,native_create,reinterpret_cast<void*>(create_browser_thread)))return false;
    if(replace_slot(host,native_show,reinterpret_cast<void*>(show_hook)))return true;
    detach(base);
    return false;
}

HRESULT WINAPI find_window_hook(void* self, VARIANT* location, VARIANT* root, int kind,
    long* window, int options, IDispatch** dispatch) {
    const auto hr=original_find_window.load()(self,location,root,kind,window,options,dispatch);
    // Native open and reveal ask the broker for a PIDL match with dispatch and pending support.
    if(hr!=S_OK||!location||location->vt!=(VT_ARRAY|VT_UI1)||kind!=SWC_BROWSER||
        options!=(SWFO_NEEDDISPATCH|SWFO_INCLUDEPENDING)||!window||!*window||!dispatch||!*dispatch)return hr;
    uint64_t cookie{};
    {
        std::lock_guard lock(state);
        if(broker.expiry<=GetTickCount64())return hr;
        cookie=broker.cookie;
    }
    const auto browser=static_cast<HWND>(LongToHandle(*window));
    wchar_t name[32]{};
    if(!GetClassNameW(browser,name,static_cast<int>(std::size(name)))||wcscmp(name,L"ShellTabWindowClass")!=0)return hr;
    DWORD pid{}, desktop_pid{};
    GetWindowThreadProcessId(browser,&pid);
    GetWindowThreadProcessId(GetShellWindow(),&desktop_pid);
    const auto frame=GetAncestor(browser,GA_ROOT);
    if(frame&&pid&&pid==desktop_pid)
        PostMessageW(frame,activate_message(),reinterpret_cast<WPARAM>(browser),static_cast<LPARAM>(cookie));
    return hr;
}

void detach_broker() {
    KillTimer(nullptr,broker.timer);
    replace_slot(broker.slot,reinterpret_cast<void*>(find_window_hook),reinterpret_cast<void*>(original_find_window.load()));
    broker={};
}

void CALLBACK expire_broker(HWND,UINT,UINT_PTR timer,DWORD) {
    std::lock_guard lock(state);
    if(timer==broker.timer&&broker.expiry<=GetTickCount64())detach_broker();
}
}

bool select_file(const NativeTab& tab,const std::wstring& directory,const std::wstring& file) {
    PIDLIST_ABSOLUTE raw_folder{},raw_file{};
    const auto folder_result=SHParseDisplayName(directory.c_str(),nullptr,&raw_folder,0,nullptr);
    Pidl folder(raw_folder);
    const auto file_result=SHParseDisplayName(file.c_str(),nullptr,&raw_file,0,nullptr);
    Pidl item(raw_file);
    if(FAILED(folder_result)||FAILED(file_result)||!folder||!item||!ILIsParent(folder.get(),item.get(),TRUE))
        return false;
    Request request;
    request.folder=std::move(folder);
    request.selection.push_back(std::move(item));
    return read_active_tab(tab)==tab.guid&&verify_destination(tab,request);
}

Json configure_activation(HMODULE module,DWORD owner,uint64_t started,bool enabled,uint64_t cookie) {
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ExplorerFrame.dll"));
    if(!base||!activate_message())return {{"active",false},{"error","ShellWindows activation adapter is unavailable"}};
    auto slot=reinterpret_cast<void* volatile*>(base+find_shell_slot);
    auto original=reinterpret_cast<void*>(base+find_shell_window);
    const auto replacement=reinterpret_cast<void*>(find_window_hook);
    std::lock_guard lock(state);
    const auto now=GetTickCount64();
    const bool same_owner=broker.owner==owner&&broker.started==started;
    if(broker.expiry>now&&!same_owner)return {{"active",false},{"error","ShellWindows activation is owned by another agent"}};
    if(!enabled) {
        if(same_owner)detach_broker();
        return {{"active",false}};
    }
    if(*slot!=replacement) {
        HMODULE pinned{};
        constexpr unsigned char expected[]{0x48,0x89,0x5c,0x24,0x18,0x44,0x89,0x4c,0x24,0x20};
        if(*slot!=original||memcmp(original,expected,sizeof(expected))||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<PCWSTR>(find_window_hook),&pinned)||pinned!=module)
            return {{"active",false},{"error","Could not verify the ShellWindows folder lookup slot"}};
        const auto timer=SetTimer(nullptr,0,2000,expire_broker);
        if(!timer)return {{"active",false},{"error","Could not create the ShellWindows activation lease"}};
        original_find_window=reinterpret_cast<FindWindow>(original);
        if(!replace_slot(slot,original,replacement)) {
            KillTimer(nullptr,timer);
            return {{"active",false},{"error","Could not attach the ShellWindows folder lookup adapter"}};
        }
        broker.timer=timer;
        broker.slot=slot;
    }
    broker.owner=owner;
    broker.started=started;
    broker.expiry=now+lease_ms;
    broker.cookie=cookie;
    return {{"active",true}};
}

Json configure(HMODULE module, const NativeTab& tab, DWORD owner, uint64_t owner_started, bool enabled,uint64_t cookie) {
    DWORD shell_pid{};
    GetWindowThreadProcessId(GetShellWindow(),&shell_pid);
    if(shell_pid!=tab.process_id)return {{"active",false},{"error","Window is hosted outside the desktop Explorer process"}};
    std::lock_guard lock(state);
    const auto now=GetTickCount64();
    const bool same_owner=owner_pid==owner&&owner_creation==owner_started;
    if(owner_expiry>now&&!same_owner)return {{"active",false},{"error","External tab routing is owned by another agent"}};
    std::erase_if(frames,[&](const auto& row){return row.second.expiry<=now;});
    if(enabled) {
        if(!SetWindowSubclass(tab.window,frame_proc,subclass_id,0))
            return {{"active",false},{"error","Could not register the destination window"}};
        if(!attach(module,tab.module_base)) {
            RemoveWindowSubclass(tab.window,frame_proc,subclass_id);
            return {{"active",false},{"error","Could not attach the verified folder launch adapter"}};
        }
        if(!same_owner||owner_expiry<=now)++owner_epoch;
        owner_pid=owner;
        owner_creation=owner_started;
        owner_expiry=now+lease_ms;
        activation_cookie=cookie;
        frames[tab.window]={tab,GetCurrentThreadId(),owner_expiry};
    } else if(same_owner) {
        owner_expiry=0;
        activation_cookie=0;
        frames.clear();

        RemoveWindowSubclass(tab.window,frame_proc,subclass_id);
        detach(tab.module_base);
    }
    return {{"active",owner_expiry>now&&!frames.empty()},{"ready_windows",frames.size()},
        {"redirected",redirected.load()},{"reused",reused.load()},{"fallback",fallback.load()},{"rollback_failed",rollback_failed.load()}};
}
}
