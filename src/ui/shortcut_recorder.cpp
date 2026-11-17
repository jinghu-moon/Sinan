#include "ui/shortcut_recorder.hpp"
#include <utility>
namespace sinan {
namespace {constexpr UINT input_update=WM_APP+0x542;}
std::string shortcut_chord(UINT modifiers,UINT key) {
    if(!(modifiers&(MOD_CONTROL|MOD_ALT|MOD_WIN)))return {};
    std::string name;if((key>='A'&&key<='Z')||(key>='0'&&key<='9'))name=static_cast<char>(key);
    else if(key>=VK_F1&&key<=VK_F24)name="F"+std::to_string(key-VK_F1+1);
    else return {};
    std::string chord;
    if(modifiers&MOD_CONTROL)chord+="Ctrl+";if(modifiers&MOD_ALT)chord+="Alt+";
    if(modifiers&MOD_SHIFT)chord+="Shift+";if(modifiers&MOD_WIN)chord+="Win+";
    return chord+name;
}
thread_local ShortcutRecorder* ShortcutRecorder::current_=nullptr;
ShortcutRecorder::ShortcutRecorder(HWND terminal,std::function<void()> wake):terminal_(terminal),wake_(std::move(wake)),ready_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    if(!ready_){error_=system_error("Shortcut recorder event");return;}
    thread_=std::thread([this]{
        current_=this;MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);thread_id_=GetCurrentThreadId();
        SetEvent(ready_.get());
        while(GetMessageW(&message,nullptr,0,0)>0){if(message.message==input_update)update_hook();else {TranslateMessage(&message);DispatchMessageW(&message);}}
        if(hook_)UnhookWindowsHookEx(hook_);hooked_=false;current_=nullptr;
    });
    WaitForSingleObject(ready_.get(),INFINITE);
}
ShortcutRecorder::~ShortcutRecorder(){field_=0;if(thread_.joinable()){PostThreadMessageW(thread_id_,WM_QUIT,0,0);thread_.join();}}
void ShortcutRecorder::focus(int field){if(field_.exchange(field)!=field&&thread_id_)PostThreadMessageW(thread_id_,input_update,0,0);}
void ShortcutRecorder::update_hook() {
    if(field_.load()||consumed_.any()) {
        if(!hook_){hook_=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard,GetModuleHandleW(nullptr),0);std::lock_guard lock(mutex_);error_=hook_?"":system_error("Shortcut recorder hook");}
    }else if(hook_){UnhookWindowsHookEx(hook_);hook_=nullptr;}
    hooked_=hook_!=nullptr;
}
std::string ShortcutRecorder::error(){std::lock_guard lock(mutex_);return error_;}
std::deque<CapturedShortcut> ShortcutRecorder::take(){std::lock_guard lock(mutex_);return std::exchange(events_,{});}
LRESULT CALLBACK ShortcutRecorder::keyboard(int code,WPARAM message,LPARAM data) {
    auto self=current_;if(code!=HC_ACTION||!self)return CallNextHookEx(nullptr,code,message,data);
    auto key=reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
    if(key->vkCode>=256)return CallNextHookEx(nullptr,code,message,data);
    if(message==WM_KEYUP||message==WM_SYSKEYUP){if(self->consumed_.test(key->vkCode)){self->consumed_.reset(key->vkCode);if(!self->field_&&self->consumed_.none())PostThreadMessageW(self->thread_id_,input_update,0,0);return 1;}}
    else if((message==WM_KEYDOWN||message==WM_SYSKEYDOWN)&&!(key->flags&LLKHF_INJECTED)) {
        if(self->consumed_.test(key->vkCode))return 1;
        auto field=self->field_.load();
        if(field&&self->terminal_&&GetForegroundWindow()==self->terminal_) {
            UINT modifiers{};
            if(GetAsyncKeyState(VK_CONTROL)&0x8000)modifiers|=MOD_CONTROL;
            if(GetAsyncKeyState(VK_MENU)&0x8000)modifiers|=MOD_ALT;
            if(GetAsyncKeyState(VK_SHIFT)&0x8000)modifiers|=MOD_SHIFT;
            if((GetAsyncKeyState(VK_LWIN)|GetAsyncKeyState(VK_RWIN))&0x8000)modifiers|=MOD_WIN;
            auto chord=shortcut_chord(modifiers,key->vkCode);
            if(!chord.empty()) {
                std::unique_lock lock(self->mutex_,std::try_to_lock);
                if(!lock||self->events_.size()>=16)return CallNextHookEx(nullptr,code,message,data);
                self->consumed_.set(key->vkCode);self->events_.push_back({field,std::move(chord)});lock.unlock();
                self->wake_();return 1;
            }
        }
    }
    return CallNextHookEx(nullptr,code,message,data);
}
}
