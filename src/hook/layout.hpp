#pragma once
#include "config/config.hpp"
#include "hook/native.hpp"

namespace sinan {
// A separate module/protocol lets the layout adapter coexist with a pinned v2
// title DLL. No need to replace the title DLL in a running Explorer process.
inline constexpr wchar_t layout_message[] = L"Sinan.LayoutBridge.1";
inline std::wstring layout_mapping(DWORD pid, uint64_t nonce) {
    return L"Local\\Sinan-Layout-" + std::to_wstring(pid) + L"-" + std::to_wstring(nonce);
}
enum class LayoutAction : uint32_t { apply = 1, restore = 2, inspect = 3 };
struct LayoutPacket {
    uint32_t magic = 0x54544c59, version = 1, size = sizeof(LayoutPacket);
    LayoutAction action = LayoutAction::inspect;
    uint64_t nonce = 0, sender_started = 0, target_started = 0, window = 0;
    DWORD sender_pid = 0;
    uint32_t mode = 0; // system=0, fixed=1, content=2
    int32_t width = 320, min_width = 120, max_width = 480;
    LONG completed = 0;
    HRESULT result = E_PENDING;
    char report[64 * 1024]{};
};
Json send_layout_request(const NativeTab& tab, LayoutAction action, const TabAppearance& options = {});
}
