#pragma once
#include "config/config.hpp"
#include "hook/native.hpp"

namespace sinan {
// A separate module/protocol lets the layout adapter coexist with a pinned v2
// title DLL. No need to replace the title DLL in a running Explorer process.
inline constexpr wchar_t layout_message[] = L"Sinan.LayoutBridge.2";
inline constexpr wchar_t folder_size_message[] = L"Sinan.FolderSizeBridge.1";
inline std::wstring layout_mapping(DWORD pid, uint64_t nonce) {
    return L"Local\\Sinan-Layout-" + std::to_wstring(pid) + L"-" + std::to_wstring(nonce);
}
enum class LayoutAction : uint32_t { apply = 1, restore = 2, inspect = 3, search_enable = 4, search_disable = 5,
    folder_sizes = 6, selection_status = 7, command_bar_enable = 8, command_bar_disable = 9, command_bar_inspect = 10 };
struct LayoutOptions {
    TabAppearance tabs;
    bool search_box_hidden = false;
};
struct LayoutPacket {
    uint32_t magic = 0x54544c59, version = 2, size = sizeof(LayoutPacket);
    LayoutAction action = LayoutAction::inspect;
    uint64_t nonce = 0, sender_started = 0, target_started = 0, window = 0;
    DWORD sender_pid = 0;
    uint32_t mode = 0; // system=0, fixed=1, content=2
    uint32_t search_box_hidden = 0;
    int32_t width = 320, min_width = 120, max_width = 480;
    LONG completed = 0;
    HRESULT result = E_PENDING;
    char report[64 * 1024]{};
};
Json send_layout_request(const NativeTab& tab, LayoutAction action, const LayoutOptions& options = {});
Json send_layout_payload(const NativeTab& tab, LayoutAction action, const LayoutOptions& options,
                         const Json& request);
}
