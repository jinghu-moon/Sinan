#pragma once
#include "platform/shell_tabs.hpp"
#include "config/config.hpp"
#include "hook/controller.hpp"
#include <map>
#include <functional>
#include <deque>

namespace sinan {
struct DirectoryTarget {
    std::string tab_id;
    std::optional<NativeTab> foreground_anchor;
    bool locations_resolved=true;
};
struct ClosedTab {
    uint64_t sequence;
    HWND window;
    DWORD process_id;
    uint64_t process_started;
    std::wstring path;
};

// STA-owned adapter. COM identities are held across scans to prevent pointer reuse.
// The synthetic GUID is an observation lifetime ID, NOT Explorer's native tab GUID.
class ShellTabTracker {
public:
    using ProcessProbe = std::function<ProcessLifetime(DWORD, uint64_t)>;
private:
    struct Tracked {
        Microsoft::WRL::ComPtr<IUnknown> identity;
        HWND browser_window = nullptr;
        DWORD process_id = 0;
        uint64_t process_started = 0;
        std::string id;
        unsigned missing_scans = 0;
        bool resolved = false;
        std::string warning;
        std::optional<NativeTab> native;
        std::optional<NativeTab> controlled;
        std::wstring applied_title;
        std::string bridge_error;
        bool observed = false;
        uint64_t missing_sequence = 0;
        std::optional<std::wstring> last_path;
        bool native_only = false;
    };
    TabRegistry registry_;
    RuntimePolicy policy_;
    uint64_t next_generation_ = 1;
    ProcessProbe process_probe_;
    std::map<std::string, Tracked, std::less<>> tracked_;
    std::deque<ClosedTab> closed_tabs_;
    uint64_t close_sequence_ = 0;
    void prune_exited_processes();
public:
    ShellTabTracker(std::string runtime_id, const Config& config, ProcessProbe process_probe = process_lifetime);
    void reconcile(const ShellTabScan& scan);
    void apply_config(const Config& config);
    Json list() const;
    const std::deque<ClosedTab>& closed_tabs() const { return closed_tabs_; }
    void forget_closed_tab(uint64_t sequence);
    Json set_depth(std::string_view id, std::optional<int> depth);
    Json reset_depth(std::string_view id);
    NativeTab native_tab(std::string_view id) const;
    std::vector<NativeTab> native_tabs() const;
    std::vector<NativeTab> shortcut_windows() const;
    std::pair<HWND,HWND> shell_target(std::string_view id) const;
    DirectoryTarget directory_target(std::wstring_view path, HWND foreground, bool foreground_only = false) const;
    using TitleTransport = std::function<BridgeResult(const NativeTab&, BridgeAction, const RuntimePolicy&)>;
    Json synchronize_titles(bool enabled, const TitleTransport& transport = send_title_request);
};
}
