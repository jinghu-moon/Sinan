#include "agent/selection_summary.hpp"
#include "hook/layout.hpp"
#include "platform/folder_size_query.hpp"
#include "platform/shell_actions.hpp"
#include <algorithm>
#include <future>
#include <map>
#include <utility>

namespace sinan {
using Microsoft::WRL::ComPtr;
using namespace folder_size_ipc;

namespace {
struct Selection {
    uint32_t count{};
    uint32_t files{};
    uint32_t folders{};
    std::vector<std::wstring> paths;
    std::vector<ItemKind> kinds;
    bool operator==(const Selection&) const = default;
};

Selection capture(IShellView& view, const SelectionSummarySettings& settings) {
    Selection selection;
    DWORD count{};
    const auto items = shell_selection(view, count);
    selection.count = count;
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IShellItem> item;
        SFGAOF attributes{};
        if (FAILED(items->GetItemAt(index, &item)) || !item ||
            FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes)) ||
            !(attributes & SFGAO_FILESYSTEM)) continue;
        const bool folder = (attributes & SFGAO_FOLDER) != 0;
        if (folder) ++selection.folders;
        else ++selection.files;
        if (!settings.show_size) continue;
        PWSTR path{};
        const auto result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, CoTaskMemFree);
        if (FAILED(result) || !path || !*path || wcsnlen_s(path, max_path) == max_path) continue;
        selection.paths.emplace_back(path);
        selection.kinds.push_back(folder ? ItemKind::folder : ItemKind::file);
    }
    return selection;
}

std::wstring summarize(const Selection& selection, const SelectionSummarySettings& options,
               const FolderSizeSettings& sizes, bool chinese) {
    std::wstring text;
    auto append = [&](std::wstring line) {
        if (!text.empty()) text += L"  ·  ";
        text += line;
    };
    if (selection.count) {
        if (options.show_files) {
            append((chinese ? L"文件 " : L"Files ") + std::to_wstring(selection.files));
        }
        if (options.show_folders) {
            append((chinese ? L"文件夹 " : L"Folders ") + std::to_wstring(selection.folders));
        }
        if (options.show_size) {
            FolderSizeQuery query(false);
            uint64_t bytes{};
            uint32_t unknown_sizes = selection.count;
            bool overflow = false;
            const auto deadline = GetTickCount64() + 2000;
            for (size_t offset = 0; offset < selection.paths.size() && GetTickCount64() < deadline; offset += max_batch) {
                const auto count = std::min<size_t>(max_batch, selection.paths.size() - offset);
                try {
                    const auto batch = query.query(sizes.everything_instance, {selection.paths.data() + offset, count},
                                                   {selection.kinds.data() + offset, count});
                    for (auto value : batch.bytes) {
                        if (value == unknown) continue;
                        --unknown_sizes;
                        if (UINT64_MAX - bytes < value) overflow = true;
                        else if (!overflow) bytes += value;
                    }
                } catch (const Error&) {
                    // Keep any completed batches when a selected path or the index does not respond.
                    break;
                }
            }
            const bool any_size = unknown_sizes < selection.count;
            std::wstring total = L"—";
            if (overflow) total = chinese ? L"超出可显示范围" : L"Out of range";
            else if (any_size) {
                wchar_t decimal[12]{};
                if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, std::size(decimal)))
                    decimal[0] = L'.';
                total = format_folder_size(bytes, sizes, decimal);
            }
            const auto label = unknown_sizes && any_size ? (chinese ? L"已知小计 " : L"Known subtotal ")
                                                         : (chinese ? L"大小 " : L"Size ");
            append(label + total);
            if (unknown_sizes) append(chinese ? std::to_wstring(unknown_sizes) + L" 项大小未知"
                                              : L"Unknown sizes " + std::to_wstring(unknown_sizes));
        }
    }
    return text;
}
}

struct SelectionSummary::State {
    struct Target {
        NativeTab frame;
        HWND surface{};
        uint64_t due{};
        uint64_t generation{};
        uint64_t selection_revision{};
        bool applied{};
        std::string error;
        std::wstring text;
        std::optional<Selection> selection;
        uint64_t verify_at{};
        unsigned verifies{};
    };
    struct Job {
        HWND frame{};
        HWND surface{};
        uint64_t generation{};
        std::future<std::wstring> result;
    };

    static thread_local State* observing;
    HWINEVENTHOOK selection_events{};
    HWINEVENTHOOK show_events{};
    std::map<HWND, Target> targets;
    std::optional<Job> pending;
    SelectionSummarySettings options;
    FolderSizeSettings sizes;
    bool chinese{};
    uint64_t revision{};

    static HWND visible_surface(HWND frame) {
        HWND browser{};
        while ((browser = FindWindowExW(frame, browser, L"ShellTabWindowClass", nullptr))) {
            if (!IsWindowVisible(browser)) continue;
            const auto surface = FindWindowExW(browser, nullptr, L"DUIViewWndClassName", nullptr);
            if (surface && IsWindowVisible(surface)) return surface;
        }
        return nullptr;
    }

    void schedule(Target& target) {
        target.due = GetTickCount64() + 180;
        target.generation = ++revision;
        target.verify_at = 0;
        target.selection.reset();
    }

    static void CALLBACK changed(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG, DWORD, DWORD) {
        if (!observing || !window) return;
        const auto found = observing->targets.find(GetAncestor(window, GA_ROOT));
        if (found == observing->targets.end()) return;
        if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE) {
            if (object != OBJID_WINDOW) return;
            wchar_t name[64]{};
            GetClassNameW(window, name, static_cast<int>(std::size(name)));
            const std::wstring_view type(name);
            if (type != L"ShellTabWindowClass" && type != L"DUIViewWndClassName") return;
        } else if (!found->second.surface ||
                   (window != found->second.surface && !IsChild(found->second.surface, window))) {
            return;
        }
        observing->schedule(found->second);
        found->second.selection_revision=observing->revision;
    }

    bool write(const Target& target, HWND surface, std::wstring_view text) {
        if (!surface || !IsChild(target.frame.window, surface)) return false;
        auto view = target.frame;
        view.window = surface;
        const auto report = send_layout_payload(view, LayoutAction::selection_status, {}, {{"text", utf8(text)}});
        if (report.contains("error")) throw Error(5, report.at("error").get<std::string>());
        return report.value("active", false);
    }

    void display(Target& target, std::wstring text) {
        target.applied = write(target, target.surface, text) || target.applied;
        target.text = std::move(text);
        // The shell populates this field asynchronously after selection.
        // Reuse the computed summary while those property updates settle.
        target.verify_at = GetTickCount64() + 350;
        target.verifies = 2;
        target.error.clear();
    }

    void restore(Target& target) {
        if (!target.applied) return;
        try { write(target, target.surface, {}); } catch (const std::exception&) {}
        target.applied = false;
    }

    void refresh_surface(Target& target) {
        const auto surface = visible_surface(target.frame.window);
        if (target.surface == surface) return;
        restore(target);
        target.surface = surface;
        schedule(target);
    }
};

thread_local SelectionSummary::State* SelectionSummary::State::observing = nullptr;

SelectionSummary::SelectionSummary() : state_(std::make_unique<State>()) {}
SelectionSummary::~SelectionSummary() { stop(); }

void SelectionSummary::synchronize(const std::vector<NativeTab>& windows, const Config& config, bool chinese) {
    auto& state = *state_;
    const auto& options = config.selection_summary;
    const bool summarize=options.show_files||options.show_folders||options.show_size;
    if (!summarize && !config.command_bar.enabled) {
        stop();
        return;
    }
    const bool changed = state.options != options || state.sizes != config.folder_sizes || state.chinese != chinese;
    state.options = options;
    state.sizes = config.folder_sizes;
    state.chinese = chinese;
    if (!windows.empty()) {
        State::observing = &state;
        if (!state.selection_events)
            state.selection_events = SetWinEventHook(EVENT_OBJECT_SELECTION, EVENT_OBJECT_SELECTIONWITHIN,
                nullptr, State::changed, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!state.show_events)
            state.show_events = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
                nullptr, State::changed, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    const auto foreground = GetForegroundWindow();
    for (const auto& frame : windows) {
        auto [it, inserted] = state.targets.try_emplace(frame.window, State::Target{frame});
        auto& target = it->second;
        if (target.frame.process_started != frame.process_started) target = State::Target{frame};
        state.refresh_surface(target);
        if (changed && !inserted && target.surface) state.schedule(target);
        // Some Shell views omit accessibility events for programmatic selection.
        // Reuse the existing scan for a foreground check without repeating size queries.
        if (!summarize)state.restore(target);
        if (summarize && target.surface && frame.window == foreground && !target.due &&
            (!state.pending || state.pending->frame != frame.window))
            target.due = GetTickCount64();
    }
    std::erase_if(state.targets, [](const auto& item) { return !IsWindow(item.first); });
}

uint64_t SelectionSummary::selection_revision(HWND frame) const {
    const auto found=state_->targets.find(frame);
    return found==state_->targets.end()?0:found->second.selection_revision;
}

void SelectionSummary::poll(ShellTabEnumerator& enumerator) {
    auto& state = *state_;
    const bool show_summary=state.options.show_files||state.options.show_folders||state.options.show_size;
    if (state.pending && state.pending->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto job = std::move(*state.pending);
        state.pending.reset();
        const auto found = state.targets.find(job.frame);
        if (found != state.targets.end()) {
            auto& target = found->second;
            state.refresh_surface(target);
            if (show_summary && target.generation == job.generation && target.surface == job.surface) {
                try {
                    state.display(target, job.result.get());
                } catch (const std::exception& error) {
                    target.error = error.what();
                    state.restore(target);
                }
            }
        }
    }
    if(!show_summary)return;
    const auto now = GetTickCount64();
    for (auto& [frame, target] : state.targets) {
        if (target.verify_at && now >= target.verify_at) {
            state.refresh_surface(target);
            if (target.verify_at) {
                target.verify_at = --target.verifies ? now + 650 : 0;
                try { target.applied = state.write(target, target.surface, target.text) || target.applied; }
                catch (const std::exception& error) {
                    target.error = error.what();
                    target.verify_at = 0;
                }
            }
        }
        if (state.pending || !target.due || now < target.due) continue;
        state.refresh_surface(target);
        if (target.due > now) continue;
        target.due = 0;
        if (!target.surface) continue;
        try {
            const auto view = enumerator.view(frame, GetParent(target.surface));
            auto selection = capture(*view.Get(), state.options);
            if (target.selection == selection) continue;
            target.selection = selection;
            state.restore(target);
            target.error.clear();
            if (!selection.count) continue;
            const auto options = state.options;
            const auto sizes = state.sizes;
            const auto chinese = state.chinese;
            if (!options.show_size) {
                state.display(target, summarize(selection, options, sizes, chinese));
                continue;
            }
            state.pending = State::Job{frame, target.surface, target.generation,
                std::async(std::launch::async, [selection = std::move(selection), options, sizes, chinese] {
                    return summarize(selection, options, sizes, chinese);
                })};
        } catch (const std::exception& error) {
            target.selection.reset();
            target.error = error.what();
        }
    }
}

DWORD SelectionSummary::wait_time(DWORD limit) const {
    if(!state_->options.show_files&&!state_->options.show_folders&&!state_->options.show_size)return limit;
    if (state_->pending) limit = std::min<DWORD>(limit, 50);
    const auto now = GetTickCount64();
    for (const auto& [frame, target] : state_->targets)
        for (auto wake_at : {state_->pending ? 0 : target.due, target.verify_at})
            if (wake_at) limit = static_cast<DWORD>(std::min<uint64_t>(limit, wake_at > now ? wake_at - now : 0));
    return limit;
}

Json SelectionSummary::status() const {
    Json windows = Json::array();
    for (const auto& [frame, target] : state_->targets)
        windows.push_back({{"window", reinterpret_cast<uintptr_t>(frame)},
                           {"surface", reinterpret_cast<uintptr_t>(target.surface)},
                           {"active", target.applied}, {"error", target.error}});
    return {{"watching", state_->selection_events != nullptr}, {"windows", std::move(windows)}};
}

void SelectionSummary::stop() {
    auto& state = *state_;
    State::observing = nullptr;
    if (state.selection_events) UnhookWinEvent(std::exchange(state.selection_events, nullptr));
    if (state.show_events) UnhookWinEvent(std::exchange(state.show_events, nullptr));
    for (auto& [frame, target] : state.targets) state.restore(target);
    state.targets.clear();
}
}
