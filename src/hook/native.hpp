#pragma once
#include "platform/windows.hpp"
#include <vector>
#include <optional>

namespace sinan {
// Built-in offline fallback. Symbol profiles admit additional images only after layout validation.
inline constexpr char supported_frame_sha256[] = "4e28d2076608df5525ec1b7b2ed8dc0e866f43bf2ded6a5e4213ccd1c2284cbe";
inline constexpr uint32_t frame_vtable_rva = 0x24d2e0;
inline constexpr uint32_t host_vtable_rva = 0x241958;
inline constexpr uint32_t host_interface_vtable_rva = 0x241858;
inline constexpr uint32_t set_frame_title_rva = 0x42cc0;
inline constexpr uint32_t location_item_rva = 0xe7040;
struct NativeTab {
    HWND window = nullptr;
    HWND browser_window = nullptr;
    DWORD process_id = 0;
    uint64_t process_started = 0;
    uintptr_t frame = 0;
    uintptr_t host = 0;
    uintptr_t module_base = 0;
    GUID guid{};
};
std::string image_file_hash(const fs::path& path);
std::string frame_file_hash();
// Keep admissions separate: a title profile does not validate protection/XAML layouts.
bool native_build_supported(); // title
bool native_inventory_supported();
bool native_features_supported();
bool native_selection_supported();
uintptr_t native_rva(const char* key);
uint32_t native_field(const char* key);
std::vector<NativeTab> read_native_tabs(HWND window, DWORD pid, uint64_t process_started);
std::optional<GUID> read_active_tab(const NativeTab& tab);
std::string guid_text(const GUID& guid);
}
