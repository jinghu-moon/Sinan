#include "platform/shell_collection.hpp"
#include <wrl/implements.h>
#include <iostream>

using namespace tabtail;
using namespace Microsoft::WRL;
namespace {
int checks=0;
void expect(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
template<class Interface> class Automation : public RuntimeClass<RuntimeClassFlags<ClassicCom>,Interface> {
public:
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT,LCID,ITypeInfo**) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR*,UINT,LCID,DISPID*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Invoke(DISPID,REFIID,LCID,WORD,DISPPARAMS*,VARIANT*,EXCEPINFO*,UINT*) override {return E_NOTIMPL;}
};
class Object final : public Automation<IDispatch> {};
struct Scenario {
    long before=0,after=0;
    HRESULT count_hr=S_OK,enum_hr=S_OK;
    std::vector<ComPtr<IDispatch>> entries;
    int fail_at=-1;
    HRESULT next_hr=E_FAIL;
    bool unknown=false,invalid=false,bad_fetch=false,null_enum=false;
    int item_calls=0,count_calls=0,next_calls=0;
};
class Enumerator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,IEnumVARIANT> {
    Scenario& scenario_;size_t position_=0;
public:
    explicit Enumerator(Scenario& scenario):scenario_(scenario) {}
    HRESULT STDMETHODCALLTYPE Next(ULONG count,VARIANT* output,ULONG* fetched) override {
        ++scenario_.next_calls;
        if(count!=1||!output||!fetched)return E_INVALIDARG;
        VariantInit(output);*fetched=0;
        if(scenario_.fail_at==static_cast<int>(position_))return scenario_.next_hr;
        if(position_==scenario_.entries.size())return S_FALSE;
        auto& entry=scenario_.entries[position_++];
        output->vt=static_cast<VARTYPE>(scenario_.invalid?VT_I4:scenario_.unknown?VT_UNKNOWN:VT_DISPATCH);
        if(!scenario_.invalid) {output->pdispVal=entry.Get();if(entry)entry->AddRef();}
        *fetched=scenario_.bad_fetch?0:1;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Reset() override {position_=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT**) override {return E_NOTIMPL;}
};
class Collection final : public Automation<IShellWindows> {
public:
    Scenario scenario;
    HRESULT STDMETHODCALLTYPE get_Count(long* output) override {
        *output=scenario.count_calls++?scenario.after:scenario.before;return scenario.count_hr;
    }
    HRESULT STDMETHODCALLTYPE Item(VARIANT,IDispatch** output) override {
        ++scenario.item_calls;*output=nullptr;return S_FALSE; // Vacant indexed registration.
    }
    HRESULT STDMETHODCALLTYPE _NewEnum(IUnknown** output) override {
        *output=nullptr;
        if(scenario.enum_hr!=S_OK||scenario.null_enum)return scenario.enum_hr;
        auto enumerator=Make<Enumerator>(scenario);return enumerator.CopyTo(output);
    }
    HRESULT STDMETHODCALLTYPE Register(IDispatch*,long,int,long*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE RegisterPending(long,VARIANT*,VARIANT*,int,long*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Revoke(long) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE OnNavigate(long,VARIANT*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE OnActivated(long,VARIANT_BOOL) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE FindWindowSW(VARIANT*,VARIANT*,int,long*,int,IDispatch**) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE OnCreated(long,IUnknown*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE ProcessAttachDetach(VARIANT_BOOL) override {return E_NOTIMPL;}
};
ComPtr<Collection> make_collection(int objects,long slots) {
    auto collection=Make<Collection>();collection->scenario.before=slots;collection->scenario.after=slots;
    for(int i=0;i<objects;++i)collection->scenario.entries.push_back(Make<Object>());
    return collection;
}
}
int main() {
    try {
        auto sparse=make_collection(256,300);
        auto result=read_shell_collection(*sparse.Get());
        expect(result.complete&&result.warnings.empty(),"sparse indexed registrations are not incomplete scans");
        expect(result.windows.size()==256,"all pre-existing window objects discovered regardless of vacant slots");
        expect(sparse->scenario.item_calls==0&&sparse->scenario.next_calls==257,"enumerate valid objects instead of indexing Count slots");
        for(size_t i=0;i<result.windows.size();++i)expect(result.windows[i]==sparse->scenario.entries[i],"identity and ordering retained");
        auto empty=make_collection(0,2);result=read_shell_collection(*empty.Get());
        expect(result.complete&&result.windows.empty()&&result.warnings.empty(),"empty enumeration with registered vacant slots is valid");
        auto unknown=make_collection(2,2);unknown->scenario.unknown=true;
        expect(read_shell_collection(*unknown.Get()).windows.size()==2,"IUnknown variants resolve to dispatch");
        auto changed=make_collection(2,2);changed->scenario.after=3;
        result=read_shell_collection(*changed.Get());expect(!result.complete&&result.windows.size()==2,"collection mutation remains incomplete without dropping observations");
        auto failing=make_collection(3,3);failing->scenario.fail_at=1;
        result=read_shell_collection(*failing.Get());expect(!result.complete&&result.windows.size()==1,"COM failure keeps observed objects and reports partial result");
        expect(result.warnings[0].find("0x80004005")!=std::string::npos,"COM failure preserves HRESULT instead of Win32 error 1");
        auto malformed=make_collection(2,2);malformed->scenario.invalid=true;
        result=read_shell_collection(*malformed.Get());expect(!result.complete&&result.windows.empty(),"unexpected variant is not silently ignored");
        auto null_item=make_collection(1,1);null_item->scenario.entries[0].Reset();
        expect(!read_shell_collection(*null_item.Get()).complete,"null returned object is incomplete");
        auto bad_fetch=make_collection(1,1);bad_fetch->scenario.bad_fetch=true;
        expect(!read_shell_collection(*bad_fetch.Get()).complete,"S_OK with zero fetched objects is incomplete");
        auto null_enum=make_collection(0,0);null_enum->scenario.null_enum=true;
        expect(!read_shell_collection(*null_enum.Get()).complete,"missing enumerator is incomplete");
        auto unavailable=make_collection(0,0);unavailable->scenario.enum_hr=E_ACCESSDENIED;
        expect(!read_shell_collection(*unavailable.Get()).complete,"enumerator access failure remains visible");
        auto count_failure=make_collection(0,0);count_failure->scenario.count_hr=E_FAIL;
        expect(!read_shell_collection(*count_failure.Get()).complete,"count failure remains visible");
        auto over=make_collection(0,10001);
        expect(!read_shell_collection(*over.Get()).complete,"oversized count rejected");
        auto endless=make_collection(10001,0);
        result=read_shell_collection(*endless.Get());expect(!result.complete&&result.windows.size()==10000,"enumeration bounded independently of count");
        std::cout<<checks<<" Shell collection checks passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
