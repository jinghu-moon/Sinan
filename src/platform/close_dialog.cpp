#include "platform/close_dialog.hpp"
#include <commctrl.h>

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace sinan {
namespace {
HRESULT CALLBACK dialog_callback(HWND window,UINT message,WPARAM,LPARAM,LONG_PTR data) {
    auto& state=*reinterpret_cast<CloseDialogState*>(data);
    if(message==TDN_CREATED){state.window=window;if(state.ending_session)PostMessageW(window,TDM_CLICK_BUTTON,IDCANCEL,0);}
    if(message==TDN_DESTROYED)state.window=nullptr;
    return S_OK;
}
struct ActivationScope {
    HANDLE context=INVALID_HANDLE_VALUE;ULONG_PTR cookie{};
    ~ActivationScope(){if(cookie)DeactivateActCtx(0,cookie);if(context!=INVALID_HANDLE_VALUE)ReleaseActCtx(context);}
};
}
void cancel_close_dialog_for_session_end(CloseDialogState& state) {
    state.ending_session=true;
    if(state.window)PostMessageW(state.window,TDM_CLICK_BUTTON,IDCANCEL,0);
}
bool confirm_window_close(HWND owner,HMODULE module,std::string_view language,CloseDialogState& state,CloseDialogRunner runner) {
    state={};
    wchar_t path[32768]{};if(!GetModuleFileNameW(module,path,32768))return false;
    ACTCTXW descriptor{sizeof(descriptor)};
    descriptor.dwFlags=ACTCTX_FLAG_RESOURCE_NAME_VALID|ACTCTX_FLAG_HMODULE_VALID;
    descriptor.lpSource=path;descriptor.lpResourceName=MAKEINTRESOURCEW(2);descriptor.hModule=module;
    ActivationScope activation;activation.context=CreateActCtxW(&descriptor);
    if(activation.context==INVALID_HANDLE_VALUE||!ActivateActCtx(activation.context,&activation.cookie))return false;
    auto controls=LoadLibraryExW(L"comctl32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!controls)return false;
    auto show=reinterpret_cast<decltype(&TaskDialogIndirect)>(GetProcAddress(controls,"TaskDialogIndirect"));
    bool chinese=language=="zh-CN"||(language!="en"&&PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_CHINESE);
    TASKDIALOG_BUTTON buttons[]{
        {IDCANCEL,chinese?L"保留窗口":L"Keep window open"},
        {IDYES,chinese?L"关闭整个窗口":L"Close entire window"}
    };
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent=owner;config.hInstance=module;
    config.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW|TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle=L"Sinan";
    config.pszMainInstruction=chinese?L"关闭此资源管理器窗口？":L"Close this File Explorer window?";
    config.pszContent=chinese?L"此窗口包含固定或受保护的标签。关闭整个窗口将同时关闭其中的所有标签。":L"This window contains pinned or protected tabs. Closing the window will close all of its tabs.";
    config.cButtons=ARRAYSIZE(buttons);config.pButtons=buttons;config.nDefaultButton=IDCANCEL;
    config.pfCallback=dialog_callback;config.lpCallbackData=reinterpret_cast<LONG_PTR>(&state);
    int selected=IDCANCEL;auto result=show?(runner?runner:show)(&config,&selected,nullptr,nullptr):E_NOTIMPL;
    FreeLibrary(controls);state.window=nullptr;
    return SUCCEEDED(result)&&selected==IDYES&&!state.ending_session;
}
}
