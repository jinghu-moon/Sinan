#include "platform/shell_collection.hpp"
#include <sstream>

namespace sinan {
namespace {
constexpr size_t max_shell_windows = 10000;
std::string com_error(const char* action, HRESULT hr) {
    std::ostringstream out;out<<action<<" (HRESULT=0x"<<std::hex<<static_cast<uint32_t>(hr)<<")";return out.str();
}
struct Variant {
    VARIANT value{};
    ~Variant() { VariantClear(&value); }
};
}
ShellCollectionSnapshot read_shell_collection(IShellWindows& collection) {
    using Microsoft::WRL::ComPtr;
    ShellCollectionSnapshot result;
    auto fail=[&](const char* action,HRESULT hr) {result.complete=false;result.warnings.push_back(com_error(action,hr));};
    long before=0;
    auto hr=collection.get_Count(&before);
    if(hr!=S_OK) {fail("读取 Shell 集合长度失败",hr);return result;}
    if(before<0||before>static_cast<long>(max_shell_windows)) {fail("Shell 集合长度超出限制",E_INVALIDARG);return result;}
    result.registration_count=before;
    ComPtr<IUnknown> unknown;ComPtr<IEnumVARIANT> enumerator;
    hr=collection._NewEnum(&unknown);
    if(hr!=S_OK||!unknown) {fail("取得 Shell 枚举器失败",hr==S_OK?E_POINTER:hr);return result;}
    hr=unknown.As(&enumerator);
    if(FAILED(hr)) {fail("Shell 枚举器不支持 IEnumVARIANT",hr);return result;}
    for(size_t index=0;;++index) {
        Variant next;ULONG fetched=0;
        hr=enumerator->Next(1,&next.value,&fetched);
        if(hr==S_FALSE&&fetched==0) break; // Normal end of enumeration, not Win32 error 1.
        if(hr!=S_OK||fetched!=1) {fail("读取 Shell 枚举项失败",hr==S_OK?E_UNEXPECTED:hr);break;}
        if(index>=max_shell_windows) {fail("Shell 枚举项超出限制",E_INVALIDARG);break;}
        ComPtr<IDispatch> dispatch;
        if(next.value.vt==VT_DISPATCH&&next.value.pdispVal) dispatch=next.value.pdispVal;
        else if(next.value.vt==VT_UNKNOWN&&next.value.punkVal) hr=next.value.punkVal->QueryInterface(IID_PPV_ARGS(&dispatch));
        else hr=DISP_E_TYPEMISMATCH;
        if(!dispatch) {fail("Shell 枚举项不是窗口对象",FAILED(hr)?hr:E_NOINTERFACE);continue;}
        result.windows.push_back(std::move(dispatch));
    }
    long after=0;hr=collection.get_Count(&after);
    if(hr!=S_OK) fail("复核 Shell 集合长度失败",hr);
    else if(after!=before) {result.complete=false;result.warnings.push_back("Shell 集合在读取期间变化，保留上次未确认关闭的标签");}
    // Do not compare the number of objects with Count: vacant registrations are valid.
    return result;
}
}
