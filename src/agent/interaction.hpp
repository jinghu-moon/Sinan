#pragma once
#include "config/config.hpp"
#include "hook/native.hpp"
#include <deque>
#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <thread>
#include <array>
#include <bitset>
#include <memory>

namespace sinan {
struct ShortcutEvent {std::string action;HWND window{};GUID expected_guid{};uint64_t process_started{};};
class DirectoryShortcuts {
    HWND window_{};
    std::map<int,std::string> ids_;
    Json config_,errors_=Json::object();
    uint64_t retry_{};
    std::deque<ShortcutEvent> events_;
    static LRESULT CALLBACK procedure(HWND,UINT,WPARAM,LPARAM);
public:
    DirectoryShortcuts();
    ~DirectoryShortcuts();
    void update(const Json& entries);
    std::deque<ShortcutEvent> take();
    Json status() const;
};
class ExplorerShortcuts {
public:
    using SelectionReader=std::function<std::optional<GUID>(const NativeTab&)>;
    using ForegroundProbe=std::function<HWND()>;
private:
    struct Binding {std::string action;Shortcut chord;bool operator==(const Binding&) const=default;};
    struct Settings {std::vector<Binding> bindings;std::map<HWND,NativeTab> windows;uint64_t generation{};};
    struct Pending {size_t binding{};NativeTab tab;uint64_t generation{},time{};std::shared_ptr<const Settings> settings;};
    // One producer (hook thread) and one consumer (selection worker). The
    // callback never waits for a lock, allocator, remote read, scan or IPC.
    std::array<Pending,17> pending_;
    std::atomic<size_t> head_{},tail_{};
    std::atomic<uint64_t> generation_{},dropped_{};
    std::atomic<bool> hooked_{};
    std::atomic<size_t> bound_{};
    std::atomic<DWORD> input_id_{};
    Handle input_ready_,capture_ready_,stopping_;
    std::thread input_thread_,capture_thread_;
    SelectionReader read_selection_;
    ForegroundProbe foreground_;
    std::mutex command_mutex_,event_mutex_,error_mutex_;
    std::deque<std::function<void()>> commands_;
    std::deque<std::pair<uint64_t,ShortcutEvent>> events_;
    std::string hook_error_;
    Json config_,errors_=Json::object();
    uint64_t retry_{};
    HHOOK hook_{};
    std::shared_ptr<const Settings> settings_;
    std::shared_ptr<const Settings> published_;
    std::bitset<256> pressed_,consumed_;
    std::vector<Binding> bindings_;
    bool post_input(std::function<void()> command);
    void configure(std::shared_ptr<const Settings> settings);
    bool on_key(UINT key,bool down,UINT modifiers,HWND foreground);
    void capture();
    static LRESULT CALLBACK keyboard(int,WPARAM,LPARAM);
    static thread_local ExplorerShortcuts* current_;
    friend struct ShortcutInputTestAccess;
public:
    explicit ExplorerShortcuts(SelectionReader reader={},ForegroundProbe foreground=GetForegroundWindow);
    ~ExplorerShortcuts();
    void update(const Json& config,const std::vector<NativeTab>& tabs,bool enabled);
    std::deque<ShortcutEvent> take();
    Json status();
};
class SystemNotifications {
    HWND window_{};
    UINT next_=1;
    struct Item {std::string tab_id;uint64_t expires;};
    std::map<UINT,Item> items_;
    std::deque<std::string> clicks_;
    std::map<std::string,uint64_t> throttle_;
    std::string error_;
    static LRESULT CALLBACK procedure(HWND,UINT,WPARAM,LPARAM);
    void remove(UINT id);
public:
    SystemNotifications();
    ~SystemNotifications();
    bool show(const std::string& mode,const std::string& text,const std::string& tab_id,bool failure=false,const std::string& throttle_key={});
    std::deque<std::string> clicks();
    Json status() const;
};
}
