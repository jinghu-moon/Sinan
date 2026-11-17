#include "policy/tab_registry.hpp"
#include <algorithm>
#include <limits>

namespace tabtail {
namespace {
void validate_identity(const TabIdentity& identity) {
    if (!identity.process_id || !identity.process_started || !identity.frame_id || !identity.frame_generation || !identity.tab_generation ||
        std::all_of(identity.tab_guid.begin(), identity.tab_guid.end(), [](auto byte) { return byte == 0; }))
        throw PolicyError(PolicyErrorCode::invalid_argument, "标签身份缺少进程、frame 或标签生命周期信息");
}
void validate_location(const TabLocation& location) {
    if (location.original_title.find(L'\0') != std::wstring::npos ||
        (location.filesystem_path && (location.filesystem_path->empty() || location.filesystem_path->find(L'\0') != std::wstring::npos)))
        throw PolicyError(PolicyErrorCode::invalid_argument, "标签位置文本为空或含 NUL");
}
void require_newer(uint64_t sequence, uint64_t previous) {
    if (!sequence || sequence <= previous)
        throw PolicyError(PolicyErrorCode::stale_update, "标签更新已过期或重复");
}
void require_render_capacity(const TabSnapshot& state) {
    if (state.render_version == std::numeric_limits<uint64_t>::max())
        throw PolicyError(PolicyErrorCode::counter_exhausted, "标签刷新版本已耗尽");
}
void queue_refresh(TabSnapshot& state) {
    require_render_capacity(state);
    ++state.render_version;
    state.refresh = RefreshState::pending;
    state.refresh_error.clear();
}
}
TabRegistry::TabRegistry(std::string runtime_id, DisplayOptions defaults, uint64_t revision, std::shared_ptr<const RuleSet> rules)
    : runtime_id_(std::move(runtime_id)), defaults_(std::move(defaults)), defaults_revision_(revision), rules_(std::move(rules)) {
    if (runtime_id_.empty() || runtime_id_.size() > 128 || !revision ||
        !std::all_of(runtime_id_.begin(), runtime_id_.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        })) throw PolicyError(PolicyErrorCode::invalid_argument, "代理运行期标识或默认配置版本无效");
    validate_display(defaults_);
}
TabSnapshot& TabRegistry::require_tab(std::string_view id) {
    auto tab = tabs_.find(id);
    if (tab == tabs_.end()) throw PolicyError(PolicyErrorCode::unknown_tab, "标签不存在、已关闭或属于其他代理运行期");
    return tab->second;
}
TabSnapshot TabRegistry::register_tab(const TabIdentity& identity, TabLocation location, uint64_t sequence) {
    validate_identity(identity); validate_location(location);
    if (!sequence) throw PolicyError(PolicyErrorCode::invalid_argument, "标签位置更新序号必须为正整数");
    std::lock_guard lock(mutex_);
    if (auto existing = native_ids_.find(identity); existing != native_ids_.end()) return tabs_.at(existing->second);
    if (retired_.contains(identity)) throw PolicyError(PolicyErrorCode::retired_identity, "已关闭标签的身份不能复用，请使用新的生命周期代次");
    if (next_id_ == std::numeric_limits<uint64_t>::max()) throw PolicyError(PolicyErrorCode::counter_exhausted, "标签标识已耗尽");
    TabSnapshot state;
    state.id = runtime_id_ + "." + std::to_string(next_id_++);
    state.identity = identity;
    state.location = std::move(location);
    state.location_sequence = sequence;
    state.defaults_revision = defaults_revision_;
    state.effective = RuntimePolicy{defaults_, rules_, std::nullopt}.resolve(state.location.filesystem_path);
    auto [tab, inserted] = tabs_.emplace(state.id, state);
    try { native_ids_.emplace(identity, state.id); }
    catch (...) { tabs_.erase(tab); throw; }
    return state;
}
TabSnapshot TabRegistry::get(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto tab = tabs_.find(id);
    if (tab == tabs_.end()) throw PolicyError(PolicyErrorCode::unknown_tab, "标签不存在、已关闭或属于其他代理运行期");
    return tab->second;
}
std::vector<TabSnapshot> TabRegistry::list() const {
    std::lock_guard lock(mutex_);
    std::vector<TabSnapshot> result;
    result.reserve(tabs_.size());
    for (const auto& [id, tab] : tabs_) result.push_back(tab);
    return result;
}
TabSnapshot TabRegistry::update_override(std::string_view id, std::optional<DepthOverride> override, uint64_t sequence) {
    if (override) validate_depth(override->depth);
    std::lock_guard lock(mutex_);
    auto& state = require_tab(id);
    require_newer(sequence, state.policy_sequence);
    auto effective = RuntimePolicy{defaults_, rules_, override}.resolve(state.location.filesystem_path);
    auto next = state;
    if (!same_title_policy(state.effective.display, effective.display) || state.effective.native != effective.native) queue_refresh(next);
    next.depth_override = override;
    next.effective = std::move(effective);
    next.policy_sequence = sequence;
    state = std::move(next);
    return state;
}
TabSnapshot TabRegistry::set_depth(std::string_view id, DepthOverride override, uint64_t sequence) {
    return update_override(id, override, sequence);
}
TabSnapshot TabRegistry::reset_depth(std::string_view id, uint64_t sequence) {
    return update_override(id, std::nullopt, sequence);
}
TabSnapshot TabRegistry::navigate(std::string_view id, TabLocation location, uint64_t sequence) {
    validate_location(location);
    std::lock_guard lock(mutex_);
    auto& state = require_tab(id);
    require_newer(sequence, state.location_sequence);
    auto next = state;
    if (next.location != location) queue_refresh(next);
    next.location = std::move(location);
    next.effective = RuntimePolicy{defaults_, rules_, next.depth_override}.resolve(next.location.filesystem_path);
    next.location_sequence = sequence;
    state = std::move(next);
    return state;
}
std::vector<TabSnapshot> TabRegistry::apply_defaults(DisplayOptions defaults, uint64_t revision, std::shared_ptr<const RuleSet> rules) {
    validate_display(defaults);
    std::lock_guard lock(mutex_);
    bool same_rules = (!rules && !rules_) || (rules && rules_ && rules->definitions() == rules_->definitions()) ||
        (rules && rules->definitions().empty() && !rules_) || (rules_ && rules_->definitions().empty() && !rules);
    if (revision < defaults_revision_ || (revision == defaults_revision_ && (defaults != defaults_ || !same_rules)))
        throw PolicyError(PolicyErrorCode::stale_update, "全局默认配置版本已过期或冲突");
    if (revision == defaults_revision_) return {};
    // Prepare the transaction first so errors cannot leave only some tabs updated.
    auto next_tabs = tabs_;
    std::vector<TabSnapshot> refresh;
    for (auto& [id, state] : next_tabs) {
        auto effective = RuntimePolicy{defaults, rules, state.depth_override}.resolve(state.location.filesystem_path);
        bool changed = !same_title_policy(state.effective.display, effective.display) || state.effective.native != effective.native;
        if (changed) queue_refresh(state);
        state.effective = std::move(effective);
        state.defaults_revision = revision;
        if (changed) refresh.push_back(state);
    }
    rules_ = std::move(rules);
    defaults_ = std::move(defaults);
    defaults_revision_ = revision;
    tabs_.swap(next_tabs);
    return refresh;
}
TabSnapshot TabRegistry::request_refresh(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto& state = require_tab(id);
    queue_refresh(state);
    return state;
}
bool TabRegistry::acknowledge_render(std::string_view id, uint64_t version, bool success, std::string error) {
    std::lock_guard lock(mutex_);
    auto& state = require_tab(id);
    if (version > state.render_version || !version) throw PolicyError(PolicyErrorCode::invalid_argument, "刷新确认引用了未发出的版本");
    if (version != state.render_version || state.refresh != RefreshState::pending) return false;
    state.refresh = success ? RefreshState::applied : RefreshState::failed;
    state.refresh_error = success ? std::string{} : std::move(error);
    if (success) state.applied_render_version = version;
    return true;
}
void TabRegistry::erase_tab(std::map<std::string, TabSnapshot, std::less<>>::iterator tab) {
    retired_.insert(tab->second.identity);
    native_ids_.erase(tab->second.identity);
    tabs_.erase(tab);
}
void TabRegistry::close_tab(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto tab = tabs_.find(id);
    if (tab == tabs_.end()) throw PolicyError(PolicyErrorCode::unknown_tab, "无法关闭未知或已关闭标签");
    erase_tab(tab);
}
size_t TabRegistry::close_frame(uint32_t pid, uint64_t started, uint64_t frame, uint64_t generation) {
    std::lock_guard lock(mutex_);
    size_t count = 0;
    for (auto tab = tabs_.begin(); tab != tabs_.end();) {
        const auto& id = tab->second.identity;
        if (id.process_id == pid && id.process_started == started && id.frame_id == frame && id.frame_generation == generation) {
            auto removed = tab++; erase_tab(removed); ++count;
        } else ++tab;
    }
    return count;
}
size_t TabRegistry::close_process(uint32_t pid, uint64_t started) {
    std::lock_guard lock(mutex_);
    size_t count = 0;
    for (auto tab = tabs_.begin(); tab != tabs_.end();) {
        const auto& id = tab->second.identity;
        if (id.process_id == pid && id.process_started == started) {
            auto removed = tab++; erase_tab(removed); ++count;
        } else ++tab;
    }
    return count;
}
void TabRegistry::clear() {
    std::lock_guard lock(mutex_);
    while (!tabs_.empty()) erase_tab(tabs_.begin());
}
}
