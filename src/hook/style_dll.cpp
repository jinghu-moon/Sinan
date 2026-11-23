#include "compat/symbols.hpp"
#include "hook/style.hpp"
#include "hook/style_abi.hpp"
#include "hook/tab_icon.hpp"
#include "hook/identity.hpp"
#include "hook/xaml_visual_tree.hpp"
#include <xamlOM.h>
#include <ocidl.h>
#include <weakreference.h>
#include <wrl.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <list>
#include <mutex>
#include <unordered_map>

// All styling executes on the owning XAML thread. Native tab identity is
// gated by verified module contracts; properties are resolved by diagnostics.
namespace sinan::style {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
constexpr CLSID tap_class{0x7094f37f,0x60b9,0x4c80,{0xac,0x02,0x66,0x57,0x18,0x2e,0xd2,0xc1}};
// The diagnostics runtime retains every reported object. Hand that reference
// back after making a weak reference, including for objects we don't style.
MIDL_INTERFACE("7776037B-CBBE-4DE1-AD6F-0DBF1D5817B4") IDiagnosticsLifetime : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(InstanceHandle, void**) = 0;
};
// Public WinRT ABI from Microsoft.UI.Xaml.winmd. Only the first two methods
// are needed; Resources is a CLR property and is absent from the DP chain.
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
struct SavedAttached {ComPtr<IInspectable> property,original;bool local=false;};
struct Element {
    InstanceHandle id = 0;
    bool tab_view = false;
    InstanceHandle parent=0;
    std::wstring type,name;
    ComPtr<IWeakReference> weak;
    std::map<std::wstring,SavedProperty> saved;
    std::map<std::wstring,std::string> applied_values;
    std::map<std::wstring,ComPtr<IInspectable>> resources;
    std::string background_resource_color,state_description;std::vector<SavedAttached> attached;
    ComPtr<IInspectable> replacement_icon,icon_geometry;
    std::string icon_signature,icon_color;
};
thread_local std::list<Element> elements;
thread_local std::unordered_map<InstanceHandle,Element*> element_index;
thread_local bool refreshing=false;thread_local std::set<InstanceHandle> retired;
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
        InstanceHandle value=0;operation="GetProperty original "+utf8(name);winrt::check_hresult(api.tree->GetProperty(handle,saved.index,&value));
        operation="resolve original "+utf8(name)+" handle="+std::to_string(value)+" chain="+(entry->Value?utf8(entry->Value):"null")+" flags="+std::to_string(entry->MetadataBits);
        auto hr=api.diag->GetIInspectableFromHandle(value,&saved.value);
        if(FAILED(hr)&&entry->Value)try {auto numeric=std::stoull(entry->Value);hr=identity_from_callback(numeric,&saved.value);}catch(...){}
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
template<class T>T native_read(uintptr_t address) {
    T value{};SIZE_T n{};if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(value),&n)||n!=sizeof(value))throw Error(3,"Style identity layout changed");return value;
}
void verify_style_build() {
    // Cache successful live PE/PDB checks briefly on this XAML thread. Do not
    // rehash large files while traversing individual tab elements.
    thread_local uint64_t verified_until=0;
    thread_local const compat::Profile *last_view=nullptr,*last_ui=nullptr;
    auto view=compat::current_xaml_profile(compat::Module::view),ui=compat::current_xaml_profile(compat::Module::ui);
    if(!view->xaml_identity||!ui->xaml_identity)throw Error(3,"Explorer XAML identity contracts unavailable; run symbols prepare: "+view->reason+"; "+ui->reason);
    if(GetTickCount64()<verified_until&&last_view==view.get()&&last_ui==ui.get())return;
    compat::verify_loaded_module(GetModuleHandleW(L"FileExplorerExtensions.dll"),*view,compat::module_path(compat::Module::view));
    compat::verify_loaded_module(GetModuleHandleW(L"Windows.UI.FileExplorer.dll"),*ui,compat::module_path(compat::Module::ui));
    last_view=view.get();last_ui=ui.get();verified_until=GetTickCount64()+5000;
}
GUID context_guid(IInspectable* context) {
    verify_style_build();
    auto view=compat::current_xaml_profile(compat::Module::view),ui=compat::current_xaml_profile(compat::Module::ui);
    constexpr GUID iid{0x73208773,0x3919,0x5686,{0x8c,0x0b,0x20,0xfa,0xf7,0x9d,0x5b,0xec}};
    ComPtr<IInspectable> vm;winrt::check_hresult(context->QueryInterface(iid,&vm));
    auto vt=native_read<uintptr_t>(reinterpret_cast<uintptr_t>(vm.Get()));
    auto view_base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"FileExplorerExtensions.dll"));
    if(vt!=view_base+view->rva("view_vtable"))throw Error(3,"Tab ViewModel interface vtable changed");
    auto get=native_read<uintptr_t>(vt+view->field("view_get_slot")*sizeof(uintptr_t));
    if(get!=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"FileExplorerExtensions.dll"))+view->rva("view_get"))throw Error(3,"Tab ViewModel vtable changed");
    constexpr std::array<BYTE,16> expected{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x8b,0xc1,0x48,0x83,0x22,0};
    if(native_read<std::array<BYTE,16>>(get)!=expected)throw Error(3,"Live ViewModel getter changed or is hooked");
    using Get=HRESULT(WINAPI*)(void*,IInspectable**);ComPtr<IInspectable> tab;
    winrt::check_hresult(reinterpret_cast<Get>(get)(vm.Get(),&tab));
    if(native_read<uintptr_t>(reinterpret_cast<uintptr_t>(tab.Get()))!=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Windows.UI.FileExplorer.dll"))+ui->rva("tab_vtable"))throw Error(3,"FileExplorerTab vtable changed");
    return native_read<GUID>(reinterpret_cast<uintptr_t>(tab.Get())+ui->field("tab_guid"));
}GUID element_guid(Api& api,InstanceHandle handle) {
    verify_style_build();Chain chain(api.tree.Get(),handle);auto property=chain.find(L"DataContext");if(!property)throw Error(3,"No tab ViewModel");
    InstanceHandle value{};winrt::check_hresult(api.tree->GetProperty(handle,property->Index,&value));
    ComPtr<IInspectable> context;auto hr=api.diag->GetIInspectableFromHandle(value,&context);if(FAILED(hr)&&property->Value)try{hr=identity_from_callback(std::stoull(property->Value),&context);}catch(...){}if(value)api.life->UnregisterInstance(value);winrt::check_hresult(hr);
    return context_guid(context.Get());
}
struct StyledTab {NativeTab tab;Json style;DWORD owner{};uint64_t owner_started{},expires{};std::string error;size_t applied{};std::string description;bool text_seen=false,icon_applied=false,mapped=false;};
thread_local std::vector<StyledTab> styles;
Element* parent_tab(Element& element) {
    auto current=&element;
    for(int i=0;i<32;++i){if(current->type==L"Microsoft.UI.Xaml.Controls.TabViewItem")return current;
        auto it=element_index.find(current->parent);if(it==element_index.end())return nullptr;current=it->second;}
    return nullptr;
}
bool has_ancestor(Element& e,const std::wstring& name) {
    auto current=&e;
    for(int i=0;i<32;++i){if(current->name==name)return true;auto it=element_index.find(current->parent);if(it==element_index.end())return false;current=it->second;}return false;
}
bool same_object(IInspectable* a,IInspectable* b);
ComPtr<IInspectable> object_property(Api& api,InstanceHandle handle,const wchar_t* name);
void restore_one(Element& e,Api& api,InstanceHandle handle,const std::wstring& name) {
    auto it=e.saved.find(name);if(it==e.saved.end())return;
    auto& p=it->second;if(p.local){Registered original(p.value.Get(),api.diag.Get(),api.life.Get());winrt::check_hresult(api.tree->SetProperty(handle,original.handle,p.index));}
    else winrt::check_hresult(api.tree->ClearProperty(handle,p.index));
    if(name==L"IconSource"&&p.local){auto actual=object_property(api,handle,L"IconSource");if(!same_object(actual.Get(),p.value.Get()))throw Error(5,"Original tab IconSource restoration readback mismatch");}
    e.saved.erase(it);e.applied_values.erase(name);
    if(name==L"IconSource"){e.replacement_icon.Reset();e.icon_geometry.Reset();e.icon_signature.clear();e.icon_color.clear();}
}
std::wstring property_value(Api& api,InstanceHandle handle,const wchar_t* name) {
    Chain chain(api.tree.Get(),handle);auto v=chain.find(name);return v&&v->Value?v->Value:L"";
}
void color_brush(Api& api,InstanceHandle handle,const wchar_t* name,const std::string& color) {
    set_property(api,handle,name,L"Microsoft.UI.Xaml.Media.Brush",wide(color));
}
InstanceHandle create_value(Api& api,const wchar_t* type,const std::wstring& value) {
    auto t=SysAllocString(type),v=SysAllocString(value.c_str());InstanceHandle result{};auto hr=api.tree->CreateInstance(t,v,&result);SysFreeString(t);SysFreeString(v);winrt::check_hresult(hr);return result;
}
void background_resources(Element& e,Api& api,InstanceHandle handle,const Json& colors) {
    auto signature=colors.dump();if(e.background_resource_color==signature)return;
    if(colors.empty()&&e.resources.empty())return;
    ComPtr<IVisualTreeService3> tree;winrt::check_hresult(api.tree.As(&tree));
    auto object=resolve(e);ComPtr<IElementResources> element;winrt::check_hresult(object.As(&element));
    ComPtr<IInspectable> dictionary_object;winrt::check_hresult(element->get_Resources(&dictionary_object));
    Registered dictionary(dictionary_object.Get(),api.diag.Get(),api.life.Get());
    for(auto [name,state]:{std::pair{L"TabViewItemHeaderBackground","normal"},std::pair{L"TabViewItemHeaderBackgroundPointerOver","hover"},std::pair{L"TabViewItemHeaderBackgroundSelected","selected"},std::pair{L"TabViewItemHeaderBackgroundSelectedPointerOver","selected"}}) {
        auto key=create_value(api,L"String",name);auto color=colors.value(state,std::string("system"));
        try {
            if(color=="system") {
                auto original=e.resources.find(name);
                if(original!=e.resources.end()) {
                    if(original->second){Registered value(original->second.Get(),api.diag.Get(),api.life.Get());winrt::check_hresult(tree->AddDictionaryItem(dictionary.handle,key,value.handle));}
                    else {auto hr=tree->RemoveDictionaryItem(dictionary.handle,key);if(FAILED(hr)&&hr!=E_BOUNDS&&hr!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND))winrt::check_hresult(hr);}
                    e.resources.erase(original);
                }
            }else {
                if(!e.resources.contains(name)) {
                    InstanceHandle value{};ComPtr<IInspectable> original;
                    if(SUCCEEDED(tree->GetDictionaryItem(dictionary.handle,name,FALSE,&value))&&value){auto hr=api.diag->GetIInspectableFromHandle(value,&original);if(FAILED(hr))hr=identity_from_callback(value,&original);api.life->UnregisterInstance(value);winrt::check_hresult(hr);}
                    e.resources[name]=original;
                }
                auto brush=create_value(api,L"Microsoft.UI.Xaml.Media.Brush",wide(color));
                auto hr=tree->AddDictionaryItem(dictionary.handle,key,brush);api.life->UnregisterInstance(brush);winrt::check_hresult(hr);
            }
        }catch(...){api.life->UnregisterInstance(key);throw;}api.life->UnregisterInstance(key);
    }
    // Refresh ThemeResource references once per effective palette/theme change.
    // Preserve the original local/resource source of RequestedTheme exactly.
    save_property(e,api,handle,L"RequestedTheme");auto actual_theme=property_value(api,handle,L"ActualTheme");
    set_property(api,handle,L"RequestedTheme",L"Microsoft.UI.Xaml.ElementTheme",actual_theme==L"2"?L"Light":L"Dark");
    restore_one(e,api,handle,L"RequestedTheme");e.background_resource_color=signature;
}
template<class T>ComPtr<T> factory(const wchar_t* name) {ComPtr<T> out;auto text=winrt::hstring(name);winrt::check_hresult(RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(text)),IID_PPV_ARGS(&out)));return out;}
bool same_object(IInspectable* a,IInspectable* b) {
    if(!a||!b)return a==b;ComPtr<IUnknown> left,right;winrt::check_hresult(a->QueryInterface(IID_PPV_ARGS(&left)));winrt::check_hresult(b->QueryInterface(IID_PPV_ARGS(&right)));return left.Get()==right.Get();
}
ComPtr<IInspectable> object_property(Api& api,InstanceHandle handle,const wchar_t* name) {
    Chain chain(api.tree.Get(),handle);auto prop=chain.find(name);if(!prop)throw Error(5,"XAML object property unavailable: "+utf8(name));
    InstanceHandle value{};winrt::check_hresult(api.tree->GetProperty(handle,prop->Index,&value));ComPtr<IInspectable> result;
    auto hr=api.diag->GetIInspectableFromHandle(value,&result);if(FAILED(hr)&&prop->Value)try{hr=identity_from_callback(std::stoull(prop->Value),&result);}catch(...){}
    if(value)api.life->UnregisterInstance(value);winrt::check_hresult(hr);return result;
}
void apply_tab_icon(Element& e,Api& api,InstanceHandle handle,const Json& icon,bool high_contrast) {
    auto source=icon.value("source",std::string("system"));if(source=="system"){restore_one(e,api,handle,L"IconSource");return;}
    auto color=icon.value("color",std::string("#F59E0B"));if(color=="system"||high_contrast){auto rgb=GetSysColor(COLOR_WINDOWTEXT);char text[8]{};sprintf_s(text,"#%02X%02X%02X",GetRValue(rgb),GetGValue(rgb),GetBValue(rgb));color=text;}
    auto signature=source+color;auto current=object_property(api,handle,L"IconSource");
    if(e.replacement_icon&&same_object(current.Get(),e.replacement_icon.Get())&&e.icon_signature==signature)return;
    // Explorer can publish a new folder icon during navigation. Remember that
    // source rather than restoring the icon of the previously visited folder.
    if(e.replacement_icon&&!same_object(current.Get(),e.replacement_icon.Get()))e.saved.erase(L"IconSource");
    save_property(e,api,handle,L"IconSource");
    bool pin=source=="builtin:pin-rounded";
    std::wstring xaml=L"<PathIconSource xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Foreground=\""+wide(color)+L"\" Data=\"";
    xaml+=pin?L"F1 ":L"F0 ";xaml+=pin?pin_geometry:lock_geometry;xaml+=L"\"/>";
    auto reader=factory<IXamlReader>(L"Microsoft.UI.Xaml.Markup.XamlReader");auto text=winrt::hstring(xaml);ComPtr<IInspectable> replacement;
    operation="load built-in PathIconSource";
    winrt::check_hresult(reader->Load(static_cast<HSTRING>(winrt::get_abi(text)),&replacement));ComPtr<IPathSource> path;winrt::check_hresult(replacement.As(&path));ComPtr<IInspectable> geometry;winrt::check_hresult(path->get_Data(&geometry));
    if(pin){auto rotation_text=winrt::hstring(L"<RotateTransform xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Angle=\"42\" CenterX=\"12\" CenterY=\"12\"/>");ComPtr<IInspectable> rotation;operation="load pin rotation";winrt::check_hresult(reader->Load(static_cast<HSTRING>(winrt::get_abi(rotation_text)),&rotation));ComPtr<IGeometryTransform> transform;winrt::check_hresult(geometry.As(&transform));winrt::check_hresult(transform->put_Transform(rotation.Get()));}
    Registered value(replacement.Get(),api.diag.Get(),api.life.Get());Chain chain(api.tree.Get(),handle);auto prop=chain.find(L"IconSource");if(!prop)throw Error(5,"Tab IconSource unavailable");
    winrt::check_hresult(api.tree->SetProperty(handle,value.handle,prop->Index));auto actual=object_property(api,handle,L"IconSource");
    if(!same_object(actual.Get(),replacement.Get()))throw Error(5,"Tab IconSource readback mismatch");
    e.replacement_icon=replacement;e.icon_geometry=geometry;e.icon_signature=signature;e.icon_color=color;
}
void restore_description(Element& e,IInspectable* object) {
    if(e.attached.empty())return;ComPtr<IDependencyValues> values;winrt::check_hresult(object->QueryInterface(IID_PPV_ARGS(&values)));
    for(auto it=e.attached.begin();it!=e.attached.end();){auto hr=it->local?values->SetValue(it->property.Get(),it->original.Get()):values->ClearValue(it->property.Get());winrt::check_hresult(hr);it=e.attached.erase(it);}e.state_description.clear();
}
void apply_description(Element& e,IInspectable* object,const std::string& description) {
    if(description.empty()){restore_description(e,object);return;}if(e.state_description==description)return;
    ComPtr<IDependencyValues> values;winrt::check_hresult(object->QueryInterface(IID_PPV_ARGS(&values)));
    if(e.attached.empty()) {
        auto tooltip=factory<IToolTipStatics>(L"Microsoft.UI.Xaml.Controls.ToolTipService");auto accessible=factory<IAccessibilityStatics>(L"Microsoft.UI.Xaml.Automation.AutomationProperties");
        auto unset=factory<IUnsetValue>(L"Microsoft.UI.Xaml.DependencyProperty");ComPtr<IInspectable> sentinel;winrt::check_hresult(unset->get_UnsetValue(reinterpret_cast<void**>(sentinel.GetAddressOf())));
        std::vector<SavedAttached> captured;for(int i=0;i<2;++i){SavedAttached saved;auto hr=i?accessible->get_HelpTextProperty(reinterpret_cast<void**>(saved.property.GetAddressOf())):tooltip->get_ToolTipProperty(reinterpret_cast<void**>(saved.property.GetAddressOf()));winrt::check_hresult(hr);
            winrt::check_hresult(values->ReadLocalValue(saved.property.Get(),reinterpret_cast<void**>(saved.original.GetAddressOf())));ComPtr<IUnknown> a,b;
            if(saved.original)saved.original.As(&a);sentinel.As(&b);saved.local=a.Get()!=b.Get();
            if(saved.local&&saved.original){HSTRING type{};if(SUCCEEDED(saved.original->GetRuntimeClassName(&type))){std::wstring name=WindowsGetStringRawBuffer(type,nullptr);WindowsDeleteString(type);if(name.find(L"BindingExpression")!=name.npos)throw Error(3,"Tab state description has an existing binding");}}
            captured.push_back(std::move(saved));}e.attached=std::move(captured);
    }
    auto text=winrt::box_value(winrt::hstring(wide(description)));
    for(auto& saved:e.attached){winrt::check_hresult(values->SetValue(saved.property.Get(),winrt::get_abi(text)));ComPtr<IInspectable> actual;winrt::check_hresult(values->GetValue(saved.property.Get(),reinterpret_cast<void**>(actual.GetAddressOf())));auto value=winrt::Windows::Foundation::IInspectable{actual.Get(),winrt::take_ownership_from_abi};actual.Detach();if(winrt::unbox_value<winrt::hstring>(value)!=wide(description))throw Error(5,"State description readback mismatch");}
    e.state_description=description;
}
void restore_element(Element& element,Api& api,InstanceHandle handle) {
    if(!element.attached.empty()){auto object=resolve(element);if(object)restore_description(element,object.Get());}
    if(!element.resources.empty())background_resources(element,api,handle,Json::object());
    std::vector<std::wstring> names;for(auto& [name,p]:element.saved)names.push_back(name);
    for(auto& name:names)restore_one(element,api,handle,name);
    element.saved.clear();element.applied_values.clear();element.replacement_icon.Reset();element.icon_geometry.Reset();element.icon_signature.clear();element.icon_color.clear();
}
void harvest_tab_tree(Api& api) {
    // Diagnostics can miss a just-realized child during initial attachment.
    // Recover from known TabView roots using public VisualTreeHelper APIs;
    // native GUID mapping still decides which exact control may be changed.
    ComPtr<ITreeHelper> helper;auto name=winrt::hstring(L"Microsoft.UI.Xaml.Media.VisualTreeHelper");
    if(FAILED(RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)),IID_PPV_ARGS(&helper))))return;
    std::vector<ComPtr<IInspectable>> roots;for(auto& e:elements)if(e.tab_view)if(auto o=resolve(e))roots.push_back(o);
    size_t visited=0;constexpr GUID dependency{0xe7beaee7,0x160e,0x50f7,{0x87,0x89,0xd6,0x34,0x63,0xf9,0x79,0xfa}};
    std::function<void(ComPtr<IInspectable>,InstanceHandle,int)> visit=[&](ComPtr<IInspectable> object,InstanceHandle parent,int depth){
        if(++visited>4096||depth>40)return;Registered instance(object.Get(),api.diag.Get(),api.life.Get());
        auto found=std::find_if(elements.begin(),elements.end(),[&](auto& e){return e.id==instance.handle;});
        if(found==elements.end()) {
            Element record;record.id=instance.handle;record.parent=parent;
            HSTRING class_name{};winrt::check_hresult(object->GetRuntimeClassName(&class_name));record.type=WindowsGetStringRawBuffer(class_name,nullptr);WindowsDeleteString(class_name);
            ComPtr<IWeakReferenceSource> source;winrt::check_hresult(object.As(&source));winrt::check_hresult(source->GetWeakReference(&record.weak));
            ComPtr<IElementResources> framework;if(SUCCEEDED(object.As(&framework))){HSTRING label{};if(SUCCEEDED(framework->get_Name(reinterpret_cast<void**>(&label)))){if(label)record.name=WindowsGetStringRawBuffer(label,nullptr);WindowsDeleteString(label);}}
            elements.push_back(std::move(record));
        }else if(parent)found->parent=parent;
        ComPtr<IInspectable> dep;if(FAILED(object->QueryInterface(dependency,&dep)))return;
        int32_t count{};if(FAILED(helper->GetChildrenCount(dep.Get(),&count))||count<0||count>4096)return;
        for(int32_t i=0;i<count;++i){ComPtr<IInspectable> child;if(SUCCEEDED(helper->GetChild(dep.Get(),i,&child))&&child)visit(child,instance.handle,depth+1);}
    };
    for(auto& root:roots)visit(root,0,0);
}
Json target_visual_tree(const NativeTab& tab) {
    Api api;auto helper=factory<ITreeHelper>(L"Microsoft.UI.Xaml.Media.VisualTreeHelper");Json out=Json::array();ComPtr<IInspectable> root;
    for(auto& e:elements)if(e.type==L"Microsoft.UI.Xaml.Controls.TabViewItem")try{auto o=resolve(e);if(o){Registered h(o.Get(),api.diag.Get(),api.life.Get());if(element_guid(api,h.handle)==tab.guid){root=o;break;}}}catch(...){}
    std::function<void(ComPtr<IInspectable>,int)> visit=[&](ComPtr<IInspectable> object,int depth){if(out.size()>=256||depth>32)return;HSTRING raw{};winrt::check_hresult(object->GetRuntimeClassName(&raw));std::wstring name(WindowsGetStringRawBuffer(raw,nullptr));WindowsDeleteString(raw);Json item{{"kind",utf8(name)},{"depth",depth}};
        Registered h(object.Get(),api.diag.Get(),api.life.Get());Chain chain(api.tree.Get(),h.handle);
        for(auto key:{L"Name",L"IconSource",L"Source",L"Content",L"Foreground",L"Data",L"ActualWidth"})if(auto p=chain.find(key))item[utf8(key)]={{"type",p->ValueType?utf8(p->ValueType):""},{"value",p->Value?utf8(p->Value):""}};
        for(auto key:{L"Foreground",L"Fill"})if(chain.find(key))try{auto brush=object_property(api,h.handle,key);if(brush){Registered paint(brush.Get(),api.diag.Get(),api.life.Get());item[utf8(key)+"Color"]=utf8(property_value(api,paint.handle,L"Color"));}}catch(...){}
        out.push_back(item);ComPtr<IDependencyValues> dep;if(FAILED(object.As(&dep)))return;int32_t count{};winrt::check_hresult(helper->GetChildrenCount(dep.Get(),&count));for(int i=0;i<count;++i){ComPtr<IInspectable> child;winrt::check_hresult(helper->GetChild(dep.Get(),i,&child));if(child)visit(child,depth+1);}
    };if(root)visit(root,0);return out;
}
bool verify_rendered_icon(Api& api,IInspectable* root,IInspectable* geometry,const std::string& expected_color) {
    auto helper=factory<ITreeHelper>(L"Microsoft.UI.Xaml.Media.VisualTreeHelper");size_t visited=0;
    std::function<bool(IInspectable*,int)> visit=[&](IInspectable* object,int depth){
        if(++visited>256||depth>32)return false;
        ComPtr<IRenderedPathIcon> path;if(SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&path)))) {
            ComPtr<IInspectable> data;winrt::check_hresult(path->get_Data(&data));
            if(same_object(data.Get(),geometry)) {
                Registered h(object,api.diag.Get(),api.life.Get());auto brush=object_property(api,h.handle,L"Foreground");Registered paint(brush.Get(),api.diag.Get(),api.life.Get());auto color=property_value(api,paint.handle,L"Color"),expected=wide(expected_color);
                std::transform(color.begin(),color.end(),color.begin(),towupper);std::transform(expected.begin(),expected.end(),expected.begin(),towupper);
                if((color==expected||color==L"#FF"+expected.substr(1))&&std::stod(property_value(api,h.handle,L"ActualWidth"))>0)return true;
            }
        }
        ComPtr<IDependencyValues> dep;if(FAILED(object->QueryInterface(IID_PPV_ARGS(&dep))))return false;int32_t count{};winrt::check_hresult(helper->GetChildrenCount(dep.Get(),&count));
        for(int i=0;i<count;++i){ComPtr<IInspectable> child;winrt::check_hresult(helper->GetChild(dep.Get(),i,&child));if(child&&visit(child.Get(),depth+1))return true;}return false;
    };return root&&geometry&&visit(root,0);
}
void refresh(bool restore) {
    if(refreshing)return;refreshing=true;struct RefreshGuard{~RefreshGuard(){refreshing=false;}} guard;
    std::erase_if(elements,[](auto& e){return retired.contains(e.id);});retired.clear();
    Api api;if(!api.tree)return;
    static thread_local uint64_t harvested=0;if(!styles.empty()&&GetTickCount64()-harvested>2000)try{harvested=GetTickCount64();harvest_tab_tree(api);}catch(...){last_error="Visual tree recovery is pending";}
    element_index.clear();for(auto& e:elements)element_index[e.id]=&e;
    std::map<InstanceHandle,StyledTab*> mapping;
    for(auto& st:styles){st.applied=0;st.error.clear();st.text_seen=false;st.icon_applied=false;st.mapped=false;}
    for(auto& e:elements)if(e.type==L"Microsoft.UI.Xaml.Controls.TabViewItem")try {
        auto object=resolve(e);if(!object)continue;Registered instance(object.Get(),api.diag.Get(),api.life.Get());auto guid=element_guid(api,instance.handle);
        auto found=std::find_if(styles.begin(),styles.end(),[&](auto& st){return st.tab.guid==guid&&st.expires>GetTickCount64();});
        if(found!=styles.end()){mapping[e.id]=&*found;found->mapped=true;}
    }catch(const std::exception& ex){last_error=ex.what();}catch(...){last_error="Tab identity mapping failed";}
    for(auto& st:styles)if(!st.style.empty()&&std::none_of(mapping.begin(),mapping.end(),[&](auto& m){return m.second==&st;}))st.error="Waiting for the target tab XAML control";
    POINT cursor{};GetCursorPos(&cursor);RECT hit{cursor.x,cursor.y,cursor.x+1,cursor.y+1};unsigned hits_count{};InstanceHandle* hits{};
    std::set<InstanceHandle> hovered;
    if(SUCCEEDED(api.diag->HitTest(hit,&hits_count,&hits))) {
        for(unsigned i=0;i<hits_count;++i){auto it=std::find_if(elements.begin(),elements.end(),[&](auto& e){return e.id==hits[i];});if(it!=elements.end())if(auto parent=parent_tab(*it))hovered.insert(parent->id);}
        CoTaskMemFree(hits);
    }
    HIGHCONTRASTW hc{sizeof(hc)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);
    for(auto& e:elements) {
        auto parent=parent_tab(e);if(!parent&&e.saved.empty()&&e.resources.empty()&&e.attached.empty())continue;
        if(e.type!=L"Microsoft.UI.Xaml.Controls.TabViewItem"&&e.type!=L"Microsoft.UI.Xaml.Controls.TextBlock"&&e.saved.empty()&&e.resources.empty()&&e.attached.empty())continue;
        auto object=resolve(e);if(!object)continue;Registered instance(object.Get(),api.diag.Get(),api.life.Get());
        auto st=parent&&mapping.contains(parent->id)?mapping[parent->id]:nullptr;
        if(restore||!st){try{restore_element(e,api,instance.handle);}catch(...){last_error="A control could not yet be restored";}continue;}
        try {
            auto parent_object=resolve(*parent);Registered parent_instance(parent_object.Get(),api.diag.Get(),api.life.Get());
            auto selected=property_value(api,parent_instance.handle,L"IsSelected")==L"1";
            auto dark=property_value(api,parent_instance.handle,L"ActualTheme")==L"2";
            auto effective=style_for_theme(st->style,dark,selected,hovered.contains(parent->id),(hc.dwFlags&HCF_HIGHCONTRASTON)!=0);
            Json desired=Json::object();
            // Only the actual title TextBlock, never the close-button glyph.
            if(e.type==L"Microsoft.UI.Xaml.Controls.TextBlock"&&!has_ancestor(e,L"CloseButton")) {
                auto text=effective.value("text",Json::object());
                if(text.contains("font_family"))desired["FontFamily"]={{"type","Microsoft.UI.Xaml.Media.FontFamily"},{"value",text.at("font_family")}};
                if(text.contains("bold")&&text.at("bold")!="inherit")desired["FontWeight"]={{"type","Windows.UI.Text.FontWeight"},{"value",text.at("bold").is_boolean()?(text.at("bold").get<bool>()?Json("Bold"):Json("Normal")):Json("system")}};
                if(text.contains("font_color"))desired["Foreground"]={{"type","brush"},{"value",text.at("font_color")}};
            }
            if(e.type==L"Microsoft.UI.Xaml.Controls.TabViewItem") {
                auto icon=effective.value("icon",Json::object());if(icon.value("source",std::string("system"))!="system")desired["IconSource"]={{"type","icon"},{"value",icon.dump()}};
                apply_description(e,object.Get(),st->description);
                Json palette=Json::object();
                if(!(hc.dwFlags&HCF_HIGHCONTRASTON))for(auto state:{"normal","hover","selected"})palette[state]=style_for_theme(st->style,dark,std::string_view(state)=="selected",std::string_view(state)=="hover",false).value("background",std::string("system"));
                background_resources(e,api,instance.handle,palette);
                if(effective.contains("background"))desired["Background"]={{"type","brush"},{"value",effective.at("background")}};
            }
            std::vector<std::wstring> removed;for(auto& [name,p]:e.saved)if(!desired.contains(utf8(name))||desired.at(utf8(name)).at("value")=="system")removed.push_back(name);
            for(auto& name:removed)restore_one(e,api,instance.handle,name);
            for(auto it=desired.begin();it!=desired.end();++it) {
                auto value=it.value().at("value").get<std::string>();if(value=="system")continue;
                auto name=wide(it.key());auto current=property_value(api,instance.handle,name.c_str());
                auto type=it.value().at("type").get<std::string>();
                if(type=="icon"){apply_tab_icon(e,api,instance.handle,Json::parse(value),(hc.dwFlags&HCF_HIGHCONTRASTON)!=0);continue;}
                
                if(e.applied_values[name]==value&&type!="brush"&&current==wide(value))continue;
                if(e.applied_values[name]==value&&type=="brush") {
                    Chain chain(api.tree.Get(),instance.handle);auto prop=chain.find(name.c_str());InstanceHandle brush{};
                    if(prop&&SUCCEEDED(api.tree->GetProperty(instance.handle,prop->Index,&brush))&&brush) {
                        auto actual=property_value(api,brush,L"Color");api.life->UnregisterInstance(brush);
                        auto expected=wide(value);if(actual==expected||actual==L"#FF"+expected.substr(1))continue;
                    }
                }
                save_property(e,api,instance.handle,name.c_str());
                if(type=="brush")color_brush(api,instance.handle,name.c_str(),value);else set_property(api,instance.handle,name.c_str(),wide(type).c_str(),wide(value));
                auto actual=property_value(api,instance.handle,name.c_str());
                if(type!="brush"&&actual!=wide(value))throw Error(5,"Style property readback mismatch: "+it.key());
                if(type=="brush") {
                    Chain check(api.tree.Get(),instance.handle);auto prop=check.find(name.c_str());
                    if(!prop||!prop->Value)throw Error(5,"Brush readback unavailable");
                    ComPtr<IInspectable> brush;winrt::check_hresult(identity_from_callback(std::stoull(prop->Value),&brush));
                    Registered registered(brush.Get(),api.diag.Get(),api.life.Get());auto color=property_value(api,registered.handle,L"Color");
                    auto expected=wide(value);std::transform(color.begin(),color.end(),color.begin(),towupper);std::transform(expected.begin(),expected.end(),expected.begin(),towupper);
                    if(color!=expected&&color!=L"#FF"+expected.substr(1))throw Error(5,"Brush color readback mismatch: "+utf8(color));
                }
                e.applied_values[name]=value;
            }
            if(!desired.empty())++st->applied;
            if(e.type==L"Microsoft.UI.Xaml.Controls.TextBlock"&&!has_ancestor(e,L"CloseButton"))st->text_seen=true;
        }catch(const std::exception& ex){st->error=ex.what();}catch(const winrt::hresult_error& ex){st->error=operation+": "+utf8(ex.message().c_str())+" ("+std::to_string(static_cast<int32_t>(ex.code()))+")";}catch(...){st->error=operation+": "+std::to_string(static_cast<int32_t>(winrt::to_hresult()));}
    }
    for(auto& [id,st]:mapping)try {
        auto parent=std::find_if(elements.begin(),elements.end(),[&](auto& e){return e.id==id;});auto o=resolve(*parent);Registered h(o.Get(),api.diag.Get(),api.life.Get());
        bool selected=property_value(api,h.handle,L"IsSelected")==L"1",dark=property_value(api,h.handle,L"ActualTheme")==L"2";
        auto effective=style_for_theme(st->style,dark,selected,hovered.contains(id),(hc.dwFlags&HCF_HIGHCONTRASTON)!=0);
        if(st->error.empty()&&effective.contains("text")&&!effective.at("text").empty()&&!st->text_seen)st->error="Waiting for the tab title TextBlock";
        auto icon=effective.value("icon",Json::object());if(icon.value("source",std::string("system"))!="system") {
            // Confirm the realized tab visual, not WM_GETICON on its native host.
            bool matched=verify_rendered_icon(api,o.Get(),parent->icon_geometry.Get(),parent->icon_color);
            st->icon_applied=matched;if(!matched&&st->error.empty())st->error="Waiting for the rendered tab PathIcon geometry and color";
        }
        auto expected=effective.value("background",std::string("system"));if(expected!="system") {
            bool matched=false;for(auto& e:elements){if(e.name!=(selected?L"SelectedBackgroundPath":L"TabContainer"))continue;auto p=parent_tab(e);if(!p||p->id!=id)continue;
                auto object=resolve(e);if(!object)continue;Registered instance(object.Get(),api.diag.Get(),api.life.Get());Chain chain(api.tree.Get(),instance.handle);auto prop=chain.find(selected?L"Fill":L"Background");if(!prop||!prop->Value)continue;
                ComPtr<IInspectable> brush;winrt::check_hresult(identity_from_callback(std::stoull(prop->Value),&brush));Registered value(brush.Get(),api.diag.Get(),api.life.Get());auto actual=property_value(api,value.handle,L"Color");auto color=wide(expected);std::transform(actual.begin(),actual.end(),actual.begin(),towupper);std::transform(color.begin(),color.end(),color.begin(),towupper);matched=actual==color||actual==L"#FF"+color.substr(1);if(matched)break;}
            if(!matched&&st->error.empty())st->error="Actual tab background brush is not yet confirmed";
        }
    }catch(...){st->error="Visual property readback is pending";}

}
bool belongs_to_tab(const Element& element) {
    auto current=&element;
    for(int i=0;i<32;++i) {
        if(current->type==L"Microsoft.UI.Xaml.Controls.TabViewItem")return true;
        auto found=std::find_if(elements.begin(),elements.end(),[&](auto& e){return e.id==current->parent;});
        if(found==elements.end())return false;current=&*found;
    }
    return false;
}
Json inspect() {
    Api api;Json result=Json::array();if(!api.tree)return result;
    for(const auto& element:elements) {
        if(!belongs_to_tab(element)&&!element.tab_view)continue;
        auto object=resolve(element);if(!object)continue;
        Registered instance(object.Get(),api.diag.Get(),api.life.Get());Chain chain(api.tree.Get(),instance.handle);
        Json item{{"kind",utf8(element.type)},{"name",utf8(element.name)},{"handle",element.id},{"parent",element.parent}};
        for(const auto* name:{L"Header",L"Tag",L"DataContext",L"IconSource",L"Data",L"Source",L"IsSelected",L"Text",L"FontFamily",L"FontWeight",L"Foreground",L"Background",L"ActualTheme",L"ActualWidth",L"Resources",L"Fill",L"IsPointerOver",L"AutomationId"})if(auto v=chain.find(name)) {
            item[utf8(name)]={{"value",v->Value?utf8(v->Value):""},{"type",v->Type?utf8(v->Type):""},{"value_type",v->ValueType?utf8(v->ValueType):""},{"flags",v->MetadataBits},{"index",v->Index}};
        }
        for(auto name:{L"Foreground",L"Background",L"Fill"})if(auto prop=chain.find(name))try {
            if(prop->Value&&wcscmp(prop->Value,L"0")!=0) {
                ComPtr<IInspectable> brush;auto raw=std::stoull(prop->Value);if(SUCCEEDED(identity_from_callback(raw,&brush))) {
                    Registered registered(brush.Get(),api.diag.Get(),api.life.Get());item[utf8(name)+"Color"]=utf8(property_value(api,registered.handle,L"Color"));
                }
            }
        }catch(...){}
        if(element.type==L"Microsoft.UI.Xaml.Controls.TabViewItem")try {item["native_guid"]=guid_text(element_guid(api,instance.handle));}catch(...){item["identity_error"]=true;}
        result.push_back(std::move(item));
    }
    return result;
}
void CALLBACK lease_timer(HWND,UINT,UINT_PTR id,DWORD) {
    try {
        std::erase_if(styles,[](auto& s){return s.expires<=GetTickCount64();});refresh(false);
        if(styles.empty()&&std::none_of(elements.begin(),elements.end(),[](auto& e){return !e.saved.empty()||!e.resources.empty()||!e.attached.empty();})){KillTimer(nullptr,id);timer=0;}
    }catch(...) {last_error="Style refresh failed";}
}
class Watcher final : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IVisualTreeServiceCallback2> {
public:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation,VisualElement element,VisualMutationType mutation) override {
        Api api;
        const char* stage="callback";
        try {
            if(mutation==Remove) {
                retired.insert(element.Handle);
            } else if(element.Type) {
                stage="GetIInspectableFromHandle";ComPtr<IInspectable> object;
                auto object_hr=api.diag->GetIInspectableFromHandle(element.Handle,&object);
                if(FAILED(object_hr))object_hr=identity_from_callback(element.Handle,&object);
                winrt::check_hresult(object_hr);
                HSTRING runtime_name{};winrt::check_hresult(object->GetRuntimeClassName(&runtime_name));
                std::wstring class_name(WindowsGetStringRawBuffer(runtime_name,nullptr));WindowsDeleteString(runtime_name);
                if(class_name!=element.Type)winrt::throw_hresult(E_NOINTERFACE);
                stage="IWeakReferenceSource";ComPtr<IWeakReferenceSource> weak_source;winrt::check_hresult(object.As(&weak_source));
                Element record;record.id=element.Handle;record.parent=relation.Parent;record.type=element.Type;record.name=element.Name?element.Name:L"";record.tab_view=wcscmp(element.Type,L"Microsoft.UI.Xaml.Controls.TabView")==0;
                stage="GetWeakReference";winrt::check_hresult(weak_source->GetWeakReference(&record.weak));
                auto found=std::find_if(elements.begin(),elements.end(),[&](const auto& e){return e.id==element.Handle;});
                if(found==elements.end())elements.push_back(std::move(record));else if(retired.erase(element.Handle))*found=std::move(record);
                stage="defer refresh";
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
Json visual_order(const NativeTab& tab) {
    Api api;Json expected=Json::array();for(auto& t:read_native_tabs(tab.window,tab.process_id,tab.process_started))expected.push_back(guid_text(t.guid));std::sort(expected.begin(),expected.end());
    for(auto& e:elements)if(e.tab_view)try {
        auto object=resolve(e);if(!object)continue;Registered instance(object.Get(),api.diag.Get(),api.life.Get());
        auto items=object_property(api,instance.handle,L"TabItems");if(!items)continue;
        winrt::Windows::Foundation::IInspectable value{nullptr};winrt::copy_from_abi(value,items.Get());
        auto vector=value.try_as<winrt::Windows::Foundation::Collections::IVector<winrt::Windows::Foundation::IInspectable>>();if(!vector)continue;
        Json order=Json::array();for(auto item:vector){auto inspectable=reinterpret_cast<IInspectable*>(winrt::get_abi(item));
            try{order.push_back(guid_text(context_guid(inspectable)));}catch(...){Registered h(inspectable,api.diag.Get(),api.life.Get());order.push_back(guid_text(element_guid(api,h.handle)));}
        }
        auto sorted=order;std::sort(sorted.begin(),sorted.end());if(sorted==expected)return order;
    }catch(...){}
    throw Error(5,"Explorer TabItems order is not available for this window");
}
void execute(StylePacket& packet) {
    verify_layout_sender(self,packet.sender_pid,packet.sender_started,packet.action==StyleAction::inspect);
    if(!native_inventory_supported())throw Error(3,"Unsupported ExplorerFrame");
    DWORD pid{};auto thread=GetWindowThreadProcessId(reinterpret_cast<HWND>(packet.window),&pid);
    if(pid!=GetCurrentProcessId()||thread!=GetCurrentThreadId()||bridge_process_started(GetCurrentProcess())!=packet.target_started)throw Error(4,"Style target identity changed");
    initialize();auto hr=initialization.load();
    if(hr==E_PENDING){auto text=Json{{"pending",true},{"initialization",hr},{"initializing",true},{"style_applied",false},{"icon_applied",false},{"error",""},{"message","Explorer appearance adapter is initializing"}}.dump();strcpy_s(packet.report,text.c_str());packet.result=E_PENDING;return;}
    if(FAILED(hr))throw Error(3,"XAML initialization: "+std::to_string(hr));
    verify_style_build();
    auto window=reinterpret_cast<HWND>(packet.window);auto tabs=read_native_tabs(window,pid,packet.target_started);
    auto tab=std::find_if(tabs.begin(),tabs.end(),[&](auto& t){return t.guid==packet.guid&&reinterpret_cast<uintptr_t>(t.browser_window)==packet.browser_window;});
    if(tab==tabs.end())throw Error(3,"Style target tab identity changed");
    if(packet.action==StyleAction::order){auto report=Json{{"order",visual_order(*tab)},{"order_confirmed",true},{"source","TabView.TabItems / native GUID bijection"}}.dump();strcpy_s(packet.report,report.c_str());packet.result=S_OK;return;}
    auto found=std::find_if(styles.begin(),styles.end(),[&](auto& st){return st.tab.guid==tab->guid;});
    if(found!=styles.end()&&found->expires>GetTickCount64()&&(found->owner!=packet.sender_pid||found->owner_started!=packet.sender_started))throw Error(4,"Style is owned by another agent");
    if(packet.action==StyleAction::restore){if(found!=styles.end())styles.erase(found);refresh(false);}
    if(packet.action==StyleAction::apply) {
        auto payload=Json::parse(packet.payload);auto next=payload.at("style");next.erase("rule_id");validate_style(next);next.erase("enabled");
        StyledTab state{*tab,next,packet.sender_pid,packet.sender_started,GetTickCount64()+10000};state.description=payload.value("description",std::string{});
        if(found==styles.end())styles.push_back(state);else *found=state;
        if(!timer){timer=SetTimer(nullptr,0,150,lease_timer);if(!timer)throw Error(3,"Cannot create style lease timer");}
        refresh(false);
    }
    Json report{{"error",""},{"initialization",hr},{"style_applied",false}};
    // Callback diagnostics can refer to a retired visual-tree handle from
    // another tab. Keep them visible without presenting a stale global error
    // as the result of this target's successfully read-back restoration.
    if(!last_error.empty())report["diagnostic_warning"]=last_error;
    if(packet.action==StyleAction::inspect){element_index.clear();for(auto& e:elements)element_index[e.id]=&e;report["elements"]=inspect();report["target_visual_tree"]=target_visual_tree(*tab);}
    auto current=std::find_if(styles.begin(),styles.end(),[&](auto& st){return st.tab.guid==tab->guid;});
    if(current!=styles.end()){report["style_applied"]=current->error.empty()&&(current->style.empty()||current->mapped);report["icon_applied"]=current->icon_applied;report["icon_verification"]="realized XAML PathIcon geometry, foreground and nonzero width";report["applied_elements"]=current->applied;report["error"]=current->error;}
    if(packet.action==StyleAction::restore) {
        bool clean=true;for(auto& e:elements)if(!e.saved.empty()||!e.resources.empty()||!e.attached.empty())try{auto parent=parent_tab(e);if(parent){auto o=resolve(*parent);if(o){Registered h(o.Get(),Api().diag.Get(),Api().life.Get());Api api;if(element_guid(api,h.handle)==tab->guid)clean=false;}}}catch(...){clean=false;}
        report["restoration_confirmed"]=clean;if(clean)report["error"]="";if(!clean)report["error"]="Some original XAML properties are still awaiting restoration";
    }
    auto text=report.dump();
    if(text.size()>=sizeof(packet.report))throw Error(2,"Visual report exceeds limit");
    strcpy_s(packet.report,text.c_str());packet.result=S_OK;
}
void process(StylePacket* packet) {
    try {
        if(packet->magic!=0x54545354||packet->version!=1||packet->size!=sizeof(StylePacket)||!memchr(packet->payload,0,sizeof(packet->payload)))throw Error(2,"无效布局报文");
        auto copy=std::make_unique<StylePacket>(*packet);execute(*copy);packet->result=copy->result;
        std::copy(std::begin(copy->report),std::end(copy->report),std::begin(packet->report));
    }catch(const Error& e){packet->result=HRESULT_FROM_WIN32(e.code==4?ERROR_ACCESS_DENIED:ERROR_NOT_SUPPORTED);auto report=Json{{"error",e.what()},{"operation",operation}}.dump();strcpy_s(packet->report,report.c_str());}
    catch(...){packet->result=winrt::to_hresult();auto text=Json{{"operation",operation},{"error",system_error("XAML",static_cast<DWORD>(packet->result))}}.dump();strcpy_s(packet->report,text.c_str());}
    InterlockedExchange(&packet->completed,1);
}
void protected_process(StylePacket* packet) {
    __try{process(packet);}__except(EXCEPTION_EXECUTE_HANDLER){packet->result=E_UNEXPECTED;InterlockedExchange(&packet->completed,1);}
}
}
STDAPI DllGetClassObject(REFCLSID clsid,REFIID iid,void** output) {
    if(!output)return E_POINTER;*output=nullptr;if(clsid!=sinan::style::tap_class)return CLASS_E_CLASSNOTAVAILABLE;
    auto factory=Microsoft::WRL::Make<sinan::style::Factory>();return factory?factory->QueryInterface(iid,output):E_OUTOFMEMORY;
}
STDAPI DllCanUnloadNow(){return S_FALSE;}
extern "C" __declspec(dllexport) LRESULT CALLBACK SinanStyleHook(int code,WPARAM wparam,LPARAM lparam) {
    using namespace sinan;
    if(code>=0) {
        auto message=reinterpret_cast<const CWPSTRUCT*>(lparam);
        if(message&&message->message==RegisterWindowMessageW(style_message)) {
            auto sender=static_cast<DWORD>(message->wParam);auto nonce=static_cast<uint64_t>(message->lParam);
            Handle mapping(OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,style_mapping(sender,nonce).c_str()));
            if(mapping) {
                auto packet=static_cast<StylePacket*>(MapViewOfFile(mapping.get(),FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(StylePacket)));
                if(packet) {
                    if(packet->nonce==nonce&&packet->sender_pid==sender&&packet->window==reinterpret_cast<uintptr_t>(message->hwnd))style::protected_process(packet);
                    UnmapViewOfFile(packet);
                }
            }
        }
    }
    return CallNextHookEx(nullptr,code,wparam,lparam);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH)sinan::style::self=module;return TRUE;
}
