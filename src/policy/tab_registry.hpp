#pragma once
#include "policy/display.hpp"
#include "policy/rules.hpp"
#include <array>
#include <compare>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>

namespace sinan {
// Supplied by the Explorer adapter, once native identity/lifetimes are verified.
// frame_id is an adapter identity, never a dereferenceable pointer in the CLI.
struct TabIdentity {
    uint32_t session_id = 0;
    uint32_t process_id = 0;
    uint64_t process_started = 0;
    uint64_t frame_id = 0;
    uint64_t frame_generation = 0;
    std::array<uint8_t, 16> tab_guid{};
    uint64_t tab_generation = 0;
    auto operator<=>(const TabIdentity&) const = default;
};
struct TabLocation {
    // nullopt denotes a virtual location or an unresolved path.
    std::optional<std::wstring> filesystem_path;
    std::wstring original_title;
    bool operator==(const TabLocation&) const = default;
};
enum class RefreshState { pending, applied, failed };
struct TabSnapshot {
    std::string id;
    TabIdentity identity;
    TabLocation location;
    std::optional<DepthOverride> depth_override;
    EffectiveDisplay effective;
    uint64_t defaults_revision = 1;
    uint64_t policy_sequence = 0;
    uint64_t location_sequence = 1;
    uint64_t render_version = 1;
    uint64_t applied_render_version = 0;
    RefreshState refresh = RefreshState::pending;
    std::string refresh_error;
};

// In-memory, synchronized registry for one agent runtime. No disk/IPC/Explorer calls.
// The agent MUST generate a fresh runtime_id for every startup, e.g. a UUID.
// Returned snapshots are copies; callers cannot mutate the registry through them.
class TabRegistry {
    const std::string runtime_id_;
    mutable std::mutex mutex_;
    DisplayOptions defaults_;
    uint64_t defaults_revision_;
    std::shared_ptr<const RuleSet> rules_;
    uint64_t next_id_ = 1;
    std::map<std::string, TabSnapshot, std::less<>> tabs_;
    std::map<TabIdentity, std::string> native_ids_;
    std::set<TabIdentity> retired_;

    TabSnapshot& require_tab(std::string_view id);
    TabSnapshot update_override(std::string_view id, std::optional<DepthOverride> override, uint64_t sequence);
    void erase_tab(std::map<std::string, TabSnapshot, std::less<>>::iterator tab);
public:
    explicit TabRegistry(std::string runtime_id, DisplayOptions defaults = {}, uint64_t defaults_revision = 1, std::shared_ptr<const RuleSet> rules = {});
    // Repeated discovery is idempotent. Use navigate() for subsequent location events.
    TabSnapshot register_tab(const TabIdentity& identity, TabLocation location, uint64_t location_sequence = 1);
    TabSnapshot get(std::string_view id) const;
    std::vector<TabSnapshot> list() const;
    TabSnapshot set_depth(std::string_view id, DepthOverride override, uint64_t sequence);
    TabSnapshot reset_depth(std::string_view id, uint64_t sequence);
    TabSnapshot navigate(std::string_view id, TabLocation location, uint64_t sequence);
    // Returns only tabs that need new rendering; updates metadata on all tabs.
    std::vector<TabSnapshot> apply_defaults(DisplayOptions defaults, uint64_t revision, std::shared_ptr<const RuleSet> rules = {});
    TabSnapshot request_refresh(std::string_view id);
    // Only the current pending render may be acknowledged. Stale/duplicate ACKs return false.
    bool acknowledge_render(std::string_view id, uint64_t version, bool success, std::string error = {});
    void close_tab(std::string_view id);
    size_t close_frame(uint32_t process_id, uint64_t process_started, uint64_t frame_id, uint64_t frame_generation);
    size_t close_process(uint32_t process_id, uint64_t process_started);
    void clear();
};
}
