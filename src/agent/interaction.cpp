#include "agent/interaction.hpp"
#include <shellapi.h>
#include <utility>
#include <algorithm>

namespace sinan {
namespace {
constexpr UINT input_update=WM_APP+0x542;
constexpr UINT shortcut_priority_interval_ms=1000;
}
DirectoryShortcuts::DirectoryShortcuts() {
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"Sinan.DirectoryShortcuts.1";RegisterClassW(&cls);
    window_=CreateWindowExW(0,cls.lpszClassName,L"Sinan",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,this);
}
DirectoryShortcuts::~DirectoryShortcuts(){for(auto& [id,entry]:ids_)UnregisterHotKey(window_,id);if(window_)DestroyWindow(window_);}
LRESULT CALLBACK DirectoryShortcuts::procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto self=reinterpret_cast<DirectoryShortcuts*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<DirectoryShortcuts*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(message==WM_HOTKEY&&self){auto it=self->ids_.find(static_cast<int>(w));if(it!=self->ids_.end()&&self->events_.size()<16)self->events_.push_back({it->second,GetForegroundWindow()});return 0;}
    return DefWindowProcW(window,message,w,l);
}
void DirectoryShortcuts::update(const Json& entries) {
    if(entries==config_&&(errors_.empty()||GetTickCount64()-retry_<5000))return;
    for(auto& [id,entry]:ids_)UnregisterHotKey(window_,id);
    // UnregisterHotKey leaves queued messages behind. Discard them before reusing IDs.
    if(window_) {
        MSG message{};
        while(PeekMessageW(&message,window_,WM_HOTKEY,WM_HOTKEY,PM_REMOVE)){}
    }
    ids_.clear();events_.clear();errors_=Json::object();config_=entries;retry_=GetTickCount64();int id=0x6800;
    for(auto& e:entries)if(e.value("enabled",true)) {
        auto chord=parse_shortcut(e.value("shortcut",std::string{}));if(!chord.key)continue;
        auto key=e.at("id").get<std::string>();++id;
        if(!window_||!RegisterHotKey(window_,id,chord.modifiers,chord.key))errors_[key]=system_error("Global directory shortcut is occupied");
    else ids_[id]=e;
    }
}
std::deque<DirectoryShortcutEvent> DirectoryShortcuts::take(){return std::exchange(events_,{});}
Json DirectoryShortcuts::status()const{return {{"available",window_!=nullptr},{"bound",ids_.size()},{"errors",errors_},{"scope","global"},{"repeat","ignored"}};}
thread_local ExplorerShortcuts* ExplorerShortcuts::current_=nullptr;
ExplorerShortcuts::ExplorerShortcuts(SelectionReader reader,ForegroundProbe foreground)
    :action_ready_(CreateEventW(nullptr,FALSE,FALSE,nullptr)),read_selection_(std::move(reader)),foreground_(std::move(foreground)) {
    if(!action_ready_)throw Error(1,system_error("Shortcut action event"));
    if(!foreground_)foreground_=GetForegroundWindow;
    if(!read_selection_)read_selection_=read_active_tab;
}
void ExplorerShortcuts::start_input() {
    Handle input_ready(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    capture_ready_.emplace(CreateEventW(nullptr,FALSE,FALSE,nullptr));
    stopping_.emplace(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!input_ready||!*capture_ready_||!*stopping_)throw Error(1,system_error("Shortcut input events"));
    try {
        capture_thread_=std::thread([this]{capture();});
        input_thread_=std::thread([this,ready=input_ready.get()]{
            current_=this;MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);input_id_=GetCurrentThreadId();SetEvent(ready);
            while(GetMessageW(&message,nullptr,0,0)>0) {
                if(message.message==input_update){
                    std::deque<std::function<void()>> commands;{std::lock_guard lock(command_mutex_);commands.swap(commands_);}
                    for(auto& command:commands)command();
                }else if(message.message==WM_TIMER&&!message.hwnd&&message.wParam==priority_timer_)refresh_priority(true);
                else {TranslateMessage(&message);DispatchMessageW(&message);}
            }
            detach_input_hooks();current_=nullptr;
        });
        WaitForSingleObject(input_ready.get(),INFINITE);
    }catch(...){SetEvent(stopping_->get());if(capture_thread_.joinable())capture_thread_.join();throw;}
}
ExplorerShortcuts::~ExplorerShortcuts(){
    ++generation_;
    if(input_thread_.joinable()){PostThreadMessageW(input_id_,WM_QUIT,0,0);input_thread_.join();}
    if(capture_thread_.joinable()){SetEvent(stopping_->get());capture_thread_.join();}
}
bool ExplorerShortcuts::post_input(std::function<void()> command) {
    {std::lock_guard lock(command_mutex_);commands_.push_back(std::move(command));}
    return PostThreadMessageW(input_id_,input_update,0,0)!=FALSE;
}
void ExplorerShortcuts::configure(std::shared_ptr<const Settings> settings) {
    settings_=std::move(settings);
    if(settings_->bindings.empty()||settings_->windows.empty()){
        detach_input_hooks();
        return;
    }
    if(!foreground_hook_){
        foreground_hook_=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,foreground_changed,0,0,WINEVENT_OUTOFCONTEXT);
        const auto error=foreground_hook_?std::string{}:system_error("Shortcut foreground notifications");
        std::lock_guard lock(error_mutex_);foreground_error_=error;
    }
    refresh_priority(false);
    bound_=hook_?settings_->bindings.size():0;
}
void ExplorerShortcuts::detach_input_hooks() {
    if(priority_timer_)KillTimer(nullptr,std::exchange(priority_timer_,0));
    if(foreground_hook_)UnhookWinEvent(std::exchange(foreground_hook_,nullptr));
    if(hook_)UnhookWindowsHookEx(std::exchange(hook_,nullptr));
    priority_window_=nullptr;
    pressed_.reset();consumed_.reset();
    hooked_=false;bound_=0;
    std::lock_guard lock(error_mutex_);
    hook_error_.clear();foreground_error_.clear();timer_error_.clear();capture_error_.clear();
}
void CALLBACK ExplorerShortcuts::foreground_changed(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD) {
    if(current_)current_->refresh_priority(false);
}
void ExplorerShortcuts::refresh_priority(bool periodic) {
    if(!settings_||settings_->bindings.empty()||settings_->windows.empty())return;
    const auto foreground=foreground_();
    const auto window=settings_->windows.contains(foreground)?foreground:nullptr;
    const bool entered=std::exchange(priority_window_,window)!=window;
    const bool keep_timer=window||!foreground_hook_;
    if(keep_timer&&!priority_timer_){
        priority_timer_=SetTimer(nullptr,0,shortcut_priority_interval_ms,nullptr);
        const auto error=priority_timer_?std::string{}:system_error("Shortcut priority timer");
        std::lock_guard lock(error_mutex_);timer_error_=error;
    }else if(!keep_timer&&priority_timer_){
        KillTimer(nullptr,std::exchange(priority_timer_,0));
    }
    if((window&&(entered||periodic))||(!hook_&&!periodic))install_keyboard_hook();
}
void ExplorerShortcuts::install_keyboard_hook() {
    // Windows inserts each new hook at the head of its chain.
    const auto next=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard,GetModuleHandleW(nullptr),0);
    if(!next){
        const auto error=system_error("Foreground keyboard hook");
        std::lock_guard lock(error_mutex_);hook_error_=error;
        return;
    }
    if(hook_&&!UnhookWindowsHookEx(hook_)){
        const auto code=GetLastError();
        if(code!=ERROR_INVALID_HOOK_HANDLE){
            UnhookWindowsHookEx(next);
            std::lock_guard lock(error_mutex_);hook_error_=system_error("Replace foreground keyboard hook",code);
            return;
        }
    }
    if(hook_)++priority_rearms_;
    hook_=next;hooked_=true;bound_=settings_->bindings.size();
    std::lock_guard lock(error_mutex_);hook_error_.clear();
}
bool ExplorerShortcuts::on_key(UINT key,bool down,UINT modifiers,HWND foreground) {
    if(key>=256)return false;
    if(!down){pressed_.reset(key);bool consumed=consumed_.test(key);consumed_.reset(key);return consumed;}
    if(consumed_.test(key))return true;
    const bool first=!pressed_.test(key);pressed_.set(key);
    if(!first||!settings_||settings_->generation!=generation_)return false;
    auto window=settings_->windows.find(foreground);if(window==settings_->windows.end())return false;
    for(size_t i=0;i<settings_->bindings.size();++i) {
        const auto& chord=settings_->bindings[i].chord;
        if(chord.key!=key||chord.modifiers!=(modifiers|MOD_NOREPEAT))continue;
        const auto& action=settings_->bindings[i].action;
        if(!window->second.frame&&action.starts_with("toggle_")&&shell_shortcut_action(action).empty())continue;
        const auto head=head_.load(std::memory_order_relaxed),next=(head+1)%pending_.size();
        if(next==tail_.load(std::memory_order_acquire)){++dropped_;return false;}
        pending_[head]={i,window->second,settings_->generation,GetTickCount64(),settings_};
        head_.store(next,std::memory_order_release);SetEvent(capture_ready_->get());consumed_.set(key);return true;
    }
    return false;
}
LRESULT CALLBACK ExplorerShortcuts::keyboard(int code,WPARAM message,LPARAM data) {
    auto self=current_;
    if(code==HC_ACTION&&self) {
        auto key=reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        if(key->flags&LLKHF_INJECTED)return CallNextHookEx(nullptr,code,message,data);
        if(message==WM_KEYUP||message==WM_SYSKEYUP){if(self->on_key(key->vkCode,false,0,nullptr))return 1;}
        else if(message==WM_KEYDOWN||message==WM_SYSKEYDOWN) {
            if(key->vkCode<self->consumed_.size()&&self->consumed_.test(key->vkCode))return 1;
            // Most typing takes this path without even querying modifier state.
            if(!self->settings_||self->settings_->windows.empty())return CallNextHookEx(nullptr,code,message,data);
            bool candidate=false;for(auto& binding:self->settings_->bindings)if(binding.chord.key==key->vkCode){candidate=true;break;}
            if(!candidate)return CallNextHookEx(nullptr,code,message,data);
            const auto foreground=self->foreground_();if(!self->settings_->windows.contains(foreground))return CallNextHookEx(nullptr,code,message,data);
            UINT modifiers{};
            if(GetAsyncKeyState(VK_CONTROL)&0x8000)modifiers|=MOD_CONTROL;
            if(GetAsyncKeyState(VK_MENU)&0x8000)modifiers|=MOD_ALT;
            if(GetAsyncKeyState(VK_SHIFT)&0x8000)modifiers|=MOD_SHIFT;
            if((GetAsyncKeyState(VK_LWIN)|GetAsyncKeyState(VK_RWIN))&0x8000)modifiers|=MOD_WIN;
            if(self->on_key(key->vkCode,true,modifiers,foreground))return 1;
        }
    }
    return CallNextHookEx(nullptr,code,message,data);
}
void ExplorerShortcuts::update(const Json& config,const std::vector<NativeTab>& tabs,bool enabled) {
    if(config!=config_) {
        config_=config;bindings_.clear();
        for(auto it=config.begin();it!=config.end();++it) {
            auto chord=parse_shortcut(it.value().get<std::string>());if(!chord.key)continue;
            bindings_.push_back({it.key(),chord});
        }
    }
    const bool has_targets=enabled&&!bindings_.empty()&&!tabs.empty();
    if(!input_thread_.joinable()){
        if(!has_targets)return;
        start_input();
    }
    auto settings=std::make_shared<Settings>();
    if(has_targets){settings->bindings=bindings_;for(auto& tab:tabs)settings->windows.try_emplace(tab.window,tab);}
    if(published_&&(hooked_||settings->bindings.empty()||settings->windows.empty())&&published_->bindings==settings->bindings&&published_->windows.size()==settings->windows.size()&&
        std::equal(published_->windows.begin(),published_->windows.end(),settings->windows.begin(),[](const auto& a,const auto& b){
            const auto& x=a.second;const auto& y=b.second;
            return a.first==b.first&&x.process_id==y.process_id&&x.process_started==y.process_started&&x.frame==y.frame&&x.guid==y.guid;
        }))return;
    settings->generation=++generation_;published_=settings;
    post_input([this,settings]{configure(settings);});
}
void ExplorerShortcuts::capture() {
    const HANDLE handles[]{stopping_->get(),capture_ready_->get()};
    while (true) {
        const auto wake=WaitForMultipleObjects(2,handles,FALSE,INFINITE);
        if (wake!=WAIT_OBJECT_0+1&&wake!=WAIT_TIMEOUT)return;
        auto tail=tail_.load(std::memory_order_relaxed);
        while(tail!=head_.load(std::memory_order_acquire)) {
            auto pending=std::move(pending_[tail]);tail=(tail+1)%pending_.size();tail_.store(tail,std::memory_order_release);
            if(WaitForSingleObject(handles[0],0)==WAIT_OBJECT_0)return;
            if(pending.generation!=generation_||GetTickCount64()-pending.time>250||foreground_()!=pending.tab.window){++dropped_;continue;}
            try {
                const auto& action=pending.settings->bindings[pending.binding].action;
                std::optional<GUID> selected;
                if(pending.tab.frame)selected=read_selection_(pending.tab);
                else selected=GUID{};
                if(!selected||pending.generation!=generation_||GetTickCount64()-pending.time>250||foreground_()!=pending.tab.window){++dropped_;continue;}
                ShortcutEvent event{action,pending.tab.window,*selected,pending.tab.process_started,pending.tab.process_id,
                    pending.settings->bindings[pending.binding].chord};
                std::lock_guard lock(event_mutex_);
                if(events_.size()<16){
                    events_.emplace_back(pending.generation,std::move(event));
                    SetEvent(action_ready_.get());
                }else ++dropped_;
            }catch(const std::exception& error){std::lock_guard lock(error_mutex_);capture_error_=error.what();++dropped_;}
        }
    }
}
std::deque<ShortcutEvent> ExplorerShortcuts::take(){
    std::deque<std::pair<uint64_t,ShortcutEvent>> pending;{std::lock_guard lock(event_mutex_);pending.swap(events_);}
    std::deque<ShortcutEvent> result;for(auto& [generation,event]:pending)if(generation==generation_)result.push_back(std::move(event));return result;
}
Json ExplorerShortcuts::status(){
    Json errors=Json::object();
    {
        std::lock_guard lock(error_mutex_);
        if(!hook_error_.empty())errors["listener"]=hook_error_;
        if(!foreground_error_.empty())errors["priority_foreground"]=foreground_error_;
        if(!timer_error_.empty())errors["priority_timer"]=timer_error_;
        if(!capture_error_.empty())errors["capture"]=capture_error_;
    }
    return {{"available",true},{"hook_active",hooked_.load()},{"input_thread_id",input_id_.load()},
        {"bound",bound_.load()},{"errors",errors},{"dropped",dropped_.load()},{"scope","foreground verified Explorer only"},
        {"priority_rearms",priority_rearms_.load()},{"priority_interval_ms",shortcut_priority_interval_ms},
        {"input_dispatch","dedicated thread; selection reads on separate worker"},{"repeat","ignored"}};
}
namespace {constexpr UINT notification_message=WM_APP+0x541;}
SystemNotifications::SystemNotifications() {
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"Sinan.NotificationHost.1";RegisterClassW(&cls);
    window_=CreateWindowExW(0,cls.lpszClassName,L"Sinan",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,this);
    if(!window_)error_=system_error("Notification host");
}
void SystemNotifications::remove(UINT id){NOTIFYICONDATAW n{sizeof(n)};n.hWnd=window_;n.uID=id;Shell_NotifyIconW(NIM_DELETE,&n);items_.erase(id);}
SystemNotifications::~SystemNotifications(){while(!items_.empty())remove(items_.begin()->first);if(window_)DestroyWindow(window_);}
LRESULT CALLBACK SystemNotifications::procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    auto self=reinterpret_cast<SystemNotifications*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<SystemNotifications*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(message==notification_message&&self) {
        auto id=static_cast<UINT>(w);auto it=self->items_.find(id);
        if(it!=self->items_.end()) {
            if(l==NIN_BALLOONUSERCLICK||l==WM_LBUTTONUP){self->clicks_.push_back(it->second.tab_id);self->remove(id);}
            else if(l==NIN_BALLOONTIMEOUT)self->remove(id);
        }
        return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
bool SystemNotifications::show(const std::string& mode,const std::string& text,const std::string& tab_id,bool failure,const std::string& throttle_key) {
    if(mode=="off"||(mode=="errors"&&!failure))return false;
    if(!window_)return false;
    auto now=GetTickCount64();
    for(auto it=items_.begin();it!=items_.end();)if(it->second.expires<=now){auto id=it->first;++it;remove(id);}else ++it;
    if(!throttle_key.empty()){auto& last=throttle_[throttle_key];if(last&&now-last<10000)return false;last=now;}
    while(items_.size()>=20)remove(items_.begin()->first);
    NOTIFYICONDATAW n{sizeof(n)};n.hWnd=window_;n.uID=next_++;n.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;n.uCallbackMessage=notification_message;
    n.hIcon=LoadIconW(nullptr,IDI_INFORMATION);wcscpy_s(n.szTip,L"Sinan");
    if(!Shell_NotifyIconW(NIM_ADD,&n)){error_=system_error("System notification registration");return false;}
    items_[n.uID]={tab_id,now+120000};n.uFlags=NIF_INFO;n.dwInfoFlags=failure?NIIF_WARNING:NIIF_INFO;
    wcscpy_s(n.szInfoTitle,L"Sinan");wcsncpy_s(n.szInfo,wide(text).c_str(),_TRUNCATE);n.uTimeout=10000;
    if(!Shell_NotifyIconW(NIM_MODIFY,&n)){error_=system_error("System notification delivery");remove(n.uID);return false;}
    error_.clear();return true;
}
std::deque<std::string> SystemNotifications::clicks(){return std::exchange(clicks_,{});}
Json SystemNotifications::status()const{return {{"available",window_!=nullptr},{"error",error_},{"transport","Win32 Shell system notification"},{"delivery","subject to Windows notification settings"}};}
}
