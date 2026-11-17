#include "agent/tab_tracker.hpp"
#include "formatter/formatter.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace sinan {
namespace {
Json depth_json(std::optional<int> depth) { return depth ? Json(*depth) : Json("all"); }
uint64_t next_sequence(uint64_t previous) {
    if (previous == std::numeric_limits<uint64_t>::max()) throw Error(1, "标签更新序号已耗尽");
    return previous + 1;
}
}
ShellTabTracker::ShellTabTracker(std::string runtime_id, const Config& config, ProcessProbe process_probe)
    : registry_(std::move(runtime_id), config.display_options(), static_cast<uint64_t>(config.revision), std::make_shared<RuleSet>(config.rules)),
      process_probe_(std::move(process_probe)) {
    validate_config(config); policy_ = {config.display_options(), std::make_shared<RuleSet>(config.rules), std::nullopt};
}
void ShellTabTracker::prune_exited_processes() {
    std::map<std::pair<DWORD, uint64_t>, ProcessLifetime> lifetimes;
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        auto key = std::make_pair(it->second.process_id, it->second.process_started);
        auto [life, inserted] = lifetimes.try_emplace(key, ProcessLifetime::unknown);
        if (inserted) life->second = process_probe_(key.first, key.second);
        if (life->second == ProcessLifetime::ended) {
            // The lifetime is gone: do not restore or renew a lease against a reused PID/HWND.
            registry_.close_tab(it->first); it = tracked_.erase(it);
        } else ++it;
    }
}
void ShellTabTracker::reconcile(const ShellTabScan& scan) {
    std::set<std::string> seen;
    for (auto& [id, known] : tracked_) known.observed = false;
    for (const auto& tab : scan.tabs) {
        if (!tab.com_identity || !tab.window || !tab.browser_window) throw Error(1, "标签观察身份无效");
        auto found = std::find_if(tracked_.begin(), tracked_.end(), [&](const auto& entry) {
            const auto& known = entry.second;
            return (known.identity == tab.com_identity || (known.native&&tab.native&&tab.native->guid!=GUID_NULL&&known.native->guid==tab.native->guid&&known.native->window==tab.native->window)) && known.browser_window == tab.browser_window &&
                known.process_id == tab.process_id && known.process_started == tab.process_started;
        });
        if (found == tracked_.end()) {
            GUID guid{};
            if (FAILED(CoCreateGuid(&guid))) throw Error(1, "无法生成标签观察身份");
            TabIdentity identity;
            identity.session_id = session_id(); identity.process_id = tab.process_id; identity.process_started = tab.process_started;
            identity.frame_id = reinterpret_cast<uintptr_t>(tab.window);
            identity.frame_generation = next_sequence(next_generation_); next_generation_ = identity.frame_generation;
            identity.tab_generation = identity.frame_generation;
            static_assert(sizeof(guid) == sizeof(identity.tab_guid));
            std::memcpy(identity.tab_guid.data(), &guid, sizeof(guid));
            auto state = registry_.register_tab(identity, tab.location);
            Tracked known{tab.com_identity, tab.browser_window, tab.process_id, tab.process_started, state.id, 0, tab.location_resolved, tab.warning, tab.native};
            found = tracked_.emplace(state.id, std::move(known)).first;
        } else {
            auto state = registry_.get(found->first);
            // Unknown location explicitly clears the old path, never previews a stale path.
            if (state.location != tab.location) registry_.navigate(state.id, tab.location, next_sequence(state.location_sequence));
            found->second.resolved = tab.location_resolved;
            found->second.warning = tab.warning;
            found->second.native = tab.native;
        }
        found->second.missing_scans = 0;
        found->second.observed = true;
        seen.insert(found->first);
    }
    // A collection race alone must not destroy an override on a living process.
    if (scan.complete) {
        for (auto it = tracked_.begin(); it != tracked_.end();) {
            if (!seen.contains(it->first) && ++it->second.missing_scans >= 2) {
                registry_.close_tab(it->first); it = tracked_.erase(it);
            } else ++it;
        }
    }
    // Process exit is independent evidence even when Shell COM enumeration is incomplete.
    prune_exited_processes();
}
void ShellTabTracker::apply_config(const Config& config) {
    validate_config(config);
    if (policy_.defaults == config.display_options() && policy_.rules->definitions() == config.rules) {
        registry_.apply_defaults(config.display_options(), static_cast<uint64_t>(config.revision), policy_.rules); return;
    }
    auto rules = std::make_shared<RuleSet>(config.rules);
    registry_.apply_defaults(config.display_options(), static_cast<uint64_t>(config.revision), rules);
    policy_ = {config.display_options(), std::move(rules), std::nullopt};
}
std::vector<NativeTab> ShellTabTracker::native_tabs() const {
    std::vector<NativeTab> result;
    for(const auto& [id,tab]:tracked_)if(tab.observed&&tab.native)result.push_back(*tab.native);
    return result;
}
Json ShellTabTracker::list() const {
    Json result = Json::array();
    for (const auto& state : registry_.list()) {
        const auto& tracked = tracked_.at(state.id);
        auto title = format_tab_title(state);
        result.push_back({{"tab_id", state.id}, {"pid", state.identity.process_id},
            {"window", state.identity.frame_id}, {"browser_window", reinterpret_cast<uintptr_t>(tracked.browser_window)},
            {"path", state.location.filesystem_path ? Json(utf8(*state.location.filesystem_path)) : Json(nullptr)},
            {"original_title", utf8(state.location.original_title)}, {"preview_title", utf8(title.text)},
            {"depth", depth_json(state.effective.display.depth)},
            {"source", state.effective.source == PolicySource::tab ? "tab" : state.effective.source == PolicySource::rule ? "rule" : "global"},
            {"rule_id", state.effective.rule_id}, {"matched_pattern", utf8(state.effective.matched_pattern)}, {"action", state.effective.native ? "native" : "depth"},
            {"override", state.depth_override ? depth_json(state.depth_override->depth) : Json(nullptr)},
            {"defaults_revision", state.defaults_revision}, {"policy_sequence", state.policy_sequence},
            {"location_sequence", state.location_sequence}, {"render_version", state.render_version},
            {"location_resolved", tracked.resolved}, {"observed", tracked.observed}, {"warning", tracked.warning},
            {"identity_source", "shell_or_verified_native_observation"}, {"native_guid", tracked.native ? Json(guid_text(tracked.native->guid)) : Json(nullptr)},
            {"title_applied", tracked.controlled.has_value() && tracked.observed && state.refresh == RefreshState::applied && state.applied_render_version == state.render_version},
            {"applied_title", utf8(tracked.applied_title)}, {"apply_error", tracked.bridge_error},
            {"refresh", !tracked.controlled ? "unavailable" : state.refresh == RefreshState::applied ? "applied" : state.refresh == RefreshState::failed ? "failed" : "pending"}});
    }
    return result;
}
Json ShellTabTracker::set_depth(std::string_view id, std::optional<int> depth) {
    auto found = tracked_.find(id);
    if (found == tracked_.end() || !found->second.observed) throw Error(2, "标签已关闭或暂未观察到，无法设置");
    auto before = registry_.get(id);
    auto state = registry_.set_depth(id, DepthOverride{depth}, next_sequence(before.policy_sequence));
    return {{"tab_id", state.id}, {"depth", depth_json(state.effective.display.depth)}, {"source", "tab"},
        {"policy_sequence", state.policy_sequence}, {"policy_saved", true}, {"title_applied", false},
        {"message", "标签策略已记录（仅本次代理运行期）；观察模式下未应用标题。"}};
}
Json ShellTabTracker::reset_depth(std::string_view id) {
    auto found = tracked_.find(id);
    if (found == tracked_.end() || !found->second.observed) throw Error(2, "标签已关闭或暂未观察到，无法重置");
    auto before = registry_.get(id);
    auto state = registry_.reset_depth(id, next_sequence(before.policy_sequence));
    return {{"tab_id", state.id}, {"depth", depth_json(state.effective.display.depth)}, {"source", state.effective.source == PolicySource::rule ? "rule" : "global"},
        {"policy_sequence", state.policy_sequence}, {"policy_saved", true}, {"title_applied", false},
        {"message", "标签策略已恢复继承；观察模式下未应用标题。"}};
}
NativeTab ShellTabTracker::native_tab(std::string_view id) const {
    auto found = tracked_.find(id);
    if (found == tracked_.end() || !found->second.observed) throw Error(2, "标签已关闭或暂未观察到");
    if (!found->second.native) throw Error(3, "当前标签没有通过校验的原生 GUID 映射");
    return *found->second.native;
}
Json ShellTabTracker::synchronize_titles(bool enabled, const TitleTransport& transport) {
    size_t applied = 0, failed = 0, restored = 0;
    for (auto state : registry_.list()) {
        auto& tracked = tracked_.at(state.id);
        if (!enabled && !tracked.controlled) continue;
        try {
            if (!enabled) {
                transport(*tracked.controlled, BridgeAction::restore, policy_);
                tracked.controlled.reset(); tracked.applied_title.clear(); tracked.bridge_error.clear();
                registry_.request_refresh(state.id); ++restored;
            } else {
                if (!tracked.observed) throw Error(3, "标签暂未观察到，未续订标题租约");
                auto native = native_tab(state.id);
                if (state.refresh != RefreshState::pending) state = registry_.request_refresh(state.id);
                // Retain ownership even on an unconfirmed request, so stop attempts restoration.
                tracked.controlled = native;
                auto render_policy = policy_; render_policy.override = state.depth_override;
                auto response = transport(native, BridgeAction::apply, render_policy);
                registry_.acknowledge_render(state.id, state.render_version, true);
                tracked.applied_title = response.title; tracked.bridge_error.clear(); ++applied;
            }
        } catch (const std::exception& error) {
            if (state.refresh != RefreshState::pending) state = registry_.request_refresh(state.id);
            registry_.acknowledge_render(state.id, state.render_version, false, error.what());
            tracked.bridge_error = error.what(); ++failed;
        }
    }
    return {{"applied_count", applied}, {"restored_count", restored}, {"failed_count", failed},
        {"complete", failed == 0}, {"runtime_applied", enabled && applied > 0 && failed == 0}};
}
}
