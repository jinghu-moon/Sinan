#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace sinan {
struct FolderSizeSettings {
    bool enabled = false;
    std::string unit_mode = "auto";
    std::string unit_system = "iec";
    int decimal_places = 2;
    std::string fixed_unit = "MiB";
    std::wstring custom_unit_label = L"块";
    uint64_t custom_unit_bytes = 4096;
    std::wstring everything_instance;
    bool operator==(const FolderSizeSettings&) const = default;
};

std::wstring format_folder_size(uint64_t bytes, const FolderSizeSettings& settings,
                                std::wstring_view decimal_separator);
}
