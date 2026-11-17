#include "policy/tab_registry.hpp"
#include "formatter/formatter.hpp"
#include <future>
#include <iostream>
#include <stdexcept>

using namespace sinan;
namespace {
int checks = 0;
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F action, PolicyErrorCode code, const char* message) {
    bool matched = false;
    try { action(); } catch (const PolicyError& e) { matched = e.code() == code; }
    expect(matched, message);
}
TabIdentity identity(uint32_t tab, uint32_t pid = 42, uint64_t process_started = 1000,
    uint64_t frame = 1, uint64_t frame_generation = 1, uint64_t tab_generation = 1) {
    TabIdentity result{1, pid, process_started, frame, frame_generation};
    for (size_t i = 0; i < sizeof(tab); ++i) result.tab_guid[i] = static_cast<uint8_t>(tab >> (8 * i));
    result.tab_generation = tab_generation;
    return result;
}
TabLocation folder(std::wstring path = L"D:\\100_Projects\\110_Daily\\VoiceInput", std::wstring original = L"VoiceInput") {
    return {std::move(path), std::move(original)};
}
void independent_depths() {
    TabRegistry tabs("run-independent");
    auto a = tabs.register_tab(identity(1), folder());
    auto b = tabs.register_tab(identity(2), folder());
    auto c = tabs.register_tab(identity(3), folder());
    auto d = tabs.register_tab(identity(4), folder());
    tabs.set_depth(a.id, DepthOverride{1}, 1);
    tabs.set_depth(b.id, DepthOverride{3}, 1);
    tabs.set_depth(c.id, DepthOverride{std::nullopt}, 1);
    expect(format_tab_title(tabs.get(a.id)).text == L"VoiceInput", "tab A uses one level");
    expect(format_tab_title(tabs.get(b.id)).text == L"100_Projects\\110_Daily\\VoiceInput", "tab B uses three levels");
    expect(format_tab_title(tabs.get(c.id)).text == L"D:\\100_Projects\\110_Daily\\VoiceInput", "tab C explicitly uses all");
    expect(format_tab_title(tabs.get(d.id)).text == L"110_Daily\\VoiceInput", "tab D inherits two");
    auto all = tabs.get(c.id);
    expect(all.depth_override.has_value() && !all.depth_override->depth, "explicit all is an engaged override");
    expect(!tabs.get(d.id).depth_override && tabs.get(d.id).effective.source == PolicySource::global, "inherit has no override");
    auto refresh = tabs.apply_defaults(DisplayOptions{4, L"\\"}, 2);
    expect(refresh.size() == 1 && refresh.front().id == d.id, "global depth only refreshes inherited tab");
    expect(tabs.get(a.id).effective.display.depth == 1 && tabs.get(b.id).effective.display.depth == 3, "global update preserves pins");
    expect(tabs.get(c.id).effective.source == PolicySource::tab && !tabs.get(c.id).effective.display.depth, "all is not inheritance");
    expect(tabs.get(d.id).effective.display.depth == 4, "inherited depth changes");
    tabs.reset_depth(b.id, 2);
    expect(tabs.get(b.id).effective.display.depth == 4 && tabs.get(b.id).effective.source == PolicySource::global, "reset inherits current default");
    expect(tabs.get(a.id).defaults_revision == 2, "all snapshots record latest defaults revision");
    auto another = tabs.register_tab(identity(5), folder());
    expect(another.effective.display.depth == 4 && !another.depth_override, "new tabs inherit updated defaults");
    auto snapshot = tabs.get(a.id);
    snapshot.depth_override = DepthOverride{8};
    snapshot.effective.display.depth = 8;
    expect(tabs.get(a.id).effective.display.depth == 1, "returned snapshot cannot mutate registry");
}
void navigation_and_lifetimes() {
    TabRegistry tabs("run-navigation");
    auto native = identity(1);
    auto a = tabs.register_tab(native, folder());
    tabs.set_depth(a.id, DepthOverride{3}, 1);
    tabs.navigate(a.id, folder(L"D:\\A\\B\\C", L"C"), 2);
    expect(format_tab_title(tabs.get(a.id)).text == L"A\\B\\C", "navigation preserves override");
    tabs.navigate(a.id, {std::nullopt, L"回收站"}, 3);
    auto virtual_title = format_tab_title(tabs.get(a.id));
    expect(virtual_title.text == L"回收站" && !virtual_title.filesystem, "virtual folder keeps Shell title");
    expect(tabs.get(a.id).depth_override->depth == 3, "virtual navigation retains override");
    tabs.navigate(a.id, folder(L"D:\\A\\B\\Renamed", L"Renamed"), 4);
    expect(format_tab_title(tabs.get(a.id)).text == L"A\\B\\Renamed", "return/rename uses same override");
    tabs.navigate(a.id, folder(L"D:\\", L"数据盘"), 5);
    expect(format_tab_title(tabs.get(a.id)).text == L"数据盘", "root uses original label");
    tabs.close_tab(a.id);
    rejects([&] { tabs.get(a.id); }, PolicyErrorCode::unknown_tab, "closed tab id invalid");
    rejects([&] { tabs.set_depth(a.id, DepthOverride{1}, 2); }, PolicyErrorCode::unknown_tab, "closed tab request rejected");
    rejects([&] { tabs.acknowledge_render(a.id, 1, true); }, PolicyErrorCode::unknown_tab, "late ACK for closed tab rejected");
    rejects([&] { tabs.register_tab(native, folder()); }, PolicyErrorCode::retired_identity, "native identity reuse requires new lifetime");
    ++native.tab_generation;
    auto reopened = tabs.register_tab(native, folder());
    expect(reopened.id != a.id && !reopened.depth_override && reopened.effective.display.depth == 2, "reopened same path has fresh id/defaults");
    TabRegistry new_runtime("run-next");
    auto next = new_runtime.register_tab(native, folder());
    expect(next.id != reopened.id, "restart namespaces ids by runtime epoch");
    rejects([&] { new_runtime.reset_depth(reopened.id, 10); }, PolicyErrorCode::unknown_tab, "previous runtime token rejected");
    tabs.clear();
    expect(tabs.list().empty(), "clear removes all live state");
    rejects([&] { tabs.get(reopened.id); }, PolicyErrorCode::unknown_tab, "clear invalidates token");
}
void request_ordering_and_refresh() {
    TabRegistry tabs("run-ordering");
    auto a = tabs.register_tab(identity(1), folder());
    expect(a.refresh == RefreshState::pending && a.applied_render_version == 0, "creation is not falsely marked rendered");
    expect(tabs.acknowledge_render(a.id, a.render_version, true), "initial render acknowledged");
    tabs.set_depth(a.id, DepthOverride{1}, 10);
    auto policy = tabs.get(a.id);
    rejects([&] { tabs.set_depth(a.id, DepthOverride{3}, 9); }, PolicyErrorCode::stale_update, "old policy cannot replace newer");
    rejects([&] { tabs.reset_depth(a.id, 10); }, PolicyErrorCode::stale_update, "duplicate sequence cannot change operation");
    expect(tabs.get(a.id).effective.display.depth == 1, "rejected requests preserve policy");
    auto nav = tabs.navigate(a.id, folder(L"D:\\A\\B", L"B"), 2);
    rejects([&] { tabs.navigate(a.id, folder(), 1); }, PolicyErrorCode::stale_update, "old navigation cannot restore old path");
    expect(!tabs.acknowledge_render(a.id, policy.render_version, true), "ACK for old render does not acknowledge new location");
    expect(tabs.acknowledge_render(a.id, nav.render_version, false, "refresh failed"), "failure acknowledged");
    auto failed = tabs.get(a.id);
    expect(failed.refresh == RefreshState::failed && failed.refresh_error == "refresh failed", "refresh error is observable");
    expect(failed.applied_render_version == a.render_version, "failure does not advance successful render");
    expect(!tabs.acknowledge_render(a.id, nav.render_version, true), "duplicate ACK does not rewrite outcome");
    auto retry = tabs.request_refresh(a.id);
    expect(retry.render_version > nav.render_version && retry.refresh == RefreshState::pending, "retry has new render version");
    expect(tabs.acknowledge_render(a.id, retry.render_version, true), "retry succeeds");
    expect(tabs.get(a.id).refresh_error.empty(), "successful retry clears error");
    rejects([&] { tabs.acknowledge_render(a.id, retry.render_version + 1, true); }, PolicyErrorCode::invalid_argument, "future ACK rejected");
    auto changed = tabs.apply_defaults(DisplayOptions{4, L"\\"}, 3);
    expect(changed.empty(), "pinned tab doesn't redraw on unused global depth change");
    rejects([&] { tabs.apply_defaults(DisplayOptions{2, L"\\"}, 2); }, PolicyErrorCode::stale_update, "old global snapshot rejected");
    rejects([&] { tabs.apply_defaults(DisplayOptions{5, L"\\"}, 3); }, PolicyErrorCode::stale_update, "same global revision conflicting values rejected");
    expect(tabs.apply_defaults(DisplayOptions{4, L"\\"}, 3).empty(), "identical global snapshot is idempotent");
    tabs.reset_depth(a.id, 11);
    expect(tabs.get(a.id).effective.display.depth == 4, "global and per-tab sequences are independent");
}
void separator_and_metadata() {
    TabRegistry tabs("run-separator");
    auto a = tabs.register_tab(identity(1), folder());
    auto b = tabs.register_tab(identity(2), folder());
    tabs.set_depth(a.id, DepthOverride{std::nullopt}, 1);
    auto full_before = tabs.get(a.id);
    auto changes = tabs.apply_defaults(DisplayOptions{2, L" · "}, 2);
    expect(changes.size() == 1 && changes.front().id == b.id, "separator refresh excludes full-path tab");
    expect(format_tab_title(tabs.get(b.id)).text == L"110_Daily · VoiceInput", "global separator reaches inherited tab");
    expect(tabs.get(a.id).render_version == full_before.render_version, "irrelevant separator doesn't bump render version");
    tabs.set_depth(b.id, DepthOverride{2}, 1);
    auto explicit_before = tabs.get(b.id);
    expect(explicit_before.effective.source == PolicySource::tab, "equal depth still becomes explicit pin");
    tabs.apply_defaults(DisplayOptions{3, L" · "}, 3);
    expect(tabs.get(b.id).effective.display.depth == 2, "equal-to-default pin protects future changes");
    tabs.reset_depth(b.id, 2);
    expect(tabs.get(b.id).effective.display.depth == 3, "reset removes equal-to-default pin");
}
void native_identity_isolation() {
    TabRegistry tabs("run-identities");
    auto a = tabs.register_tab(identity(1), folder());
    tabs.set_depth(a.id, DepthOverride{1}, 1);
    auto duplicate = tabs.register_tab(identity(1), folder(L"D:\\Wrong", L"Wrong"), 99);
    expect(duplicate.id == a.id && duplicate.depth_override->depth == 1, "duplicate discovery preserves overrides");
    expect(duplicate.location.filesystem_path == folder().filesystem_path, "discovery doesn't overwrite navigation state");
    auto second_process = tabs.register_tab(identity(1, 43), folder());
    auto reused_pid = tabs.register_tab(identity(1, 42, 2000), folder());
    auto second_frame = tabs.register_tab(identity(1, 42, 1000, 2), folder());
    auto reused_frame = tabs.register_tab(identity(1, 42, 1000, 1, 2), folder());
    expect(tabs.list().size() == 5, "same GUID in different native lifetimes stays distinct");
    expect(tabs.close_frame(42, 1000, 1, 1) == 1, "close frame matches generation");
    expect(tabs.get(reused_frame.id).effective.source == PolicySource::global, "old frame close doesn't touch reused frame");
    expect(tabs.close_process(42, 1000) == 2, "close process removes only its frames");
    expect(tabs.get(reused_pid.id).identity.process_started == 2000, "reused PID unaffected");
    expect(tabs.get(second_process.id).identity.process_id == 43, "other process unaffected");
    expect(tabs.close_process(42, 1000) == 0, "duplicate process teardown idempotent");
    (void)second_frame;
}
void validation_is_atomic() {
    rejects([] { TabRegistry bad(""); }, PolicyErrorCode::invalid_argument, "empty runtime id rejected");
    rejects([] { TabRegistry bad("unsafe.id"); }, PolicyErrorCode::invalid_argument, "runtime id token syntax validated");
    TabRegistry tabs("run-validation");
    rejects([&] { tabs.register_tab(TabIdentity{}, folder()); }, PolicyErrorCode::invalid_argument, "incomplete native identity rejected");
    auto a = tabs.register_tab(identity(1), folder());
    auto before = tabs.get(a.id);
    rejects([&] { tabs.set_depth(a.id, DepthOverride{0}, 1); }, PolicyErrorCode::invalid_argument, "invalid tab depth rejected");
    rejects([&] { tabs.apply_defaults(DisplayOptions{33, L"\\"}, 2); }, PolicyErrorCode::invalid_argument, "invalid global depth rejected");
    rejects([&] { tabs.apply_defaults(DisplayOptions{2, L"bad"}, 2); }, PolicyErrorCode::invalid_argument, "invalid global separator rejected");
    rejects([&] { tabs.navigate(a.id, {std::wstring{}, L"bad"}, 2); }, PolicyErrorCode::invalid_argument, "empty filesystem path rejected");
    expect(tabs.get(a.id).policy_sequence == before.policy_sequence && tabs.get(a.id).render_version == before.render_version, "invalid inputs do not mutate state");
    tabs.set_depth(a.id, DepthOverride{1}, 1);
    expect(tabs.get(a.id).policy_sequence == 1, "invalid request did not consume sequence");
}
void concurrent_updates() {
    TabRegistry tabs("run-concurrent");
    std::vector<std::future<void>> jobs;
    for (uint32_t worker = 0; worker < 8; ++worker) {
        jobs.push_back(std::async(std::launch::async, [&, worker] {
            for (uint32_t i = 1; i <= 25; ++i) {
                auto tab = tabs.register_tab(identity(worker * 25 + i), folder());
                tabs.set_depth(tab.id, DepthOverride{1}, 1);
                tabs.navigate(tab.id, folder(L"D:\\A\\B", L"B"), 2);
                (void)tabs.list();
            }
        }));
    }
    for (auto& job : jobs) job.get();
    expect(tabs.list().size() == 200, "concurrent discovery doesn't lose tabs");
    expect(tabs.apply_defaults(DisplayOptions{3, L"\\"}, 2).empty(), "all concurrently pinned tabs retain depth");
    for (auto tab : tabs.list()) expect(format_tab_title(tab).text == L"B", "concurrent paths/policies isolated");
    auto a = tabs.list().front();
    jobs.clear();
    for (uint64_t sequence = 2; sequence <= 64; ++sequence) {
        jobs.push_back(std::async(std::launch::async, [&, sequence] {
            try { tabs.set_depth(a.id, DepthOverride{static_cast<int>(sequence % 3 + 1)}, sequence); }
            catch (const PolicyError& e) { if (e.code() != PolicyErrorCode::stale_update) throw; }
        }));
    }
    for (auto& job : jobs) job.get();
    expect(tabs.get(a.id).policy_sequence == 64 && tabs.get(a.id).effective.display.depth == 2, "out-of-order concurrent requests converge to latest sequence");
}
}
int main() {
    try {
        independent_depths(); navigation_and_lifetimes(); request_ordering_and_refresh();
        separator_and_metadata(); native_identity_isolation(); validation_is_atomic(); concurrent_updates();
        std::cout << checks << " policy checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
