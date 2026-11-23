#include "agent/folder_click.hpp"
#include <windowsx.h>
#include <cstdlib>
#include <utility>

namespace sinan {
namespace {
constexpr wchar_t window_class[]=L"Sinan.FolderClickListener.1";

bool has_class(HWND window,std::wstring_view expected) {
    wchar_t name[64]{};
    return GetClassNameW(window,name,64)>0&&name==expected;
}
bool within_click_rectangle(POINT a,POINT b,int width,int height) {
    return std::abs(a.x-b.x)<=width/2&&std::abs(a.y-b.y)<=height/2;
}
}
FolderClickListener::~FolderClickListener() { update(false); }

void FolderClickListener::update(bool enabled) {
    if (!enabled) {
        if (window_) {
            RAWINPUTDEVICE mouse{1,2,RIDEV_REMOVE,nullptr};
            RegisterRawInputDevices(&mouse,1,sizeof(mouse));
            DestroyWindow(window_);
            window_=nullptr;
        }
        down_.reset();
        previous_.reset();
        ready_.reset();
        automation_.Reset();
        error_.clear();
        return;
    }
    if (window_) return;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation_)))) {
        error_="UI Automation is unavailable";
        return;
    }
    const auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW host{};
    host.hInstance=instance;
    host.lpfnWndProc=procedure;
    host.lpszClassName=window_class;
    RegisterClassW(&host);
    window_=CreateWindowExW(0,window_class,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instance,this);
    if (!window_) {
        error_=system_error("Create mouse listener");
        return;
    }
    RAWINPUTDEVICE mouse{1,2,RIDEV_INPUTSINK,window_};
    if (!RegisterRawInputDevices(&mouse,1,sizeof(mouse))) {
        error_=system_error("Register raw mouse input");
        DestroyWindow(window_);
        window_=nullptr;
        return;
    }
    error_.clear();
}

LRESULT CALLBACK FolderClickListener::procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    if (message==WM_NCCREATE) {
        auto create=reinterpret_cast<const CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message==WM_INPUT)
        if (auto self=reinterpret_cast<FolderClickListener*>(GetWindowLongPtrW(window,GWLP_USERDATA)))
            self->input(reinterpret_cast<HRAWINPUT>(l));
    return DefWindowProcW(window,message,w,l);
}

void FolderClickListener::input(HRAWINPUT handle) {
    RAWINPUT raw{};
    UINT size=sizeof(raw);
    if (GetRawInputData(handle,RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))!=sizeof(RAWINPUT)||raw.header.dwType!=RIM_TYPEMOUSE)
        return;
    const auto buttons=raw.data.mouse.usButtonFlags;
    if (buttons&~(RI_MOUSE_LEFT_BUTTON_DOWN|RI_MOUSE_LEFT_BUTTON_UP)) {
        down_.reset();
        previous_.reset();
        return;
    }
    if (!buttons) return;
    const auto frame=GetForegroundWindow();
    const auto pos=GetMessagePos();
    const POINT point{GET_X_LPARAM(pos),GET_Y_LPARAM(pos)};
    const auto target=WindowFromPhysicalPoint(point);
    if (!has_class(frame,L"CabinetWClass")||!IsChild(frame,target)||
        !has_class(target,L"DirectUIHWND")||
        ((GetAsyncKeyState(VK_CONTROL)|GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_MENU))&0x8000)) {
        down_.reset();
        previous_.reset();
        return;
    }
    const auto time=static_cast<DWORD>(GetMessageTime());
    if (buttons&RI_MOUSE_LEFT_BUTTON_DOWN) down_=Click{frame,target,point,time};
    if (!(buttons&RI_MOUSE_LEFT_BUTTON_UP))return;
    const auto down=std::exchange(down_,std::nullopt);
    if (!down||down->target!=target||down->frame!=frame||
        time-down->time>GetDoubleClickTime()||
        !within_click_rectangle(point,down->point,GetSystemMetrics(SM_CXDRAG),GetSystemMetrics(SM_CYDRAG))) {
        previous_.reset();
        return;
    }
    if (previous_&&previous_->frame==frame&&previous_->target==target&&
        down->time-previous_->time<=GetDoubleClickTime()&&
        within_click_rectangle(point,previous_->point,GetSystemMetrics(SM_CXDOUBLECLK),GetSystemMetrics(SM_CYDOUBLECLK))) {
        ready_=FolderDoubleClick{frame,target,previous_->point,point,time};
        previous_.reset();
    } else {
        previous_=Click{frame,target,point,down->time};
    }
}

std::optional<FolderDoubleClick> FolderClickListener::take() {
    auto click=std::exchange(ready_,std::nullopt);
    if (click&&(GetForegroundWindow()!=click->frame||GetTickCount()-click->time>400))return {};
    return click;
}

bool FolderClickListener::empty_items_view(const FolderDoubleClick& click,IShellView& view) {
    HWND folder{};
    if (!automation_||FAILED(view.GetWindow(&folder))||(folder!=click.target&&!IsChild(folder,click.target)))return false;
    for (const auto point : {click.first,click.second}) {
        if (WindowFromPhysicalPoint(point)!=click.target)return false;
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        if (FAILED(automation_->ElementFromPoint(point,&element))||!element)return false;
        BSTR name{};
        const auto hr=element->get_CurrentClassName(&name);
        const bool background=SUCCEEDED(hr)&&name&&std::wstring_view(name,SysStringLen(name))==L"UIItemsView";
        SysFreeString(name);
        UIA_HWND window{};
        if (!background||FAILED(element->get_CurrentNativeWindowHandle(&window))||
            reinterpret_cast<HWND>(window)!=click.target)return false;
    }
    return GetForegroundWindow()==click.frame;
}

Json FolderClickListener::status() const { return {{"active",window_!=nullptr},{"error",error_}}; }
}
