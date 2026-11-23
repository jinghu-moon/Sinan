#include "agent/folder_size_hover.hpp"
#include "platform/shell_tabs.hpp"
#include <UIAutomation.h>
#include "agent/explorer_tooltip.hpp"
#include <shlobj.h>
#include <algorithm>
#include <mutex>
#include <thread>

namespace sinan {
using Microsoft::WRL::ComPtr;
using namespace folder_size_ipc;
namespace {
using TaskString = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>;
using Bstr = std::unique_ptr<wchar_t, decltype(&SysFreeString)>;

std::wstring query_time(uint64_t timestamp) {
    FILETIME utc{static_cast<DWORD>(timestamp), static_cast<DWORD>(timestamp >> 32)};
    SYSTEMTIME time{}, local{};
    if (!timestamp || !FileTimeToSystemTime(&utc, &time) || !SystemTimeToTzSpecificLocalTimeEx(nullptr, &time, &local))
        return L"—";
    wchar_t date[80]{}, clock[80]{};
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, std::size(date), nullptr) ||
        !GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, nullptr, clock, std::size(clock))) return L"—";
    return std::wstring(date) + L" " + clock;
}

std::wstring describe(Cache& cache, std::wstring_view path, bool chinese) {
    uint64_t bytes{}, timestamp{}, queried{};
    IndexStatus index{};
    {
        CacheLock lock(&cache);
        if (!lock || cache.count > cache_capacity) return {};
        const auto end = cache.entries + cache.count;
        const auto found = std::find_if(cache.entries, end, [&](const auto& entry) { return path == entry.path; });
        if (found == end) return {};
        bytes = found->bytes;
        timestamp = found->successful_at;
        queried = found->queried;
        index = cache.index;
    }
    const wchar_t* status{};
    switch (index) {
    case IndexStatus::ready:
        status = bytes != unknown ? (chinese ? L"已索引" : L"Indexed") : queried
            ? (chinese ? L"暂无索引结果" : L"No indexed result") : (chinese ? L"等待查询" : L"Pending query");
        break;
    case IndexStatus::loading: status = chinese ? L"索引加载中" : L"Loading index"; break;
    case IndexStatus::incompatible: status = chinese ? L"Everything 版本不兼容" : L"Incompatible Everything version"; break;
    default: status = chinese ? L"Everything 未连接" : L"Everything disconnected"; break;
    }
    std::wstring text = chinese ? L"精确大小：" : L"Exact size: ";
    text += bytes == unknown ? L"—" : std::to_wstring(bytes) + (chinese ? L" 字节" : L" bytes");
    text += (chinese ? L"\n最近成功查询：" : L"\nLast successful query: ") + query_time(timestamp);
    text += chinese ? L"\n索引状态：" : L"\nIndex status: ";
    return text + status;
}

std::wstring hovered_path(IUIAutomation& automation, ShellTabEnumerator& enumerator,
                          const FolderSizeHover::Target& target, HWND surface, POINT point) {
    ComPtr<IUIAutomationElement> cell;
    if (FAILED(automation.ElementFromPoint(point, &cell)) || !cell) return {};
    BSTR id{};
    const auto id_result = cell->get_CurrentAutomationId(&id);
    const Bstr id_owner(id, SysFreeString);
    if (FAILED(id_result) || !id || std::wstring_view(id, SysStringLen(id)) != L"System.Size") return {};
    ComPtr<IUIAutomationGridItemPattern> grid;
    int row_index{};
    if (FAILED(cell->GetCurrentPatternAs(UIA_GridItemPatternId, IID_PPV_ARGS(&grid))) || !grid ||
        FAILED(grid->get_CurrentRow(&row_index)) || row_index < 0) return {};

    ComPtr<IUIAutomationTreeWalker> walker;
    ComPtr<IUIAutomationElement> row;
    if (FAILED(automation.get_RawViewWalker(&walker)) || !walker ||
        FAILED(walker->GetParentElement(cell.Get(), &row)) || !row) return {};
    BSTR name{};
    const auto name_result = row->get_CurrentName(&name);
    const Bstr row_name(name, SysFreeString);
    if (FAILED(name_result) || !name) return {};

    const auto view = enumerator.view(target.frame, target.browser, false);
    HWND folder_window{};
    ComPtr<IFolderView2> folder;
    ComPtr<IShellItem> item;
    if (FAILED(view->GetWindow(&folder_window)) || (folder_window != surface && !IsChild(folder_window, surface)) ||
        FAILED(view.As(&folder)) || FAILED(folder->GetItem(row_index, IID_PPV_ARGS(&item))) || !item) return {};
    constexpr auto attributes = SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_LINK;
    SFGAOF actual{};
    if (FAILED(item->GetAttributes(attributes, &actual)) || actual != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) return {};
    PWSTR display{};
    const auto display_result = item->GetDisplayName(SIGDN_NORMALDISPLAY, &display);
    const TaskString display_name(display, CoTaskMemFree);
    if (FAILED(display_result) || !display || std::wstring_view(name, SysStringLen(name)) != display) return {};
    PWSTR path{};
    const auto path_result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    const TaskString filesystem_path(path, CoTaskMemFree);
    return SUCCEEDED(path_result) && path ? std::wstring(path) : std::wstring{};
}
}

struct FolderSizeHover::State {
    Handle changed{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex;
    std::vector<Target> targets;
    bool chinese{};
    std::jthread thread;

    State() : thread([this](std::stop_token stop) {
        if (!changed) return;
        try { run(stop); } catch (const std::exception&) {}
    }) {}
    ~State() {
        thread.request_stop();
        if (changed) SetEvent(changed.get());
    }

    void run(std::stop_token stop) {
        ShellTabEnumerator enumerator;
        ComPtr<IUIAutomation> automation;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return;
        ExplorerTooltip tooltip;
        POINT previous{};
        HWND previous_surface{};
        HWND previous_browser{};
        uint64_t next_lookup{};
        const HANDLE signal = changed.get();
        DWORD wait = 0;
        while (!stop.stop_requested()) {
            const auto result = MsgWaitForMultipleObjects(1, &signal, FALSE, wait, QS_ALLINPUT);
            if (result == WAIT_FAILED || stop.stop_requested()) break;
            if (result == WAIT_OBJECT_0 + 1) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            std::vector<Target> current;
            bool zh{};
            {
                std::lock_guard lock(mutex);
                current = targets;
                zh = chinese;
            }
            const auto frame = GetForegroundWindow();
            POINT point{};
            const auto surface = !current.empty() && GetPhysicalCursorPos(&point) ? WindowFromPhysicalPoint(point) : nullptr;
            wchar_t class_name[32]{};
            const auto target = std::find_if(current.begin(), current.end(), [&](const auto& candidate) {
                return candidate.frame == frame && IsWindowVisible(candidate.browser) && IsChild(candidate.browser, surface);
            });
            const auto now = GetTickCount64();
            wait = current.empty() ? INFINITE : 100;
            if (target == current.end() || !surface || !GetClassNameW(surface, class_name, std::size(class_name)) ||
                std::wstring_view(class_name) != L"DirectUIHWND" ||
                ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) ||
                now >= static_cast<uint64_t>(InterlockedCompareExchange64(&target->cache->expires, 0, 0))) {
                tooltip.hide();
                previous_surface = nullptr;
                continue;
            }
            if (point.x != previous.x || point.y != previous.y || surface != previous_surface || target->browser != previous_browser) {
                tooltip.hide();
                previous = point;
                previous_surface = surface;
                previous_browser = target->browser;
                next_lookup = now + 450;
            }
            if (now < next_lookup) continue;
            next_lookup = now + 500;
            try {
                const auto path = hovered_path(*automation.Get(), enumerator, *target, surface, point);
                POINT latest{};
                if (path.empty() || GetForegroundWindow() != frame || !IsWindowVisible(target->browser) ||
                    GetTickCount64() >= static_cast<uint64_t>(InterlockedCompareExchange64(&target->cache->expires, 0, 0)) ||
                    !GetPhysicalCursorPos(&latest) || latest.x != point.x || latest.y != point.y) tooltip.hide();
                else tooltip.show(point, describe(*target->cache, path, zh));
            } catch (const Error&) {
                tooltip.hide();
            }
        }
    }
};

FolderSizeHover::FolderSizeHover() = default;
FolderSizeHover::~FolderSizeHover() = default;

void FolderSizeHover::update(std::vector<Target> targets, bool chinese) {
    if (!state_ && targets.empty()) return;
    if (!state_) state_ = std::make_unique<State>();
    std::lock_guard lock(state_->mutex);
    if (state_->targets == targets && state_->chinese == chinese) return;
    state_->targets = std::move(targets);
    state_->chinese = chinese;
    if (state_->changed) SetEvent(state_->changed.get());
}
}
