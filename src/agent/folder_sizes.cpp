#include "agent/folder_sizes.hpp"
#include "agent/folder_size_hover.hpp"
#include "hook/identity.hpp"
#include "platform/folder_size_cache.hpp"
#include "platform/folder_size_query.hpp"
#include "platform/shell_tabs.hpp"
#include <sddl.h>
#include <shlobj.h>
#include <propkey.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace sinan {
using namespace folder_size_ipc;
namespace {
void notify_shell(const std::wstring& path, LONG event = SHCNE_UPDATEITEM) {
    if (path.size() < MAX_PATH) {
        SHChangeNotify(event, SHCNF_PATHW, path.c_str(), nullptr);
        return;
    }
    PIDLIST_ABSOLUTE item{};
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr))) {
        SHChangeNotify(event, SHCNF_IDLIST, item, nullptr);
        CoTaskMemFree(item);
    }
}

const char* index_status(IndexStatus status) {
    switch (status) {
    case IndexStatus::ready: return "ready";
    case IndexStatus::loading: return "loading";
    case IndexStatus::incompatible: return "incompatible";
    default: return "disconnected";
    }
}

std::wstring_view parent_directory(std::wstring_view path) {
    const auto slash = path.rfind(L'\\');
    return slash == path.npos ? std::wstring_view{} : path.substr(0, slash == 2 ? 3 : slash);
}

bool same_path(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() && (left.empty() ||
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                             static_cast<int>(right.size()), TRUE) == CSTR_EQUAL);
}

bool within_directory(std::wstring_view directory, std::wstring_view path) {
    return !directory.empty() && path.size() >= directory.size() && same_path(directory, path.substr(0, directory.size())) &&
        (path.size() == directory.size() || directory.back() == L'\\' || path[directory.size()] == L'\\');
}

struct Channel {
    NativeTab tab;
    std::unique_ptr<Handle> mapping;
    Cache* cache{};
    bool active = false;
    bool numeric_sort = false;
    std::atomic<bool> recheck_all = false;
    std::string error;
    std::vector<std::wstring> roots;

    explicit Channel(const NativeTab& target) : tab(target) {
        const auto name = cache_name(GetCurrentProcessId(), bridge_process_started(GetCurrentProcess()), target.process_id);
        PSECURITY_DESCRIPTOR descriptor{};
        const auto sddl = L"D:P(A;;GA;;;" + current_sid() + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                  &descriptor, nullptr))
            throw Error(4, system_error("创建文件夹大小缓存权限"));
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        const auto handle = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE,
                                               0, sizeof(Cache), name.c_str());
        const auto error_code = GetLastError();
        LocalFree(descriptor);
        mapping = std::make_unique<Handle>(handle);
        if (!*mapping || error_code == ERROR_ALREADY_EXISTS)
            throw Error(4, "无法创建独立的 Explorer 文件夹大小缓存");
        cache = static_cast<Cache*>(MapViewOfFile(mapping->get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Cache)));
        if (!cache) throw Error(5, system_error("映射 Explorer 文件夹大小缓存"));
    }
    ~Channel() { if (cache) UnmapViewOfFile(cache); }
};

struct Pending {
    unsigned slot;
    uint64_t queried;
    bool urgent;
    std::wstring path;
};

std::vector<Pending> due_entries(Channel& channel, uint64_t now, const std::vector<std::wstring>& roots,
                                 JournalStatus journal) {
    auto& cache = *channel.cache;
    std::vector<Pending> result;
    CacheLock lock(&cache);
    if (!lock || cache.count > cache_capacity) return result;
    const auto recheck_all = channel.recheck_all.exchange(false);
    for (unsigned i = 0; i < cache.count; ++i) {
        auto& entry = cache.entries[i];
        if (recheck_all) entry.dirty = true;
        const std::wstring_view path(entry.path);
        const auto parent = parent_directory(path);
        if (!std::ranges::any_of(roots, [&](const auto& root) { return same_path(root, parent); })) continue;
        const auto elapsed = now > entry.requested ? now - entry.requested : 0;
        const auto fallback_interval = elapsed < 30000 ? 650 : 3000;
        const auto interval = journal == JournalStatus::listening && entry.bytes != unknown ? 30000 : fallback_interval;
        if (entry.dirty || !entry.queried || now - entry.queried >= interval)
            result.push_back({i, entry.queried, entry.dirty || !entry.queried, std::wstring(path)});
    }
    std::stable_sort(result.begin(), result.end(), [](const Pending& a, const Pending& b) {
        if (a.urgent != b.urgent) return a.urgent;
        return a.queried < b.queried;
    });
    if (result.size() > max_batch) result.resize(max_batch);
    return result;
}
}

struct FolderSizes::State {
    std::map<DWORD, std::shared_ptr<Channel>> channels;
    FolderSizeSettings settings;
    mutable std::mutex mutex;
    std::condition_variable wake;
    struct Target {
        std::shared_ptr<Channel> channel;
        std::vector<std::wstring> roots;
    };
    std::vector<Target> active;
    std::vector<std::wstring> refresh_roots;
    std::set<std::pair<DWORD, std::wstring>> resort_roots;
    std::string attach_error;
    std::wstring instance;
    uint64_t generation{};
    IndexStatus index{};
    JournalStatus journal_status{};
    uint64_t journal_rechecks{};
    std::string query_error;
    FolderSizeHover hover;
    std::jthread thread;

    State() : thread([this](std::stop_token stop) { run(stop); }) {}
    ~State() {
        hover.update({}, false);
        thread.request_stop();
        wake.notify_all();
        thread.join();
        for (auto& [_, channel] : channels) {
            InterlockedExchange64(&channel->cache->expires, 0);
            invalidate(*channel);
        }
    }

    void reset_connection() {
        std::lock_guard lock(mutex);
        active.clear();
        instance = settings.everything_instance;
        ++generation;
        index = IndexStatus::disconnected;
        journal_status = JournalStatus::unavailable;
        query_error.clear();
        refresh_roots.clear();
        resort_roots.clear();
        wake.notify_all();
    }

    void update() {
        std::lock_guard lock(mutex);
        active.clear();
        for (auto& [_, channel] : channels)
            if (channel->active && !channel->roots.empty()) active.push_back({channel, channel->roots});
        wake.notify_all();
    }

    static void invalidate(Channel& channel, IndexStatus status = IndexStatus::disconnected, bool clear_time = true) {
        std::vector<std::wstring> paths;
        {
            CacheLock lock(channel.cache);
            if (!lock || channel.cache->count > cache_capacity) return;
            channel.cache->index = status;
            for (unsigned i = 0; i < channel.cache->count; ++i) {
                auto& entry = channel.cache->entries[i];
                if (entry.bytes != unknown) paths.emplace_back(entry.path);
                entry.bytes = unknown;
                if (clear_time) entry.queried = entry.successful_at = 0;
            }
        }
        for (const auto& path : paths) notify_shell(path);
    }

    void run(std::stop_token stop) {
        FolderSizeQuery query;
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_lock lock(mutex);
        uint64_t last_probe = 0;
        uint64_t query_epoch = 0;
        auto journal = JournalStatus::unavailable;
        while (!stop.stop_requested()) {
            if (active.empty()) {
                query.reset();
                last_probe = 0;
                journal = journal_status = JournalStatus::unavailable;
                wake.wait(lock, [&] { return stop.stop_requested() || !active.empty(); });
                continue;
            }
            const auto targets = active;
            const auto epoch = generation;
            const auto current_instance = instance;
            const auto refresh = std::exchange(refresh_roots, {});
            lock.unlock();
            if (query_epoch != epoch) {
                query.reset();
                query_epoch = epoch;
                last_probe = 0;
                journal = JournalStatus::unavailable;
            }
            for (const auto& root : refresh) notify_shell(root, SHCNE_UPDATEDIR);
            auto delay = 100;
            for (const auto& [channel, roots] : targets) {
                if (stop.stop_requested()) break;
                auto& cache = *channel->cache;
                const auto now = GetTickCount64();
                auto pending = due_entries(*channel, now, roots, journal);
                if (pending.empty() && last_probe && now - last_probe < 2000 && !query.journal_pending()) continue;
                last_probe = now;
                std::vector<std::wstring> paths;
                for (const auto& item : pending) paths.push_back(item.path);
                std::vector<std::wstring> changed;
                auto status = IndexStatus::disconnected;
                std::string failure;
                try {
                    const auto result = query.query(current_instance, paths);
                    status = result.status;
                    journal = result.journal_status;
                    if (status == IndexStatus::incompatible)
                        failure = "当前 Everything 版本不支持文件夹大小查询。";
                    std::lock_guard state_lock(mutex);
                    if (generation != epoch) break;
                    if (GetTickCount64() >= static_cast<uint64_t>(InterlockedCompareExchange64(&cache.expires, 0, 0))) continue;
                    {
                        CacheLock cache_lock(&cache);
                        if (cache_lock) {
                            FILETIME timestamp{};
                            GetSystemTimeAsFileTime(&timestamp);
                            const auto completed = (uint64_t{timestamp.dwHighDateTime} << 32) | timestamp.dwLowDateTime;
                            cache.index = status;
                            for (size_t i = 0; i < pending.size(); ++i) {
                                auto& entry = cache.entries[pending[i].slot];
                                if (pending[i].path != entry.path) continue;
                                const auto bytes = status == IndexStatus::ready ? result.bytes[i] : unknown;
                                if (entry.bytes != bytes) changed.push_back(pending[i].path);
                                entry.bytes = bytes;
                                entry.dirty = false;
                                entry.queried = GetTickCount64();
                                if (bytes != unknown) entry.successful_at = completed;
                            }
                        }
                    }
                    if (result.journal_rescan || !result.journal_paths.empty()) {
                        for (const auto& target : targets) {
                            auto& other = *target.channel;
                            CacheLock cache_lock(other.cache);
                            if (!cache_lock || other.cache->count > cache_capacity) {
                                other.recheck_all = true;
                                continue;
                            }
                            for (unsigned i = 0; i < other.cache->count; ++i) {
                                auto& entry = other.cache->entries[i];
                                const std::wstring_view path(entry.path);
                                if (entry.dirty) continue;
                                if (result.journal_rescan || std::ranges::any_of(result.journal_paths, [&](const auto& changed_path) {
                                    return within_directory(path, changed_path) || within_directory(changed_path, path);
                                })) {
                                    entry.dirty = true;
                                    ++journal_rechecks;
                                }
                            }
                        }
                    }
                } catch (const std::exception& error) {
                    failure = error.what();
                    journal = JournalStatus::unavailable;
                }
                {
                    std::lock_guard state_lock(mutex);
                    if (generation != epoch) break;
                    index = status;
                    journal_status = journal;
                    query_error = std::move(failure);
                    for (const auto& path : changed) {
                        if (resort_roots.size() >= cache_capacity) break;
                        resort_roots.emplace(channel->tab.process_id, parent_directory(path));
                    }
                }
                if (status != IndexStatus::ready) {
                    for (const auto& target : targets) invalidate(*target.channel, status, false);
                    delay = 1000;
                }
                for (const auto& path : changed) notify_shell(path);
                if (delay > 100) break;
            }
            lock.lock();
            wake.wait_for(lock, std::chrono::milliseconds(delay));
        }
        lock.unlock();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

FolderSizes::FolderSizes() = default;
FolderSizes::~FolderSizes() = default;

void FolderSizes::synchronize(const FolderSizeSettings& settings, const std::vector<NativeTab>& windows,
                              const Json& tabs, ShellTabEnumerator& enumerator, bool chinese) {
    if (!settings.enabled && !state_) return;
    if (!state_) state_ = std::make_unique<State>();
    auto& state = *state_;
    const auto old_settings = std::exchange(state.settings, settings);
    const bool connection_changed = old_settings.enabled != settings.enabled ||
        old_settings.everything_instance != settings.everything_instance;
    if (connection_changed) state.reset_connection();
    for (auto it = state.channels.begin(); it != state.channels.end();) {
        auto& channel = *it->second;
        if (process_lifetime(channel.tab.process_id, channel.tab.process_started) == ProcessLifetime::ended)
            it = state.channels.erase(it);
        else ++it;
    }
    if (!settings.enabled) {
        state.hover.update({}, chinese);
        for (auto& [_, channel] : state.channels) {
            InterlockedExchange64(&channel->cache->expires, 0);
            channel->active = false;
            State::invalidate(*channel);
        }
        state.update();
        return;
    }
    state.attach_error.clear();
    std::set<std::pair<DWORD, std::wstring>> resorts;
    {
        std::lock_guard lock(state.mutex);
        resorts.swap(state.resort_roots);
    }
    std::map<DWORD, std::vector<std::wstring>> roots;
    for (const auto& tab : tabs) {
        if (!tab.value("observed", false) || !tab.value("location_resolved", false) ||
            !tab.at("path").is_string()) continue;
        const auto browser = reinterpret_cast<HWND>(tab.at("browser_window").get<uintptr_t>());
        if (!IsWindowVisible(browser)) continue;
        auto path = wide(tab.at("path").get<std::string>());
        if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') continue;
        if (path.size() > 3 && path.back() == L'\\') path.pop_back();
        const auto pid = tab.at("pid").get<DWORD>();
        const auto channel = state.channels.find(pid);
        if (channel != state.channels.end() && channel->second->active &&
            channel->second->numeric_sort && resorts.contains({pid, path})) try {
            auto view = enumerator.view(reinterpret_cast<HWND>(tab.at("window").get<uintptr_t>()), browser, false);
            Microsoft::WRL::ComPtr<IFolderView2> folder;
            if (SUCCEEDED(view.As(&folder))) {
                int count{};
                if (SUCCEEDED(folder->GetSortColumnCount(&count)) && count > 0 && count <= 16) {
                    std::vector<SORTCOLUMN> columns(static_cast<size_t>(count));
                    if (SUCCEEDED(folder->GetSortColumns(columns.data(), count)) &&
                        columns.front().propkey == PKEY_Size)
                        folder->SetSortColumns(columns.data(), count);
                }
            }
        } catch (const Error&) {}
        auto& paths = roots[pid];
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(std::move(path));
    }
    for (auto& [pid, channel] : state.channels)
        if (!roots.contains(pid)) channel->roots.clear();
    std::set<DWORD> visited;
    for (const auto& tab : windows) {
        if (!roots.contains(tab.process_id) || !visited.insert(tab.process_id).second) continue;
        auto& channel = state.channels[tab.process_id];
        try {
            if (!channel) channel = std::make_shared<Channel>(tab);
            const auto& visible_roots = roots.at(tab.process_id);
            if (std::ranges::any_of(visible_roots, [&](const auto& root) {
                return !std::ranges::any_of(channel->roots, [&](const auto& previous) { return same_path(root, previous); });
            })) channel->recheck_all = true;
            channel->roots = visible_roots;
            const auto was_active = channel->active;
            const auto report = send_layout_payload(tab, LayoutAction::folder_sizes, {},
                                                     folder_sizes_json(settings, true));
            channel->active = report.value("active", false);
            channel->numeric_sort = report.value("numeric_sort", false);
            channel->error = report.value("error", std::string{});
            if (channel->active) {
                InterlockedExchange64(&channel->cache->expires, GetTickCount64() + lease_duration_ms);
                if (connection_changed) State::invalidate(*channel);
                std::lock_guard lock(state.mutex);
                for (const auto& path : channel->roots)
                    if (!was_active || old_settings != settings) {
                        if (std::find(state.refresh_roots.begin(), state.refresh_roots.end(), path) ==
                            state.refresh_roots.end())
                            state.refresh_roots.push_back(path);
                    }
            }
        } catch (const std::exception& error) {
            state.attach_error = error.what();
            if (channel) channel->active = false;
        }
        if (!channel) state.channels.erase(tab.process_id);
    }
    std::vector<FolderSizeHover::Target> hover_targets;
    for (const auto& tab : tabs) {
        if (!tab.value("observed", false) || !tab.value("location_resolved", false) || !tab.at("path").is_string())
            continue;
        const auto browser = reinterpret_cast<HWND>(tab.at("browser_window").get<uintptr_t>());
        const auto frame = reinterpret_cast<HWND>(tab.at("window").get<uintptr_t>());
        const auto channel = state.channels.find(tab.at("pid").get<DWORD>());
        if (!IsWindowVisible(browser) || channel == state.channels.end() || !channel->second->active ||
            channel->second->roots.empty()) continue;
        hover_targets.push_back({frame, browser, {channel->second, channel->second->cache}});
    }
    state.hover.update(std::move(hover_targets), chinese);
    state.update();
}

Json FolderSizes::status() const {
    if (!state_ || !state_->settings.enabled) return {{"active", false}};
    const auto& state = *state_;
    std::lock_guard lock(state.mutex);
    std::string attach_error = state.attach_error;
    bool ready = false;
    bool numeric_sort = false;
    for (const auto& [_, channel] : state.channels) {
        ready = ready || (channel->active && !channel->roots.empty());
        numeric_sort = numeric_sort || (channel->active && channel->numeric_sort && !channel->roots.empty());
        if (attach_error.empty() && !channel->roots.empty()) attach_error = channel->error;
    }
    const auto& error = ready ? state.query_error : attach_error;
    return {{"active", ready && state.index == IndexStatus::ready}, {"pending", !ready && error.empty()},
            {"index", index_status(state.index)},
            {"journal", state.journal_status == JournalStatus::listening ? "listening" : "unavailable"},
            {"journal_rechecks", state.journal_rechecks}, {"numeric_sort", numeric_sort}, {"error", error}};
}
}
