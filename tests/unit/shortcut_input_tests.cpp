#include "agent/interaction.hpp"
#include <chrono>
#include <future>
#include <iostream>
using namespace tabtail;
using namespace std::chrono_literals;

// Drive the same callback decision path on its actual owner thread without
// SendInput, keyboard injection, Explorer changes or typing into user apps.
namespace tabtail {
struct ShortcutInputTestAccess {
    static std::future<DWORD> ping(ExplorerShortcuts& shortcuts) {
        auto promise=std::make_shared<std::promise<DWORD>>();auto result=promise->get_future();
        shortcuts.post_input([promise]{promise->set_value(GetCurrentThreadId());});return result;
    }
    static std::future<bool> key(ExplorerShortcuts& shortcuts,UINT key,bool down,UINT modifiers,HWND window) {
        auto promise=std::make_shared<std::promise<bool>>();auto result=promise->get_future();
        shortcuts.post_input([&,key,down,modifiers,window,promise]{promise->set_value(shortcuts.on_key(key,down,modifiers,window));});return result;
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
    NativeTab tab;tab.window=window;tab.process_started=42;GUID guid{};guid.Data1=123;
    ExplorerShortcuts shortcuts([&](const NativeTab&)->std::optional<GUID>{
        reader_thread=GetCurrentThreadId();++reads;if(blocked){SetEvent(entered.get());WaitForSingleObject(release.get(),INFINITE);}return guid;
    },[&]{return foreground.load();});
    // Release the test reader even when an assertion throws.
    struct ReleaseOnExit {HANDLE event;~ReleaseOnExit(){SetEvent(event);}} cleanup{release.get()};
    auto config=Json{{"toggle_pin","Ctrl+Alt+Shift+F24"}};const UINT mods=MOD_CONTROL|MOD_ALT|MOD_SHIFT;
    auto ping=ShortcutInputTestAccess::ping(shortcuts);expect(ping.wait_for(1s)==std::future_status::ready,"input thread starts");const auto input_thread=ping.get();
    expect(input_thread!=main_thread,"input hook owns an independent message thread");
    expect(!shortcuts.status().at("hook_active").get<bool>(),"no hook on startup");
    shortcuts.update(config,{tab},false);ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(!shortcuts.status().at("hook_active").get<bool>(),"no hook in observation mode");
    shortcuts.update(config,{},true);ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(!shortcuts.status().at("hook_active").get<bool>(),"no hook without Explorer targets");
    shortcuts.update(config,{tab},true);ShortcutInputTestAccess::ping(shortcuts).wait();
    expect(shortcuts.status().at("hook_active").get<bool>(),"eligible Explorer shortcut installs hook");
    expect(!key(shortcuts,VK_F24,true,mods,reinterpret_cast<HWND>(uintptr_t(0x5678))),"other application retains shortcut");
    key(shortcuts,VK_F24,false,0,nullptr);
    expect(!key(shortcuts,'A',true,0,window)&&reads==0,"normal text does not trigger a selection read");
    key(shortcuts,'A',false,0,nullptr);
    expect(key(shortcuts,VK_F24,true,mods,window),"configured chord is captured");
    expect(key(shortcuts,VK_F24,true,mods,window),"held chord is consumed without repeat");
    expect(key(shortcuts,VK_F24,false,0,nullptr),"captured chord consumes matching release");
    std::deque<ShortcutEvent> events;
    expect(until([&]{events=shortcuts.take();return !events.empty();}),"selection worker emits verified target");
    expect(events.size()==1&&events.front().expected_guid==guid&&events.front().process_started==42,"event preserves target identity and lifetime");
    expect(reader_thread!=input_thread&&reader_thread!=main_thread,"selection reading runs outside hook and agent threads");
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
    {ShortcutRecorder recorder(GetForegroundWindow(),[]{});
        expect(!recorder.hook_active(),"TUI installs no idle recording hook");
        recorder.focus(4);expect(until([&]{return recorder.hook_active();}),"shortcut field arms recording hook");
        recorder.focus(0);expect(until([&]{return !recorder.hook_active();}),"leaving shortcut field removes recording hook");
    }
    std::cout<<checks<<" shortcut input isolation checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
