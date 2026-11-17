#include "hook/layout.hpp"
#include "hook/identity.hpp"
#include <xamlOM.h>
#include <ocidl.h>
#include <weakreference.h>
#include <wrl.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>

// XAML diagnostics is used only for tab layout. No XAML private vtable offsets
// or generated WinUI projections are needed; the runtime resolves properties.
namespace sinan::layout {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
constexpr CLSID tap_class{0x7094f37e,0x60b9,0x4c80,{0xac,0x02,0x66,0x57,0x18,0x2e,0xd2,0xc1}};
// The diagnostics runtime retains every reported object. Hand that reference
// back after making a weak reference, including for objects we don't style.
MIDL_INTERFACE("7776037B-CBBE-4DE1-AD6F-0DBF1D5817B4") IDiagnosticsLifetime : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(InstanceHandle, void**) = 0;
};
HMODULE self = nullptr;
std::atomic<HRESULT> initialization{E_PENDING};
std::atomic<bool> started{false};
std::atomic<const char*> initialization_stage{"starting"};
std::mutex site_mutex;
ComPtr<IXamlDiagnostics> diagnostics;
ComPtr<IVisualTreeService2> service;
ComPtr<IDiagnosticsLifetime> lifetime;

struct Chain {
    unsigned sources_count = 0, values_count = 0;
    PropertyChainSource* sources = nullptr;
    PropertyChainValue* values = nullptr;
    Chain(IVisualTreeService2* api, InstanceHandle handle) {
        winrt::check_hresult(api->GetPropertyValuesChain(handle, &sources_count, &sources, &values_count, &values));
    }
    ~Chain() {
        for(unsigned i=0;i<sources_count;++i) {
            SysFreeString(sources[i].TargetType);SysFreeString(sources[i].Name);
            SysFreeString(sources[i].SrcInfo.FileName);SysFreeString(sources[i].SrcInfo.Hash);
        }
        for(unsigned i=0;i<values_count;++i) {
            auto& v=values[i];SysFreeString(v.Type);SysFreeString(v.DeclaringType);SysFreeString(v.ValueType);
            SysFreeString(v.ItemType);SysFreeString(v.Value);SysFreeString(v.PropertyName);
        }
        CoTaskMemFree(sources);CoTaskMemFree(values);
    }
    const PropertyChainValue* find(const wchar_t* name) const {
        for(unsigned i=0;i<values_count;++i)
            if(values[i].PropertyName && !values[i].Overridden && wcscmp(values[i].PropertyName,name)==0) return &values[i];
        return nullptr;
    }
};
struct Registered {
    ComPtr<IXamlDiagnostics> diagnostics;
    ComPtr<IDiagnosticsLifetime> lifetime;
    InstanceHandle handle = 0;
    Registered(IInspectable* object, IXamlDiagnostics* diag, IDiagnosticsLifetime* life):diagnostics(diag),lifetime(life) {
        winrt::check_hresult(diagnostics->RegisterInstance(object,&handle));
    }
    ~Registered(){if(handle&&lifetime)lifetime->UnregisterInstance(handle);}
};
struct SavedProperty {
    unsigned index = 0;
    bool local = false;
    ComPtr<IInspectable> value;
};
struct Element {
    InstanceHandle id = 0;
    bool tab_view = false;
    ComPtr<IWeakReference> weak;
    std::map<std::wstring,SavedProperty> saved;
};
thread_local std::vector<Element> elements;
thread_local TabAppearance options;
thread_local DWORD owner = 0;
thread_local uint64_t owner_started = 0, expires = 0;
thread_local UINT_PTR timer = 0;
thread_local std::string last_error;
thread_local std::string operation;
thread_local bool geometry_changed = false;
thread_local std::vector<InstanceHandle> release_queue;
thread_local UINT_PTR release_timer = 0;
// The pinned runtime uses the IInspectable identity pointer as its handle.
// An older diagnostics listener can unregister the shared cache before our
// callback runs. QI is guarded and used only for handles received from XAML,
// never for a pointer supplied by the client.
HRESULT identity_from_callback(InstanceHandle handle,IInspectable** output) {
    __try {return reinterpret_cast<IUnknown*>(handle)->QueryInterface(__uuidof(IInspectable),reinterpret_cast<void**>(output));}
    __except(EXCEPTION_EXECUTE_HANDLER){return E_UNEXPECTED;}
}

ComPtr<IInspectable> resolve(const Element& element) {
    ComPtr<IInspectable> result;
    winrt::check_hresult(element.weak->Resolve(__uuidof(IInspectable),&result));
    return result;
}
struct Api {
    ComPtr<IXamlDiagnostics> diag;
    ComPtr<IVisualTreeService2> tree;
    ComPtr<IDiagnosticsLifetime> life;
    Api(){std::lock_guard lock(site_mutex);diag=diagnostics;tree=service;life=lifetime;}
};
void CALLBACK release_handles(HWND,UINT,UINT_PTR id,DWORD) {
    Api api;auto handles=std::move(release_queue);release_queue.clear();
    KillTimer(nullptr,id);release_timer=0;
    if(api.life)for(auto handle:handles)api.life->UnregisterInstance(handle);
}
void save_property(Element& element, Api& api, InstanceHandle handle, const wchar_t* name) {
    operation="save "+utf8(name);
    if(element.saved.contains(name)) return;
    Chain chain(api.tree.Get(),handle);auto entry=chain.find(name);
    if(!entry || entry->PropertyChainIndex>=chain.sources_count) winrt::throw_hresult(E_NOTIMPL);
    if(entry->MetadataBits & IsValueBindingExpression) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    SavedProperty saved; saved.index=entry->Index;
    saved.local=chain.sources[entry->PropertyChainIndex].Source==BaseValueSourceLocal;
    if(saved.local) {
        InstanceHandle value=0;winrt::check_hresult(api.tree->GetProperty(handle,saved.index,&value));
        auto hr=api.diag->GetIInspectableFromHandle(value,&saved.value);
        if(value) api.life->UnregisterInstance(value);
        winrt::check_hresult(hr);
    }
    element.saved.emplace(name,std::move(saved));
}
void set_property(Api& api, InstanceHandle handle, const wchar_t* name, const wchar_t* type, const std::wstring& value) {
    operation="index "+utf8(name);
    Chain chain(api.tree.Get(),handle);auto property=chain.find(name);
    if(!property)winrt::throw_hresult(E_NOTIMPL);auto index=property->Index;
    auto type_bstr=SysAllocString(type), value_bstr=SysAllocString(value.c_str());
    operation="create "+utf8(name)+" ("+utf8(type)+"="+utf8(value)+")";
    InstanceHandle boxed=0;auto hr=api.tree->CreateInstance(type_bstr,value_bstr,&boxed);
    SysFreeString(type_bstr);SysFreeString(value_bstr);winrt::check_hresult(hr);
    operation="set "+utf8(name);hr=api.tree->SetProperty(handle,boxed,index);
    if(boxed) api.life->UnregisterInstance(boxed);
    winrt::check_hresult(hr);
    geometry_changed=true;
}
void restore_element(Element& element, Api& api, InstanceHandle handle) {
    if(!element.saved.empty())geometry_changed=true;
    // Widen constraints before restoring width and its original constraints.
    if(!element.tab_view && !element.saved.empty()) {
        set_property(api,handle,L"MinWidth",L"Double",L"0");
        set_property(api,handle,L"MaxWidth",L"Double",L"Infinity");
    }
    for(const auto* name:{L"Width",L"MaxWidth",L"MinWidth",L"TabWidthMode"}) {
        auto it=element.saved.find(name);if(it==element.saved.end())continue;
        const auto& p=it->second;
        if(p.local) {Registered original(p.value.Get(),api.diag.Get(),api.life.Get());winrt::check_hresult(api.tree->SetProperty(handle,original.handle,p.index));}
        else winrt::check_hresult(api.tree->ClearProperty(handle,p.index));
    }
    element.saved.clear();
}
void apply_element(Element& element, Api& api, InstanceHandle handle) {
    if(element.tab_view) {
        if(!element.saved.empty())return; // Both custom modes use SizeToContent.
        save_property(element,api,handle,L"TabWidthMode");
        set_property(api,handle,L"TabWidthMode",L"Microsoft.UI.Xaml.Controls.TabViewWidthMode",L"SizeToContent");
        return;
    }
    // Avoid resetting constraints on every lease renewal: it needlessly
    // invalidates layout. Explorer may still write Width when a tab is added.
    {
        Chain current(api.tree.Get(),handle);
        auto equal=[&](const wchar_t* name,int expected){
            auto property=current.find(name);if(!property||!property->Value)return false;
            try{return std::abs(std::stod(property->Value)-expected)<0.01;}catch(...){return false;}
        };
        auto width=current.find(L"Width");
        const bool natural=width&&width->Value&&wcscmp(width->Value,L"Auto")==0;
        if(!element.saved.empty()&&equal(L"MinWidth",options.width_mode=="fixed"?options.width:options.min_width)&&
            equal(L"MaxWidth",options.width_mode=="fixed"?options.width:options.max_width)&&
            (options.width_mode=="fixed"?equal(L"Width",options.width):natural))return;
    }
    for(const auto* name:{L"Width",L"MinWidth",L"MaxWidth"})save_property(element,api,handle,name);
    set_property(api,handle,L"MinWidth",L"Double",L"0");
    set_property(api,handle,L"MaxWidth",L"Double",L"Infinity");
    set_property(api,handle,L"Width",L"Double",options.width_mode=="fixed"?std::to_wstring(options.width):L"NaN");
    set_property(api,handle,L"MaxWidth",L"Double",std::to_wstring(options.width_mode=="fixed"?options.width:options.max_width));
    set_property(api,handle,L"MinWidth",L"Double",std::to_wstring(options.width_mode=="fixed"?options.width:options.min_width));
}
void refresh(bool restore) {
    Api api;if(!api.tree)return;
    // Applying the TabView first lets native layout update its item widths.
    // Restoring the TabView last lets its original mode recalculate all tabs.
    for(bool view:restore?std::initializer_list<bool>{false,true}:std::initializer_list<bool>{true,false}) {
        for(auto it=elements.begin();it!=elements.end();) {
            auto object=resolve(*it);
            if(!object){it=elements.erase(it);continue;}
            if(it->tab_view==view) {
                Registered instance(object.Get(),api.diag.Get(),api.life.Get());
                if(restore)restore_element(*it,api,instance.handle);else apply_element(*it,api,instance.handle);
            }
            ++it;
        }
    }
}
Json inspect() {
    Api api;Json result=Json::array();
    if(!api.tree)return result;
    for(const auto& element:elements) {
        auto object=resolve(element);if(!object)continue;
        Registered instance(object.Get(),api.diag.Get(),api.life.Get());Chain chain(api.tree.Get(),instance.handle);
        Json item{{"kind",element.tab_view?"TabView":"TabViewItem"}};
        for(const auto* name:{L"Width",L"MinWidth",L"MaxWidth",L"ActualWidth"}) {
            if(auto value=chain.find(name)) {
                item[utf8(name)]=value->Value?utf8(value->Value):"";
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}
void CALLBACK lease_timer(HWND,UINT,UINT_PTR id,DWORD) {
    if(expires>GetTickCount64())return;
    try{refresh(true);last_error.clear();}catch(...){last_error="Layout restoration failed";return;}
    owner=0;owner_started=0;expires=0;options={};KillTimer(nullptr,id);timer=0;
}
class Watcher final : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IVisualTreeServiceCallback2> {
public:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation,VisualElement element,VisualMutationType mutation) override {
        Api api;
        const char* stage="callback";
        try {
            if(mutation==Remove) {
                std::erase_if(elements,[&](const auto& e){return e.id==element.Handle;});
            } else if(element.Type && (wcscmp(element.Type,L"Microsoft.UI.Xaml.Controls.TabViewItem")==0 || wcscmp(element.Type,L"Microsoft.UI.Xaml.Controls.TabView")==0)) {
                stage="GetIInspectableFromHandle";ComPtr<IInspectable> object;
                auto object_hr=api.diag->GetIInspectableFromHandle(element.Handle,&object);
                if(object_hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))object_hr=identity_from_callback(element.Handle,&object);
                winrt::check_hresult(object_hr);
                HSTRING runtime_name{};winrt::check_hresult(object->GetRuntimeClassName(&runtime_name));
                std::wstring class_name(WindowsGetStringRawBuffer(runtime_name,nullptr));WindowsDeleteString(runtime_name);
                if(class_name!=element.Type)winrt::throw_hresult(E_NOINTERFACE);
                stage="IWeakReferenceSource";ComPtr<IWeakReferenceSource> weak_source;winrt::check_hresult(object.As(&weak_source));
                Element record;record.id=element.Handle;record.tab_view=wcscmp(element.Type,L"Microsoft.UI.Xaml.Controls.TabView")==0;
                stage="GetWeakReference";winrt::check_hresult(weak_source->GetWeakReference(&record.weak));
                auto found=std::find_if(elements.begin(),elements.end(),[&](const auto& e){return e.id==element.Handle;});
                if(found==elements.end())elements.push_back(std::move(record));
                stage="refresh";if(expires>GetTickCount64()&&options.width_mode!="system")refresh(false);
            }
        } catch(...){last_error=std::string(stage)+": "+std::to_string(static_cast<int32_t>(winrt::to_hresult()));}
        // Initial enumeration supplies a whole batch. Releasing a parent in
        // the middle of that batch can invalidate its not-yet-delivered children.
        if(api.life&&mutation==Add) {
            release_queue.push_back(element.Handle);
            if(!release_timer)release_timer=SetTimer(nullptr,0,100,release_handles);
        }
        SysFreeString(element.Type);SysFreeString(element.Name);SysFreeString(element.SrcInfo.FileName);SysFreeString(element.SrcInfo.Hash);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,VisualElementState,LPCWSTR) override {return S_OK;}
};
ComPtr<Watcher> watcher;
class Tap final : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IObjectWithSite> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        try {
            if(!site){initialization=E_ABORT;return S_OK;}
            ComPtr<IXamlDiagnostics> diag;ComPtr<IVisualTreeService2> tree;ComPtr<IDiagnosticsLifetime> life;
            initialization_stage="IXamlDiagnostics";winrt::check_hresult(site->QueryInterface(IID_PPV_ARGS(&diag)));
            initialization_stage="IVisualTreeService2";winrt::check_hresult(site->QueryInterface(IID_PPV_ARGS(&tree)));
            initialization_stage="IDiagnosticsLifetime";
            auto lifetime_hr=site->QueryInterface(IID_PPV_ARGS(&life));
            if(FAILED(lifetime_hr)) {
                constexpr GUID internal_lifetime{0x735941a2,0x3ee3,0x495a,{0x8d,0xa9,0x97,0x26,0x27,0x00,0x30,0x75}};
                lifetime_hr=site->QueryInterface(internal_lifetime,reinterpret_cast<void**>(life.GetAddressOf()));
            }
            winrt::check_hresult(lifetime_hr);
            {std::lock_guard lock(site_mutex);diagnostics=diag;service=tree;lifetime=life;watcher=Microsoft::WRL::Make<Watcher>();}
            auto thread=CreateThread(nullptr,0,[](void*)->DWORD {
                Api api;initialization_stage="AdviseVisualTreeChange";auto hr=api.tree->AdviseVisualTreeChange(watcher.Get());initialization=hr;return 0;
            },nullptr,0,nullptr);
            if(!thread)winrt::throw_last_error();CloseHandle(thread);return S_OK;
        } catch(...){return initialization=winrt::to_hresult();}
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid,void** output) override {
        std::lock_guard lock(site_mutex);if(!output)return E_POINTER;*output=nullptr;
        return diagnostics?diagnostics->QueryInterface(iid,output):E_FAIL;
    }
};
class Factory final : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IClassFactory> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;if(outer)return CLASS_E_NOAGGREGATION;
        auto instance=Microsoft::WRL::Make<Tap>();return instance?instance->QueryInterface(iid,output):E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override{return S_OK;}
};
void initialize() {
    if(started.exchange(true))return;
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<PCWSTR>(initialize),&pinned)) {
        initialization=HRESULT_FROM_WIN32(GetLastError());return;
    }
    auto thread=CreateThread(nullptr,0,[](void*)->DWORD {
        auto module=GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
        auto initialize_xaml=module?reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(GetProcAddress(module,"InitializeXamlDiagnosticsEx")):nullptr;
        if(!initialize_xaml){initialization=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);return 0;}
        wchar_t path[32768]{};if(!GetModuleFileNameW(self,path,32768)){initialization=HRESULT_FROM_WIN32(GetLastError());return 0;}
        HRESULT hr=HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        for(int i=1;i<=10000;++i) {
            auto name=L"WinUIVisualDiagConnection"+std::to_wstring(i);
            hr=initialize_xaml(name.c_str(),GetCurrentProcessId(),L"",path,tap_class,nullptr);
            if(hr!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND))break;
        }
        if(FAILED(hr))initialization=hr;return 0;
    },nullptr,0,nullptr);
    if(thread)CloseHandle(thread);else initialization=HRESULT_FROM_WIN32(GetLastError());
}
void execute(LayoutPacket& packet) {
    verify_layout_sender(self,packet.sender_pid,packet.sender_started,packet.action==LayoutAction::inspect);
    if(!native_inventory_supported())throw Error(3,"当前 Explorer 版本尚无布局适配");
    DWORD pid{};auto thread=GetWindowThreadProcessId(reinterpret_cast<HWND>(packet.window),&pid);
    if(pid!=GetCurrentProcessId()||thread!=GetCurrentThreadId()||bridge_process_started(GetCurrentProcess())!=packet.target_started)
        throw Error(4,"布局目标身份或 UI 线程已变化");
    if(packet.action!=LayoutAction::apply&&packet.action!=LayoutAction::restore&&packet.action!=LayoutAction::inspect)throw Error(2,"未知布局操作");
    if(packet.mode>2)throw Error(2,"未知布局模式");
    TabAppearance next{packet.mode==1?"fixed":packet.mode==2?"content":"system",packet.width,packet.min_width,packet.max_width};
    validate_appearance(next);
    initialize();
    auto hr=initialization.load();
    if(FAILED(hr)) {
        packet.result=hr;auto text=Json{{"layout_applied",false},{"message",initialization_stage.load()}}.dump();strcpy_s(packet.report,text.c_str());return;
    }
    if(packet.action!=LayoutAction::inspect) {
        geometry_changed=false;
        if(owner && expires>GetTickCount64() && (owner!=packet.sender_pid||owner_started!=packet.sender_started))throw Error(4,"标签宽度由其他代理控制");
        const bool restore=packet.action==LayoutAction::restore||next.width_mode=="system";
        if(!restore) {
            if(!timer){timer=SetTimer(nullptr,0,1000,lease_timer);if(!timer)throw Error(1,"无法创建宽度恢复计时器");}
            options=next;owner=packet.sender_pid;owner_started=packet.sender_started;expires=GetTickCount64()+10000;
        }
        try {refresh(restore);last_error.clear();}
        catch(...) {
            auto error=winrt::to_hresult();auto failed_operation=operation;try{refresh(true);}catch(...){}
            expires=0;operation=failed_operation;throw winrt::hresult_error(error);
        }
        if(restore) {if(timer)KillTimer(nullptr,timer);timer=0;owner=0;expires=0;owner_started=0;options={};}
    }
    auto observations=inspect();size_t count=0;bool measured=true;
    for(const auto& item:observations)if(item.at("kind")=="TabViewItem") {
        ++count;
        try {
            auto actual=std::stod(item.value("ActualWidth",std::string{}));
            if(!std::isfinite(actual)||actual<=0)measured=false;
            else if(options.width_mode=="fixed"&&std::abs(actual-options.width)>1.0)measured=false;
            else if(options.width_mode=="content"&&(actual<options.min_width-1.0||actual>options.max_width+1.0))measured=false;
            auto width=item.value("Width",std::string{});
            if(options.width_mode=="content"&&width!="Auto")measured=false;
            if(options.width_mode=="system"&&width!="Auto"&&std::abs(actual-std::stod(width))>1.0)measured=false;
        }catch(...){measured=false;}
    }
    Json report{{"layout_applied",count>0&&measured&&last_error.empty()&&!geometry_changed},{"tab_count",count},{"mode",options.width_mode},
        {"elements",observations},{"error",last_error},{"measurement","XAML ActualWidth (DIP)"}};
    auto text=report.dump();if(text.size()>=sizeof(packet.report))throw Error(2,"布局报告过大");
    strcpy_s(packet.report,text.c_str());packet.result=S_OK;geometry_changed=false;
}
void process(LayoutPacket* packet) {
    try {
        if(packet->magic!=0x54544c59||packet->version!=1||packet->size!=sizeof(LayoutPacket))throw Error(2,"无效布局报文");
        auto copy=std::make_unique<LayoutPacket>(*packet);execute(*copy);packet->result=copy->result;
        std::copy(std::begin(copy->report),std::end(copy->report),std::begin(packet->report));
    }catch(const Error& e){packet->result=HRESULT_FROM_WIN32(e.code==4?ERROR_ACCESS_DENIED:ERROR_NOT_SUPPORTED);}
    catch(...){packet->result=winrt::to_hresult();auto text=Json{{"operation",operation},{"error",system_error("XAML",static_cast<DWORD>(packet->result))}}.dump();strcpy_s(packet->report,text.c_str());}
    InterlockedExchange(&packet->completed,1);
}
void protected_process(LayoutPacket* packet) {
    __try{process(packet);}__except(EXCEPTION_EXECUTE_HANDLER){packet->result=E_UNEXPECTED;InterlockedExchange(&packet->completed,1);}
}
}
STDAPI DllGetClassObject(REFCLSID clsid,REFIID iid,void** output) {
    if(!output)return E_POINTER;*output=nullptr;if(clsid!=sinan::layout::tap_class)return CLASS_E_CLASSNOTAVAILABLE;
    auto factory=Microsoft::WRL::Make<sinan::layout::Factory>();return factory?factory->QueryInterface(iid,output):E_OUTOFMEMORY;
}
STDAPI DllCanUnloadNow(){return S_FALSE;}
extern "C" __declspec(dllexport) LRESULT CALLBACK SinanLayoutHook(int code,WPARAM wparam,LPARAM lparam) {
    using namespace sinan;
    if(code>=0) {
        auto message=reinterpret_cast<const CWPSTRUCT*>(lparam);
        if(message&&message->message==RegisterWindowMessageW(layout_message)) {
            auto sender=static_cast<DWORD>(message->wParam);auto nonce=static_cast<uint64_t>(message->lParam);
            Handle mapping(OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,layout_mapping(sender,nonce).c_str()));
            if(mapping) {
                auto packet=static_cast<LayoutPacket*>(MapViewOfFile(mapping.get(),FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(LayoutPacket)));
                if(packet) {
                    if(packet->nonce==nonce&&packet->sender_pid==sender&&packet->window==reinterpret_cast<uintptr_t>(message->hwnd))layout::protected_process(packet);
                    UnmapViewOfFile(packet);
                }
            }
        }
    }
    return CallNextHookEx(nullptr,code,wparam,lparam);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH)sinan::layout::self=module;return TRUE;
}
