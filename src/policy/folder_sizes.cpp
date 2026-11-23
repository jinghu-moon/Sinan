#include "policy/folder_sizes.hpp"
#include <algorithm>
#include <array>

namespace sinan {
namespace {
constexpr std::array iec{L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB", L"EiB"};
constexpr std::array si{L"B", L"kB", L"MB", L"GB", L"TB", L"PB", L"EB"};

struct Rounded {
    uint64_t whole;
    unsigned fraction;
};

Rounded round_size(uint64_t bytes, uint64_t divisor, unsigned scale) {
    uint64_t whole = bytes / divisor;
    uint64_t remainder = bytes % divisor;
    unsigned fraction = 0;
    // Compute each decimal digit without multiplying a 64-bit value by the full scale.
    for (unsigned count = scale; count > 1; count /= 10) {
        remainder *= 10;
        fraction = fraction * 10 + static_cast<unsigned>(remainder / divisor);
        remainder %= divisor;
    }
    if (remainder >= divisor / 2 + divisor % 2 && ++fraction == scale) {
        ++whole;
        fraction = 0;
    }
    return {whole, fraction};
}
}

std::wstring format_folder_size(uint64_t bytes, const FolderSizeSettings& settings,
                                std::wstring_view decimal_separator) {
    const bool automatic = settings.unit_mode == "auto";
    const bool custom = settings.unit_mode == "custom";
    const bool binary = automatic ? settings.unit_system == "iec" :
        settings.fixed_unit.find('i') != std::string::npos;
    const auto& units = binary ? iec : si;
    const uint64_t base = binary ? 1024 : 1000;
    size_t level = 0;
    uint64_t divisor = 1;
    if (custom) {
        divisor = settings.custom_unit_bytes;
    } else if (automatic) {
        while (level + 1 < units.size() && bytes / divisor >= base) {
            divisor *= base;
            ++level;
        }
    } else {
        const std::wstring unit(settings.fixed_unit.begin(), settings.fixed_unit.end());
        const auto found = std::find(units.begin(), units.end(), unit);
        if (found != units.end()) {
            level = static_cast<size_t>(found - units.begin());
            for (size_t i = 0; i < level; ++i) divisor *= base;
        }
    }

    unsigned places = custom || level ? static_cast<unsigned>(settings.decimal_places) : 0;
    unsigned scale = 1;
    for (unsigned i = 0; i < places; ++i) scale *= 10;
    auto rounded = round_size(bytes, divisor, scale);
    if (automatic && rounded.whole == base && level + 1 < units.size()) {
        ++level;
        rounded = round_size(bytes, divisor * base, scale);
    }
    std::wstring prefix;
    if (bytes && !rounded.whole && !rounded.fraction) {
        prefix = L"<";
        rounded = places ? Rounded{0, 1} : Rounded{1, 0};
    }
    auto result = prefix + std::to_wstring(rounded.whole);
    if (places) {
        const auto fraction = std::to_wstring(rounded.fraction);
        result.append(decimal_separator).append(places - fraction.size(), L'0').append(fraction);
    }
    return result + L" " + (custom ? settings.custom_unit_label : units[level]);
}
}
