#include "platform/close_dialog.hpp"
#include "platform/windows.hpp"
#include <iostream>

using namespace tabtail;
namespace {
int checks=0,choice=IDCANCEL;bool chinese=true,session_end=false,called=false;HRESULT outcome=S_OK;
void expect(bool value,const char* description){++checks;if(!value)throw std::runtime_error(description);}
HRESULT WINAPI inspect_dialog(const TASKDIALOGCONFIG* config,int* button,int*,BOOL*) {
    called=true;
    // Exercise the production DLL manifest and v6 activation/loading path,
    // substituting only the modal display so the test never blocks for input.
    expect(config->nDefaultButton==IDCANCEL,"destructive action must not be the default");
    expect((config->dwFlags&TDF_ALLOW_DIALOG_CANCELLATION)!=0,"Escape and window close must cancel");
    expect(config->cButtons==2&&config->pButtons[0].nButtonID==IDCANCEL&&config->pButtons[1].nButtonID==IDYES,"explicit keep/close choices");
    expect(std::wstring(config->pszMainInstruction)==(chinese?L"关闭此资源管理器窗口？":L"Close this File Explorer window?"),"single-language prompt follows preference");
    expect(std::wstring(config->pButtons[0].pszButtonText)==(chinese?L"保留窗口":L"Keep window open"),"localized safe choice");
    expect(std::wstring(config->pButtons[1].pszButtonText)==(chinese?L"关闭整个窗口":L"Close entire window"),"localized explicit destructive choice");
    if(session_end)cancel_close_dialog_for_session_end(*reinterpret_cast<CloseDialogState*>(config->lpCallbackData));
    *button=choice;return outcome;
}
}
int main() {
    HMODULE module{};
    try {
        auto dll=executable_path().parent_path()/L"tabtail-features.dll";
        module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        expect(module!=nullptr,"load actual feature DLL without attaching Explorer");
        CloseDialogState state;
        expect(!confirm_window_close(nullptr,module,"zh-CN",state,inspect_dialog)&&called,"cancel preserves window through actual activation context");
        chinese=false;choice=IDYES;expect(confirm_window_close(nullptr,module,"en",state,inspect_dialog),"explicit close is accepted");
        outcome=E_FAIL;expect(!confirm_window_close(nullptr,module,"en",state,inspect_dialog),"failed display cannot allow closing");
        outcome=S_OK;session_end=true;expect(!confirm_window_close(nullptr,module,"en",state,inspect_dialog)&&state.ending_session,"session ending dismisses prompt without explicit close approval");
        called=false;expect(!confirm_window_close(nullptr,GetModuleHandleW(nullptr),"en",state,inspect_dialog)&&!called,"missing DLL activation resource keeps window open");
        FreeLibrary(module);std::cout<<checks<<" close dialog checks passed\n";return 0;
    }catch(const std::exception& ex){if(module)FreeLibrary(module);std::cerr<<"FAIL: "<<ex.what()<<'\n';return 1;}
}
