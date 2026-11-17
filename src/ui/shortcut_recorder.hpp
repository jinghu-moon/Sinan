#pragma once
#include "platform/windows.hpp"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <bitset>
namespace sinan {
std::string shortcut_chord(UINT modifiers,UINT key);
struct CapturedShortcut {int field{};std::string chord;};
// Records input only while a shortcut field in this terminal has focus.
// The hook is needed because Ctrl+C/Q can otherwise terminate the terminal UI.
class ShortcutRecorder {
    HWND terminal_{};
    std::function<void()> wake_;
    std::thread thread_;
    std::atomic<DWORD> thread_id_{};
    std::atomic<int> field_{};
    std::atomic<bool> hooked_{};
    Handle ready_;
    HHOOK hook_{};
    std::mutex mutex_;
    std::deque<CapturedShortcut> events_;
    std::bitset<256> consumed_;
    std::string error_;
    static thread_local ShortcutRecorder* current_;
    static LRESULT CALLBACK keyboard(int,WPARAM,LPARAM);
    void update_hook();
public:
    ShortcutRecorder(HWND terminal,std::function<void()> wake);
    ~ShortcutRecorder();
    void focus(int field);
    std::deque<CapturedShortcut> take();
    std::string error();
    bool hook_active() const{return hooked_;}
};
}
