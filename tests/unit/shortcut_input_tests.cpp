#include "agent/interaction.hpp"
#include "ui/shortcut_recorder.hpp"
#include <chrono>
#include <future>
#include <iostream>
using namespace sinan;
using namespace std::chrono_literals;

// Drive the same callback decision path on its actual owner thread without
// SendInput, keyboard injection, Explorer changes or typing into user apps.
namespace sinan {
struct ShortcutInputTestAccess {
    static std::future<DWORD> ping(ExplorerShortcuts& shortcuts) {
        auto promise=std::make_shared<std::promise<DWORD>>();auto result=promise->get_future();
        shortcuts.post_input([promise]{promise->set_value(GetCurrentThreadId());});return result;
    }
    static std::future<bool> key(ExplorerShortcuts& shortcuts,UINT key,bool down,UINT modifiers,HWND window) {
        auto promise=std::make_shared<std::promise<bool>>();auto result=promise->get_future();
        shortcuts.post_input([&,key,down,modifiers,window,promise]{promise->set_value(shortcuts.on_key(key,down,modifiers,window));});return result;
    }
    static void foreground_changed(ExplorerShortcuts& shortcuts) {
        auto done=std::make_shared<std::promise<void>>();
        auto result=done->get_future();
        shortcuts.post_input([done]{
            ExplorerShortcuts::foreground_changed(nullptr,EVENT_SYSTEM_FOREGROUND,nullptr,0,0,0,0);
            done->set_value();
        });
        result.get();
    }
};
}
namespace {
int checks{};
void expect(bool result,const char* message){++checks;if(!result)throw std::runtime_error(message);}
template<class Predicate>bool until(Predicate predicate){auto end=GetTickCount64()+2000;do{if(predicate())return true;Sleep(1);}while(GetTickCount64()<end);return false;}
bool key(ExplorerShortcuts& shortcuts,UINT code,bool down,UINT modifiers,HWND window){
    auto result=ShortcutInputTestAccess::key(shortcuts,code,down,modifiers,window);
    expect(result.wait_for(300ms)==std::future_status::ready,"input dispatch blocked by owner or selection worker");return result.get();
}
}
int main(){try {
    const auto main_thread=GetCurrentThreadId();const auto window=reinterpret_cast<HWND>(uintptr_t(0x1234));
    std::atomic<HWND> foreground{window};std::atomic<DWORD> reader_thread{};
    std::atomic<int> reads{};std::atomic<bool> blocked{};
    Handle entered(CreateEventW(nullptr,TRUE,FALSE,nullptr)),release(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    NativeTab tab;tab.window=window;tab.process_started=42;tab.frame=1;GUID guid{};guid.Data1=123;
    ExplorerShortcuts shortcuts([&](const NativeTab&)->std::optional<GUID>{
        reader_thread=GetCurrentThreadId();++reads;if(blocked){SetEvent(entered.get());WaitForSingleObject(release.get(),INFINITE);}return guid;
    },[&]{return foreground.load();});
    // Release the test reader even when an assertion throws.
    struct ReleaseOnExit {HANDLE event;~ReleaseOnExit(){SetEvent(event);}} cleanup{release.get()};
    auto config=Json{{"toggle_pin","Ctrl+Alt+Shift+F24"}};const UINT mods=MOD_CONTROL|MOD_ALT|MOD_SHIFT;
    struct HotkeyRegistration {
        int id;
        ~HotkeyRegistration(){UnregisterHotKey(nullptr,id);}
    } competing_hotkey{0x6680};
    expect(RegisterHotKey(nullptr,competing_hotkey.id,mods|MOD_NOREPEAT,VK_F24)!=FALSE,"register a competing global shortcut");
    expect(shortcuts.status().at("input_thread_id")==0,"no input thread on startup");
    shortcuts.update(config,{tab},false);
    expect(shortcuts.status().at("input_thread_id")==0,"no input thread for disabled bindings");
    shortcuts.update(config,{},true);
    expect(shortcuts.status().at("input_thread_id")==0,"no input thread without Explorer targets");
    shortcuts.update(config,{tab},true);
    auto ping=ShortcutInputTestAccess::ping(shortcuts);
    expect(ping.wait_for(1s)==std::future_status::ready,"input thread starts on demand");
    const auto input_thread=ping.get();
    expect(input_thread!=main_thread,"input hook owns an independent message thread");
    expect(shortcuts.status().at("hook_active").get<bool>(),"eligible Explorer shortcut installs hook");
    expect(shortcuts.status().at("bound")==1,"competing RegisterHotKey does not reject the Explorer binding");
    expect(!key(shortcuts,VK_F24,true,mods,reinterpret_cast<HWND>(uintptr_t(0x5678))),"other application retains shortcut");
    key(shortcuts,VK_F24,false,0,nullptr);
    expect(!key(shortcuts,'A',true,0,window)&&reads==0,"normal text does not trigger a selection read");
    key(shortcuts,'A',false,0,nullptr);
    expect(key(shortcuts,VK_F24,true,mods,window),"configured chord is captured");
    expect(key(shortcuts,VK_F24,true,mods,window),"held chord is consumed without repeat");
    expect(key(shortcuts,VK_F24,false,0,nullptr),"captured chord consumes matching release");
    expect(WaitForSingleObject(shortcuts.action_ready(),1000)==WAIT_OBJECT_0,"a verified shortcut wakes the agent");
    std::deque<ShortcutEvent> events;
    expect(until([&]{events=shortcuts.take();return !events.empty();}),"selection worker emits verified target");
    expect(events.size()==1&&events.front().expected_guid==guid&&events.front().process_started==42,"event preserves target identity and lifetime");
    expect(reader_thread!=input_thread&&reader_thread!=main_thread,"selection reading runs outside hook and agent threads");
    const auto rearms=[&]{return shortcuts.status().at("priority_rearms").get<uint64_t>();};
    foreground=nullptr;ShortcutInputTestAccess::foreground_changed(shortcuts);
    const auto outside=rearms();
    Sleep(shortcuts.status().at("priority_interval_ms").get<DWORD>()+100);
    expect(rearms()==outside,"hook chain is not refreshed outside Explorer");
    foreground=window;ShortcutInputTestAccess::foreground_changed(shortcuts);
    if(rearms()==outside)throw std::runtime_error("Foreground did not refresh the chain: "+shortcuts.status().dump());
    const auto on_entry=rearms();
    expect(until([&]{return rearms()>on_entry;}),"foreground timer refreshes the chain while Explorer stays active");
    expect(key(shortcuts,VK_F24,true,mods,window),"refreshed hook still captures the competing chord");
    expect(key(shortcuts,VK_F24,false,0,nullptr),"refreshed hook still consumes the captured key release");
    expect(until([&]{events=shortcuts.take();return events.size()==1;}),"refreshed hook emits one event");
    blocked=true;
    expect(key(shortcuts,VK_F24,true,mods,window),"slow selection request queued");key(shortcuts,VK_F24,false,0,nullptr);
    expect(WaitForSingleObject(entered.get(),2000)==WAIT_OBJECT_0,"slow reader entered");
    // Neither a sleeping agent nor a stalled remote read may block callbacks.
    auto responsive=ShortcutInputTestAccess::ping(shortcuts);Sleep(150);
    expect(responsive.wait_for(0ms)==std::future_status::ready,"input thread pumps while agent thread is busy");
    expect(!key(shortcuts,'B',true,0,window),"text passes while selection worker is blocked");
    key(shortcuts,'B',false,0,nullptr);
    int accepted=0;for(int i=0;i<24;++i){if(key(shortcuts,VK_F24,true,mods,window))++accepted;key(shortcuts,VK_F24,false,0,nullptr);}
    expect(accepted==16&&shortcuts.status().at("dropped").get<uint64_t>()>=8,"full bounded queue passes input instead of blocking");
    shortcuts.update(config,{tab},false);ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(!shortcuts.status().at("hook_active").get<bool>(),"hook detaches even while selection worker is stalled");
    SetEvent(release.get());expect(until([&]{return shortcuts.status().at("dropped").get<uint64_t>()>=25;}),"reader drains invalidated queued and in-flight actions");
    expect(shortcuts.take().empty(),"disabled generation emits no stale actions");
    blocked=false;ResetEvent(entered.get());ResetEvent(release.get());
    shortcuts.update(Json{{"toggle_pin",""}},{tab},true);ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(!shortcuts.status().at("hook_active").get<bool>(),"no hook when all shortcuts are empty");
    const auto before_shell_reads=reads.load();
    tab.process_id=13;tab.frame=0;
    shortcuts.update(Json{{"new_folder","Ctrl+Alt+Shift+F24"}},{tab},true);
    ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(key(shortcuts,VK_F24,true,mods,window),"Shell action accepts an observed Explorer window without a native mapping");
    key(shortcuts,VK_F24,false,0,nullptr);
    expect(until([&]{events=shortcuts.take();return !events.empty();}),"Shell action is dispatched");
    expect(events.front().action=="new_folder"&&events.front().process_id==13&&reads==before_shell_reads,"Shell action can use the public Explorer view");
    shortcuts.update(Json{{"new_file","Ctrl+Alt+Shift+F24"}},{tab},true);
    ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(!key(shortcuts,VK_F24,true,mods,reinterpret_cast<HWND>(uintptr_t(0x5678))),"New file chord passes through outside Explorer");
    key(shortcuts,VK_F24,false,0,nullptr);
    expect(key(shortcuts,VK_F24,true,mods,window),"New file chord is captured in Explorer");
    key(shortcuts,VK_F24,false,0,nullptr);
    expect(until([&]{events=shortcuts.take();return !events.empty();}),"New file action is dispatched");
    expect(shell_shortcut_action(events.front().action)=="explorer.new_file"&&events.front().process_id==13&&reads==before_shell_reads,
        "New file action supports the public Explorer view");
    shortcuts.update(Json{{"script:context","Ctrl+Alt+Shift+F24"}},{tab},true);
    ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(!key(shortcuts,VK_F24,true,mods,reinterpret_cast<HWND>(uintptr_t(0x5678))),"script chord remains local to Explorer");
    key(shortcuts,VK_F24,false,0,nullptr);
    expect(key(shortcuts,VK_F24,true,mods,window),"configured script chord is captured in Explorer");
    key(shortcuts,VK_F24,false,0,nullptr);
    expect(until([&]{events=shortcuts.take();return !events.empty();}),"script action is dispatched");
    expect(events.front().action=="script:context"&&events.front().chord==parse_shortcut("Ctrl+Alt+Shift+F24"),
           "script event preserves the binding for revalidation");
    shortcuts.update(Json::object(),{tab},true);
    ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(!key(shortcuts,VK_RETURN,true,MOD_CONTROL,window),"Ctrl+Enter passes through to Explorer");
    expect(!key(shortcuts,VK_RETURN,false,0,nullptr),"pass-through Ctrl+Enter leaves its release alone");
    expect(!key(shortcuts,VK_RETURN,true,0,window),"plain Enter remains native");
    key(shortcuts,VK_RETURN,false,0,nullptr);
    {ShortcutRecorder recorder(GetForegroundWindow(),[]{});
        expect(!recorder.hook_active(),"TUI installs no idle recording hook");
        recorder.focus(4);expect(until([&]{return recorder.hook_active();}),"shortcut field arms recording hook");
        recorder.focus(0);expect(until([&]{return !recorder.hook_active();}),"leaving shortcut field removes recording hook");
    }
    std::cout<<checks<<" shortcut input isolation checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
