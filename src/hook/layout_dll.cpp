#include "hook/layout.hpp"
#include "hook/everything_search.hpp"
#include "hook/identity.hpp"
#include "hook/style_abi.hpp"
#include "hook/xaml_visual_tree.hpp"
#include <xamlOM.h>
#include <windows.foundation.h>
#include <ocidl.h>
#include <weakreference.h>
#include <wrl.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>

// XAML diagnostics is used for Explorer layout. No XAML private vtable offsets
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
enum class ElementKind { tab_view, tab_item, search_box, address_grid };
struct SavedColumnSpan {
    ComPtr<IInspectable> property;
    ComPtr<IInspectable> original;
};
struct Element {
    InstanceHandle id = 0;
    ElementKind kind = ElementKind::tab_item;
    ComPtr<IWeakReference> weak;
    std::map<std::wstring,SavedProperty> saved;
    std::optional<SavedColumnSpan> column_span;
};
thread_local std::vector<Element> elements;
thread_local std::map<InstanceHandle,ComPtr<IWeakReference>> navigation_roots;
thread_local TabAppearance options;
thread_local bool hide_search_box = false;
thread_local DWORD owner = 0;
thread_local uint64_t owner_started = 0, expires = 0;
thread_local UINT_PTR timer = 0;
thread_local std::string last_error;
thread_local std::string operation;
thread_local bool geometry_changed = false;
thread_local size_t expanded_address_bars = 0;
thread_local std::vector<InstanceHandle> release_queue;
thread_local UINT_PTR release_timer = 0;
struct CommandBar {
    ComPtr<IWeakReference> element;
    bool secondary = false;
    std::array<ComPtr<IWeakReference>, 2> probes;
    ComPtr<IWeakReference> observed_commands;
    EventRegistrationToken vector_changed{};
    std::optional<int32_t> original_overflow;
    bool attached = true;
};
thread_local std::map<InstanceHandle, CommandBar> command_bars;
thread_local DWORD command_bar_owner = 0;
thread_local uint64_t command_bar_owner_started = 0, command_bar_expires = 0;
thread_local UINT_PTR command_bar_lease_timer = 0, command_bar_refresh_timer = 0;
thread_local bool probe_hide_more = false;
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
Element& track_element(InstanceHandle handle,ElementKind kind,IInspectable* object) {
    auto found=std::find_if(elements.begin(),elements.end(),[&](const auto& element){return element.id==handle;});
    if(found!=elements.end())return *found;
    ComPtr<IWeakReferenceSource> source;
    winrt::check_hresult(object->QueryInterface(IID_PPV_ARGS(&source)));
    Element record;
    record.id=handle;
    record.kind=kind;
    winrt::check_hresult(source->GetWeakReference(&record.weak));
    elements.push_back(std::move(record));
    return elements.back();
}
template<class Interface>
ComPtr<Interface> xaml_factory(const wchar_t* class_name) {
    const winrt::hstring name(class_name);
    ComPtr<Interface> factory;
    RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)),IID_PPV_ARGS(&factory));
    return factory;
}
bool has_type(IInspectable* object,std::wstring_view expected) {
    winrt::hstring type;
    return SUCCEEDED(object->GetRuntimeClassName(reinterpret_cast<HSTRING*>(winrt::put_abi(type))))&&
        std::wstring_view(type)==expected;
}
bool has_name(IInspectable* object,std::wstring_view expected) {
    ComPtr<style::IElementResources> element;
    winrt::hstring name;
    return SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&element)))&&
        SUCCEEDED(element->get_Name(winrt::put_abi(name)))&&std::wstring_view(name)==expected;
}

ComPtr<IXamlCommandVector> primary_commands(IInspectable* element) {
    ComPtr<IXamlCommandBar> bar;
    ComPtr<IInspectable> commands;
    ComPtr<IXamlCommandVector> vector;
    winrt::check_hresult(element->QueryInterface(IID_PPV_ARGS(&bar)));
    winrt::check_hresult(bar->get_PrimaryCommands(&commands));
    winrt::check_hresult(commands.As(&vector));
    return vector;
}

ComPtr<IWeakReference> weak_reference(IInspectable* element) {
    ComPtr<IWeakReferenceSource> source;
    ComPtr<IWeakReference> result;
    winrt::check_hresult(element->QueryInterface(IID_PPV_ARGS(&source)));
    winrt::check_hresult(source->GetWeakReference(&result));
    return result;
}

ComPtr<IInspectable> resolve_weak(const ComPtr<IWeakReference>& weak) {
    ComPtr<IInspectable> element;
    if (weak) winrt::check_hresult(weak->Resolve(__uuidof(IInspectable), &element));
    return element;
}

void queue_command_bar_refresh();

class CommandsChanged final : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IXamlCommandsChanged> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IInspectable*) override {
        queue_command_bar_refresh();
        return S_OK;
    }
};

void observe_commands(CommandBar& record, IXamlCommandVector* commands) {
    auto previous=resolve_weak(record.observed_commands);
    ComPtr<IInspectable> current;
    if(commands) winrt::check_hresult(commands->QueryInterface(IID_PPV_ARGS(&current)));
    if(previous.Get()==current.Get())return;
    if(previous) {
        ComPtr<IXamlObservableCommands> observable;
        winrt::check_hresult(previous.As(&observable));
        winrt::check_hresult(observable->remove_VectorChanged(record.vector_changed));
    }
    record.observed_commands.Reset();
    if(!current)return;
    ComPtr<IXamlObservableCommands> observable;
    winrt::check_hresult(current.As(&observable));
    auto weak=weak_reference(current.Get());
    auto handler=Microsoft::WRL::Make<CommandsChanged>();
    if(!handler)winrt::throw_hresult(E_OUTOFMEMORY);
    winrt::check_hresult(observable->add_VectorChanged(handler.Get(),&record.vector_changed));
    record.observed_commands=std::move(weak);
}

ComPtr<IInspectable> new_probe_button(bool secondary, size_t slot) {
    auto reader = xaml_factory<style::IXamlReader>(L"Microsoft.UI.Xaml.Markup.XamlReader");
    if (!reader) winrt::throw_hresult(E_NOINTERFACE);
    const auto label=secondary?(slot?L"Sinan menu probe":L"Sinan terminal probe"):L"Sinan left probe";
    const winrt::hstring markup(std::wstring(L"<AppBarButton xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Label=\"")+
        label+L"\" Icon=\"Setting\" />");
    ComPtr<IInspectable> result;
    winrt::check_hresult(reader->Load(static_cast<HSTRING>(winrt::get_abi(markup)), &result));
    return result;
}

Json refresh_command_bars(std::optional<bool> enabled, bool measure = false) {
    Json result = Json::array();
    for (auto& [handle, record] : command_bars) {
        auto bar = resolve_weak(record.element);
        auto apply=enabled;
        if (apply && !record.attached) apply=false;
        if (apply==false || !bar) observe_commands(record,nullptr);
        if (!bar) {
            record.attached=false;
            continue;
        }
        auto commands = primary_commands(bar.Get());
        if (apply==true) observe_commands(record,commands.Get());
        ComPtr<IXamlCommandBar> control;
        int32_t more_visibility=0;
        if (!record.secondary) {
            winrt::check_hresult(bar.As(&control));
            winrt::check_hresult(control->get_OverflowButtonVisibility(&more_visibility));
            if (apply==true && probe_hide_more) {
                if (!record.original_overflow)record.original_overflow=more_visibility;
                if (more_visibility!=2)winrt::check_hresult(control->put_OverflowButtonVisibility(2));
            } else if (apply && record.original_overflow) {
                winrt::check_hresult(control->put_OverflowButtonVisibility(*record.original_overflow));
                record.original_overflow.reset();
            }
            winrt::check_hresult(control->get_OverflowButtonVisibility(&more_visibility));
        }
        size_t probes=0, overflow=0;
        for (size_t slot=0;slot<(record.secondary?2u:1u);++slot) {
            auto& weak=record.probes[slot];
            auto element=resolve_weak(weak);
            ComPtr<IXamlCommandBarElement> probe;
            uint32_t index=0;
            bool present=false;
            if(element) {
                winrt::check_hresult(element.As(&probe));
                winrt::check_hresult(commands->IndexOf(probe.Get(),&index,&present));
            }
            if(apply==true&&!present) {
                element=new_probe_button(record.secondary,slot);
                winrt::check_hresult(element.As(&probe));
                weak=weak_reference(element.Get());
                winrt::check_hresult(commands->Append(probe.Get()));
                winrt::check_hresult(commands->IndexOf(probe.Get(),&index,&present));
            } else if(apply==false&&present) {
                winrt::check_hresult(commands->RemoveAt(index));
                winrt::check_hresult(commands->IndexOf(probe.Get(),&index,&present));
            }
            if(apply==false&&!present)weak.Reset();
            if(!measure||!present)continue;
            ++probes;
            bool in_overflow=false;
            winrt::check_hresult(probe->get_IsInOverflow(&in_overflow));
            if(in_overflow)++overflow;
        }
        if (!measure || !record.attached) continue;
        uint32_t count = 0;
        winrt::check_hresult(commands->get_Size(&count));
        result.push_back({{"bar", record.secondary ? "right" : "left"}, {"commands", count},
                          {"probes", probes}, {"overflow", overflow}, {"more_visibility", more_visibility}});
    }
    if(enabled)std::erase_if(command_bars,[](const auto& item){return !item.second.attached;});
    return result;
}

void CALLBACK refresh_command_bar_timer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    command_bar_refresh_timer = 0;
    if (command_bar_expires <= GetTickCount64()) return;
    try { refresh_command_bars(true); } catch (...) {}
}

void CALLBACK restore_command_bar_timer(HWND, UINT, UINT_PTR id, DWORD) {
    if (command_bar_expires > GetTickCount64()) return;
    try { refresh_command_bars(false); } catch (...) { return; }
    KillTimer(nullptr, id);
    command_bar_lease_timer = 0;
    command_bar_owner = 0;
    command_bar_owner_started = command_bar_expires = 0;
}

void queue_command_bar_refresh() {
    if (command_bar_expires > GetTickCount64() && !command_bar_refresh_timer)
        command_bar_refresh_timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, refresh_command_bar_timer);
}
void discover_search_box(Api& api) {
    erase_if(elements,[](const auto& element){return element.kind==ElementKind::search_box&&!resolve(element);});
    if(std::any_of(elements.begin(),elements.end(),[](const auto& element){return element.kind==ElementKind::search_box;}))return;
    auto helper=xaml_factory<ITreeHelper>(L"Microsoft.UI.Xaml.Media.VisualTreeHelper");
    if(!helper)return;
    std::vector<ComPtr<IInspectable>> roots;
    for(const auto& [id,weak]:navigation_roots) {
        ComPtr<IInspectable> root;
        if(SUCCEEDED(weak->Resolve(__uuidof(IInspectable),&root))&&root)roots.push_back(std::move(root));
    }
    for(const auto& element:elements) {
        if(element.kind!=ElementKind::tab_view)continue;
        auto root=resolve(element);
        for(int depth=0;root&&depth<40;++depth) {
            ComPtr<IInspectable> dependency,parent;
            if(FAILED(root->QueryInterface(xaml_dependency_object,&dependency))||
                FAILED(helper->GetParent(dependency.Get(),&parent))||!parent)break;
            root=std::move(parent);
        }
        if(root&&std::none_of(roots.begin(),roots.end(),[&](const auto& existing){return existing.Get()==root.Get();}))
            roots.push_back(std::move(root));
    }
    size_t visited=0;
    auto visit=[&](auto& self,IInspectable* object,int depth)->void {
        if(++visited>4096||depth>40)return;
        if(has_type(object,L"Microsoft.UI.Xaml.Controls.AutoSuggestBox")) {
            if(has_name(object,L"FileExplorerSearchBox")) {
                Registered instance(object,api.diag.Get(),api.life.Get());
                track_element(instance.handle,ElementKind::search_box,object);
            }
            return;
        }
        ComPtr<IInspectable> dependency;
        int32_t count=0;
        if(FAILED(object->QueryInterface(xaml_dependency_object,&dependency))||
            FAILED(helper->GetChildrenCount(dependency.Get(),&count)))return;
        for(int32_t i=0;i<count&&visited<4096;++i) {
            ComPtr<IInspectable> child;
            if(SUCCEEDED(helper->GetChild(dependency.Get(),i,&child))&&child)self(self,child.Get(),depth+1);
        }
    };
    for(const auto& root:roots)visit(visit,root.Get(),0);
}
void restore_address_bar(Element& element,IInspectable* address) {
    if(!element.column_span)return;
    ComPtr<style::IDependencyValues> values;
    winrt::check_hresult(address->QueryInterface(IID_PPV_ARGS(&values)));
    auto& [property,original]=*element.column_span;
    winrt::check_hresult(original?values->SetValue(property.Get(),original.Get()):values->ClearValue(property.Get()));
    element.column_span.reset();
    geometry_changed=true;
}
bool expand_address_bar(Api& api,ITreeHelper& tree,IGridStatics& grid,IInspectable* search) {
    ComPtr<IInspectable> search_dependency,parent,parent_dependency;
    if(FAILED(search->QueryInterface(xaml_dependency_object,&search_dependency))||
        FAILED(tree.GetParent(search_dependency.Get(),&parent))||!parent||
        !has_type(parent.Get(),L"Microsoft.UI.Xaml.Controls.Grid")||
        FAILED(parent->QueryInterface(xaml_dependency_object,&parent_dependency)))return false;
    int32_t children=0;
    if(FAILED(tree.GetChildrenCount(parent_dependency.Get(),&children))||children>32)return false;
    ComPtr<IInspectable> address;
    for(int i=0;i<children;++i) {
        ComPtr<IInspectable> child;
        if(FAILED(tree.GetChild(parent_dependency.Get(),i,&child))||!child||
            !has_type(child.Get(),L"Microsoft.UI.Xaml.Controls.Grid"))continue;
        if(has_name(child.Get(),L"FileExplorerAddressBarGrid")) {
            if(address)return false;
            address=std::move(child);
        }
    }
    if(!address)return false;
    ComPtr<style::IElementResources> address_element,search_element;
    if(FAILED(address.As(&address_element))||FAILED(search_dependency.As(&search_element)))return false;
    ComPtr<style::IDependencyValues> values;
    if(FAILED(address.As(&values)))return false;
    int32_t address_column=0,search_column=0,address_span=0,search_span=0,address_row=0,search_row=0;
    if(FAILED(grid.GetColumn(address_element.Get(),&address_column))||FAILED(grid.GetColumn(search_element.Get(),&search_column))||
        FAILED(grid.GetColumnSpan(address_element.Get(),&address_span))||FAILED(grid.GetColumnSpan(search_element.Get(),&search_span))||
        FAILED(grid.GetRow(address_element.Get(),&address_row))||FAILED(grid.GetRow(search_element.Get(),&search_row)))return false;
    if(address_row!=search_row||address_column<0||search_column<=address_column||search_column>32||
        search_span<1||search_span>32)return false;
    const int expanded_span=search_column-address_column+search_span;
    if(address_span==expanded_span)return true;
    if(address_span!=search_column-address_column)return false;

    Registered instance(address.Get(),api.diag.Get(),api.life.Get());
    auto& tracked=track_element(instance.handle,ElementKind::address_grid,address.Get());
    if(!tracked.column_span) {
        SavedColumnSpan saved;
        winrt::check_hresult(grid.get_ColumnSpanProperty(&saved.property));
        winrt::check_hresult(values->ReadLocalValue(saved.property.Get(),reinterpret_cast<void**>(saved.original.GetAddressOf())));
        auto unset_factory=xaml_factory<style::IUnsetValue>(L"Microsoft.UI.Xaml.DependencyProperty");
        if(!unset_factory)return false;
        ComPtr<IUnknown> unset_identity,original_identity;
        ComPtr<IInspectable> unset;
        winrt::check_hresult(unset_factory->get_UnsetValue(reinterpret_cast<void**>(unset.GetAddressOf())));
        winrt::check_hresult(unset.As(&unset_identity));
        if(saved.original)winrt::check_hresult(saved.original.As(&original_identity));
        if(original_identity.Get()==unset_identity.Get())saved.original.Reset();
        // A boxed integer can be restored directly. Leave bindings in place.
        if(saved.original) {
            ComPtr<ABI::Windows::Foundation::IPropertyValue> value;
            if(FAILED(saved.original.As(&value)))return false;
            int32_t original_span=0;
            if(FAILED(value->GetInt32(&original_span)))return false;
        }
        tracked.column_span=std::move(saved);
    }
    winrt::check_hresult(grid.SetColumnSpan(address_element.Get(),expanded_span));
    geometry_changed=true;
    return SUCCEEDED(grid.GetColumnSpan(address_element.Get(),&address_span))&&address_span==expanded_span;
}
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
    if(element.kind==ElementKind::tab_item && !element.saved.empty()) {
        set_property(api,handle,L"MinWidth",L"Double",L"0");
        set_property(api,handle,L"MaxWidth",L"Double",L"Infinity");
    }
    for(const auto* name:{L"Width",L"MaxWidth",L"MinWidth",L"TabWidthMode",L"Visibility"}) {
        auto it=element.saved.find(name);if(it==element.saved.end())continue;
        const auto& p=it->second;
        if(p.local) {Registered original(p.value.Get(),api.diag.Get(),api.life.Get());winrt::check_hresult(api.tree->SetProperty(handle,original.handle,p.index));}
        else winrt::check_hresult(api.tree->ClearProperty(handle,p.index));
    }
    element.saved.clear();
}
bool collapsed_visibility(std::wstring_view value) {
    return value==L"Collapsed"||value==L"1";
}
void apply_element(Element& element, Api& api, InstanceHandle handle) {
    if(element.kind==ElementKind::search_box) {
        Chain chain(api.tree.Get(),handle);
        const auto* visibility=chain.find(L"Visibility");
        if(visibility&&visibility->Value&&collapsed_visibility(visibility->Value))return;
        save_property(element,api,handle,L"Visibility");
        set_property(api,handle,L"Visibility",L"Microsoft.UI.Xaml.Visibility",L"Collapsed");
        return;
    }
    if(element.kind==ElementKind::tab_view) {
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
void refresh() {
    Api api;if(!api.tree)return;
    if(hide_search_box)discover_search_box(api);
    const bool restore_width=options.width_mode=="system";
    const auto first=restore_width?ElementKind::tab_item:ElementKind::tab_view;
    const auto second=restore_width?ElementKind::tab_view:ElementKind::tab_item;
    for(auto kind:{first,second,ElementKind::search_box}) {
        const bool restore=kind==ElementKind::search_box?!hide_search_box:restore_width;
        for(auto it=elements.begin();it!=elements.end();) {
            if(it->kind!=kind||(restore&&it->saved.empty())){++it;continue;}
            auto object=resolve(*it);
            if(!object){it=elements.erase(it);continue;}
            Registered instance(object.Get(),api.diag.Get(),api.life.Get());
            if(restore)restore_element(*it,api,instance.handle);else apply_element(*it,api,instance.handle);
            ++it;
        }
    }
    expanded_address_bars=0;
    if(hide_search_box) {
        auto tree=xaml_factory<ITreeHelper>(L"Microsoft.UI.Xaml.Media.VisualTreeHelper");
        auto grid=xaml_factory<IGridStatics>(L"Microsoft.UI.Xaml.Controls.Grid");
        if(!tree||!grid)return;
        std::vector<ComPtr<IInspectable>> searches;
        for(const auto& element:elements)if(element.kind==ElementKind::search_box)
            if(auto object=resolve(element))searches.push_back(std::move(object));
        for(const auto& search:searches)if(expand_address_bar(api,*tree.Get(),*grid.Get(),search.Get()))++expanded_address_bars;
    }else {
        for(auto it=elements.begin();it!=elements.end();) {
            if(it->kind!=ElementKind::address_grid){++it;continue;}
            if(auto object=resolve(*it)) {
                restore_address_bar(*it,object.Get());
                ++it;
            }else it=elements.erase(it);
        }
    }
}
Json inspect() {
    Api api;Json result=Json::array();
    if(!api.tree)return result;
    for(const auto& element:elements) {
        auto object=resolve(element);if(!object)continue;
        Registered instance(object.Get(),api.diag.Get(),api.life.Get());Chain chain(api.tree.Get(),instance.handle);
        Json item{{"kind",element.kind==ElementKind::search_box?"FileExplorerSearchBox":
            element.kind==ElementKind::address_grid?"FileExplorerAddressBarGrid":
            element.kind==ElementKind::tab_view?"TabView":"TabViewItem"}};
        for(const auto* name:{L"Width",L"MinWidth",L"MaxWidth",L"ActualWidth",L"Visibility"}) {
            if(auto value=chain.find(name)) {
                item[utf8(name)]=value->Value?utf8(value->Value):"";
            }
        }
        if(element.kind==ElementKind::address_grid) {
            ComPtr<style::IElementResources> address;
            double width=0;
            if(SUCCEEDED(object.As(&address))&&SUCCEEDED(address->get_ActualWidth(&width)))
                item["ActualWidth"]=std::to_string(width);
        }
        result.push_back(std::move(item));
    }
    return result;
}
void CALLBACK lease_timer(HWND,UINT,UINT_PTR id,DWORD) {
    if(expires>GetTickCount64())return;
    options={};hide_search_box=false;
    try{refresh();last_error.clear();}catch(...){last_error="Layout restoration failed";return;}
    owner=0;owner_started=0;expires=0;KillTimer(nullptr,id);timer=0;
}
class Watcher final : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IVisualTreeServiceCallback2> {
public:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation,VisualElement element,VisualMutationType mutation) override {
        Api api;
        const char* stage="callback";
        try {
            if(mutation==Remove) {
                std::erase_if(elements,[&](const auto& e){return e.id==element.Handle;});
                navigation_roots.erase(element.Handle);
                if(auto found=command_bars.find(element.Handle);found!=command_bars.end()) {
                    if(found->second.observed_commands)found->second.attached=false;
                    else command_bars.erase(found);
                }
            } else if(element.Type) {
                const std::wstring_view type(element.Type);
                const bool search=type==L"Microsoft.UI.Xaml.Controls.AutoSuggestBox"&&element.Name&&
                    std::wstring_view(element.Name)==L"FileExplorerSearchBox";
                const bool root=!relation.Parent||type==L"FileExplorerExtensions.NavigationBarControl";
                const bool tab_view=type==L"Microsoft.UI.Xaml.Controls.TabView";
                const bool tab_item=type==L"Microsoft.UI.Xaml.Controls.TabViewItem";
                const std::wstring_view name=element.Name?element.Name:L"";
                const bool secondary=name==L"FileExplorerSecondaryCommandBar";
                const bool command_bar=type==L"Microsoft.UI.Xaml.Controls.CommandBar"&&
                    (secondary||name==L"FileExplorerCommandBar");
                if(search||tab_view||tab_item||root||command_bar) {
                    stage="GetIInspectableFromHandle";ComPtr<IInspectable> object;
                    auto object_hr=api.diag->GetIInspectableFromHandle(element.Handle,&object);
                    if(object_hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))object_hr=identity_from_callback(element.Handle,&object);
                    winrt::check_hresult(object_hr);
                    HSTRING runtime_name{};winrt::check_hresult(object->GetRuntimeClassName(&runtime_name));
                    std::wstring class_name(WindowsGetStringRawBuffer(runtime_name,nullptr));WindowsDeleteString(runtime_name);
                    if(class_name!=element.Type)winrt::throw_hresult(E_NOINTERFACE);
                    stage="GetWeakReference";
                    if(root) {
                        ComPtr<IWeakReferenceSource> weak_source;
                        if(SUCCEEDED(object.As(&weak_source)))
                            weak_source->GetWeakReference(navigation_roots[element.Handle].ReleaseAndGetAddressOf());
                        if(!navigation_roots[element.Handle])navigation_roots.erase(element.Handle);
                    }
                    if(search||tab_view||tab_item) {
                        const auto kind=search?ElementKind::search_box:tab_view?ElementKind::tab_view:ElementKind::tab_item;
                        track_element(element.Handle,kind,object.Get());
                    }
                    if(command_bar&&has_name(object.Get(),name)) {
                        auto& record=command_bars[element.Handle];
                        record.element=weak_reference(object.Get());
                        record.secondary=secondary;
                        record.attached=true;
                    }
                    stage="refresh";if((tab_view||tab_item)&&expires>GetTickCount64()&&options.width_mode!="system")refresh();
                }
            }
        } catch(...){last_error=std::string(stage)+": "+std::to_string(static_cast<int32_t>(winrt::to_hresult()));}
        queue_command_bar_refresh();
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
    verify_layout_sender(self,packet.sender_pid,packet.sender_started,
        packet.action==LayoutAction::inspect||packet.action==LayoutAction::command_bar_inspect);
    if(packet.mode&&!native_inventory_supported())throw Error(3,"当前 Explorer 版本尚无标签宽度适配");
    DWORD pid{};auto thread=GetWindowThreadProcessId(reinterpret_cast<HWND>(packet.window),&pid);
    if(pid!=GetCurrentProcessId()||thread!=GetCurrentThreadId()||bridge_process_started(GetCurrentProcess())!=packet.target_started)
        throw Error(4,"布局目标身份或 UI 线程已变化");
    if(packet.action==LayoutAction::command_bar_enable||packet.action==LayoutAction::command_bar_disable||
        packet.action==LayoutAction::command_bar_inspect) {
        initialize();
        const auto status=initialization.load();
        if(status!=S_OK) {
            packet.result=status;
            const auto report=Json{{"bars",Json::array()},{"message",initialization_stage.load()}}.dump();
            strcpy_s(packet.report,report.c_str());
            return;
        }
        std::optional<bool> change;
        if(packet.action!=LayoutAction::command_bar_inspect) {
            if(command_bar_expires>GetTickCount64() &&
                (command_bar_owner!=packet.sender_pid||command_bar_owner_started!=packet.sender_started))
                throw Error(4,"Explorer command bar is controlled by another agent");
            change=packet.action==LayoutAction::command_bar_enable;
            if(*change) {
                if(!native_inventory_supported())throw Error(3,"Explorer build does not support command bar inspection");
                const auto request=Json::parse(packet.report,nullptr,false);
                probe_hide_more=request.is_object()&&request.value("hide_more",false);
                if(!command_bar_lease_timer) {
                    command_bar_lease_timer=SetTimer(nullptr,0,1000,restore_command_bar_timer);
                    if(!command_bar_lease_timer)winrt::throw_last_error();
                }
                command_bar_owner=packet.sender_pid;
                command_bar_owner_started=packet.sender_started;
                command_bar_expires=GetTickCount64()+10000;
            } else {
                command_bar_expires=0;
                if(command_bar_refresh_timer)KillTimer(nullptr,std::exchange(command_bar_refresh_timer,0));
            }
        }
        operation="command bar";
        auto bars=refresh_command_bars(change,true);
        if(change==false&&command_bar_lease_timer)restore_command_bar_timer(nullptr,0,command_bar_lease_timer,0);
        const auto report=Json{{"bars",std::move(bars)},{"discovery_error",last_error}}.dump();
        if(report.size()>=sizeof(packet.report))throw Error(2,"Command bar report too large");
        strcpy_s(packet.report,report.c_str());
        packet.result=S_OK;
        return;
    }
    if(packet.action==LayoutAction::search_enable||packet.action==LayoutAction::search_disable) {
        Json report;
        try {
            const auto request = Json::parse(packet.report, nullptr, false);
            const auto all_drives = request.is_object() && request.value("all_drives", false);
            report=explorer_search::configure(self,packet.sender_pid,packet.sender_started,
                packet.action==LayoutAction::search_enable,all_drives);
        }catch(const Error& error){report={{"active",false},{"error",error.what()}};}
        const auto result=report.dump();
        strcpy_s(packet.report,result.c_str());packet.result=S_OK;return;
    }
    if(packet.action!=LayoutAction::apply&&packet.action!=LayoutAction::restore&&packet.action!=LayoutAction::inspect)throw Error(2,"未知布局操作");
    if(packet.mode>2||packet.search_box_hidden>1)throw Error(2,"未知布局选项");
    TabAppearance next{packet.mode==1?"fixed":packet.mode==2?"content":"system",packet.width,packet.min_width,packet.max_width};
    validate_appearance(next);
    initialize();
    auto hr=initialization.load();
    if(FAILED(hr)) {
        packet.result=hr;auto text=Json{{"layout_applied",false},{"message",initialization_stage.load()}}.dump();strcpy_s(packet.report,text.c_str());return;
    }
    if(packet.action!=LayoutAction::inspect) {
        geometry_changed=false;
        if(owner && expires>GetTickCount64() && (owner!=packet.sender_pid||owner_started!=packet.sender_started))throw Error(4,"Explorer 布局由其他代理控制");
        const bool restore=packet.action==LayoutAction::restore||(next.width_mode=="system"&&!packet.search_box_hidden);
        if(!restore) {
            if(!timer){timer=SetTimer(nullptr,0,1000,lease_timer);if(!timer)throw Error(1,"无法创建布局恢复计时器");}
            owner=packet.sender_pid;owner_started=packet.sender_started;expires=GetTickCount64()+10000;
        }
        options=restore?TabAppearance{}:next;
        hide_search_box=!restore&&packet.search_box_hidden;
        try {refresh();last_error.clear();}
        catch(...) {
            auto error=winrt::to_hresult();auto failed_operation=operation;
            options={};hide_search_box=false;try{refresh();}catch(...){}
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
    size_t search_boxes=0,hidden_search_boxes=0;
    for(const auto& item:observations)if(item.at("kind")=="FileExplorerSearchBox") {
        ++search_boxes;
        if(collapsed_visibility(wide(item.value("Visibility",std::string{}))))++hidden_search_boxes;
    }
    const bool search_hidden=search_boxes>0&&search_boxes==hidden_search_boxes;
    const bool tabs_confirmed=options.width_mode=="system"|| (count>0&&measured);
    Json report{{"layout_applied",tabs_confirmed&&(!hide_search_box||search_hidden)&&last_error.empty()&&!geometry_changed},
        {"tab_count",count},{"mode",options.width_mode},{"search_boxes",search_boxes},{"search_box_hidden",search_hidden},
        {"expanded_address_bars",expanded_address_bars},
        {"elements",observations},{"error",last_error},{"measurement","XAML ActualWidth (DIP)"}};
    auto text=report.dump();if(text.size()>=sizeof(packet.report))throw Error(2,"布局报告过大");
    strcpy_s(packet.report,text.c_str());packet.result=S_OK;geometry_changed=false;
}
void process(LayoutPacket* packet) {
    try {
        if(packet->magic!=0x54544c59||packet->version!=2||packet->size!=sizeof(LayoutPacket))throw Error(2,"无效布局报文");
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
