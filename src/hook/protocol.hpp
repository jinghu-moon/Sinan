#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace tabtail {
inline constexpr wchar_t bridge_message_name[] = L"TabTail.TitleBridge.3";
inline std::wstring bridge_mapping_name(DWORD sender, uint64_t nonce) {
    return L"Local\\TabTail-Bridge3-" + std::to_wstring(sender) + L"-" + std::to_wstring(nonce);
}
enum class BridgeAction : uint32_t { probe = 1, apply = 2, restore = 3 };
struct BridgePacket {
    uint32_t magic = 0x54544252;
    uint32_t version = 3;
    uint32_t size = sizeof(BridgePacket);
    BridgeAction action = BridgeAction::probe;
    uint64_t nonce = 0;
    uint32_t sender_pid = 0;
    uint32_t reserved = 0;
    uint64_t sender_started = 0;
    uint64_t target_started = 0;
    uint64_t window = 0;
    uint64_t browser_window = 0;
    GUID guid{};
    int32_t depth = 2; // 0 is full path on this private wire, never user configuration.
    wchar_t separator[8] = L"\\";
    uint32_t policy_size = 0;
    char policy[256 * 1024]{};
    LONG completed = 0;
    HRESULT result = E_PENDING;
    wchar_t title[32768]{};
    char error[4096]{};
};
static_assert(sizeof(BridgePacket) < 512 * 1024);
}
