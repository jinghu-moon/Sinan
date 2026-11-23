#include "agent/tray.hpp"
#include "ui/app_icon.h"
#include <shellapi.h>
#include <windowsx.h>
#include <utility>

namespace sinan {
namespace {
constexpr wchar_t tray_class[]=L"Sinan.Tray.1";
constexpr UINT tray_callback=WM_APP+1;
constexpr UINT icon_id=1;

NOTIFYICONDATAW icon_data(HWND window) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd=window;
    data.uID=icon_id;
    return data;
}

const wchar_t* label(bool chinese,const wchar_t* zh,const wchar_t* en) {
    return chinese?zh:en;
}

std::wstring tooltip(const TrayState& state) {
    const auto zh=state.chinese;
    if(!state.customizing)return label(zh,L"司南 · 观察中",L"Sinan · Observing");
    if(!state.applied)return label(zh,L"司南 · 定制已启用，待确认",L"Sinan · Customization pending");
    return label(zh,L"司南 · 定制已启用",L"Sinan · Customization enabled");
}
}

TrayIcon::~TrayIcon(){close();}

void TrayIcon::close() {
    if(!window_)return;
    auto data=icon_data(window_);
    Shell_NotifyIconW(NIM_DELETE,&data);
    registered_=false;
    retry_registration_=false;
    next_menu_.reset();
    next_action_.reset();
    DestroyWindow(std::exchange(window_,nullptr));
}

void TrayIcon::add_icon() {
    auto data=icon_data(window_);
    data.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
    data.uCallbackMessage=tray_callback;
    data.hIcon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_SINAN_APP),
        IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
    if(!data.hIcon)data.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    lstrcpynW(data.szTip,tooltip_.c_str(),_countof(data.szTip));
    registered_=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE;
    retry_registration_=!registered_;
    if(!registered_){
        error_="Unable to add the notification area icon";
        return;
    }
    data.uVersion=NOTIFYICON_VERSION_4;
    version4_=Shell_NotifyIconW(NIM_SETVERSION,&data)!=FALSE;
    error_.clear();
}

void TrayIcon::update(bool enabled,TrayState state) {
    if(!enabled){
        close();
        error_.clear();
        return;
    }
    state_=state;
    auto tip=tooltip(state_);
    const bool changed=tip!=tooltip_;
    tooltip_=std::move(tip);
    if(!window_){
        const auto module=GetModuleHandleW(nullptr);
        taskbar_created_=RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSW cls{};
        cls.lpfnWndProc=procedure;
        cls.hInstance=module;
        cls.lpszClassName=tray_class;
        RegisterClassW(&cls);
        window_=CreateWindowExW(WS_EX_TOOLWINDOW,tray_class,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,module,this);
        if(!window_){
            error_=system_error("Create tray window");
            return;
        }
        if(taskbar_created_)ChangeWindowMessageFilterEx(window_,taskbar_created_,MSGFLT_ALLOW,nullptr);
        ChangeWindowMessageFilterEx(window_,tray_callback,MSGFLT_ALLOW,nullptr);
        retry_registration_=true;
    }
    if(retry_registration_)add_icon();
    else if(changed&&registered_){
        auto data=icon_data(window_);
        data.uFlags=NIF_TIP|NIF_SHOWTIP;
        lstrcpynW(data.szTip,tooltip_.c_str(),_countof(data.szTip));
        if(!Shell_NotifyIconW(NIM_MODIFY,&data)){
            retry_registration_=true;
            registered_=false;
            error_="Unable to update the notification area icon";
        }
    }
}

void TrayIcon::menu(POINT point) {
    if(menu_open_||next_action_)return;
    auto menu=CreatePopupMenu();
    if(!menu)return;
    const auto zh=state_.chinese;
    const auto append=[&](TrayAction action,const wchar_t* title,UINT flags=MF_ENABLED){
        AppendMenuW(menu,MF_STRING|flags,static_cast<UINT>(action),title);
    };
    AppendMenuW(menu,MF_STRING|MF_GRAYED,0,tooltip_.c_str());
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    if(state_.tui_available){
        append(TrayAction::open_tui,label(zh,L"打开 TUI",L"Open TUI"));
        SetMenuDefaultItem(menu,static_cast<UINT>(TrayAction::open_tui),FALSE);
    }
    const auto customization=state_.customizing?label(zh,L"停止标签定制",L"Stop tab customization"):
        label(zh,L"启用标签定制",L"Enable tab customization");
    append(TrayAction::toggle_customization,customization,state_.customizing||state_.customization_available?MF_ENABLED:MF_GRAYED);
    append(TrayAction::toggle_startup,label(zh,L"登录自启",L"Start at sign-in"),state_.autostart?MF_CHECKED:MF_UNCHECKED);
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    append(TrayAction::exit,label(zh,L"退出司南",L"Exit Sinan"));
    menu_open_=true;
    SetForegroundWindow(window_);
    const auto action=TrackPopupMenuEx(menu,TPM_RIGHTBUTTON|TPM_RETURNCMD|TPM_NONOTIFY,
        point.x,point.y,window_,nullptr);
    PostMessageW(window_,WM_NULL,0,0);
    menu_open_=false;
    DestroyMenu(menu);
    auto data=icon_data(window_);
    Shell_NotifyIconW(NIM_SETFOCUS,&data);
    if(action)next_action_=static_cast<TrayAction>(action);
}

LRESULT CALLBACK TrayIcon::procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_NCCREATE){
        const auto create=reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    const auto self=reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(self&&message==self->taskbar_created_&&message!=0){
        // A shell restart drops icon registrations. Keep the same window and icon ID.
        auto data=icon_data(window);
        Shell_NotifyIconW(NIM_DELETE,&data);
        self->add_icon();
        return 0;
    }
    if(self&&message==tray_callback){
        if(!self->registered_||(self->version4_?HIWORD(lparam)!=icon_id:wparam!=icon_id))return 0;
        const auto event=LOWORD(lparam);
        if(event==WM_CONTEXTMENU||(!self->version4_&&event==WM_RBUTTONUP)){
            POINT point{GET_X_LPARAM(wparam),GET_Y_LPARAM(wparam)};
            if(!self->version4_||(point.x==-1&&point.y==-1)){
                NOTIFYICONIDENTIFIER icon{sizeof(icon),window,icon_id};
                RECT bounds{};
                if(self->version4_&&SUCCEEDED(Shell_NotifyIconGetRect(&icon,&bounds)))point={bounds.left,bounds.bottom};
                else GetCursorPos(&point);
            }
            if(!self->menu_open_&&!self->next_action_)self->next_menu_=point;
        }else if((event==NIN_SELECT||event==NIN_KEYSELECT||(!self->version4_&&event==WM_LBUTTONUP))&&
            self->state_.tui_available&&!self->menu_open_&&!self->next_menu_&&!self->next_action_){
            self->next_action_=TrayAction::open_tui;
        }
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

std::optional<TrayAction> TrayIcon::take() {
    if(auto point=std::exchange(next_menu_,std::nullopt))menu(*point);
    return std::exchange(next_action_,std::nullopt);
}

Json TrayIcon::status() const {return {{"registered",registered_},{"error",error_}};}

void TrayIcon::report(std::wstring_view message,bool error) const {
    MessageBoxW(window_,std::wstring(message).c_str(),L"Sinan",MB_OK|MB_SETFOREGROUND|(error?MB_ICONWARNING:MB_ICONINFORMATION));
}

bool TrayIcon::confirm_exit() const {
    const auto text=label(state_.chinese,L"退出会取消正在进行和排队的解压任务。是否退出司南？",
        L"Exiting will cancel running and queued extraction jobs. Exit Sinan?");
    return MessageBoxW(window_,text,L"Sinan",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING|MB_SETFOREGROUND)==IDYES;
}
}
