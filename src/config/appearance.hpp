#pragma once
#include <string>

namespace tabtail {
// Dimensions are device-independent pixels, before Windows display scaling.
struct TabAppearance {
    std::string width_mode = "system";
    int width = 320;
    int min_width = 120;
    int max_width = 480;
    bool operator==(const TabAppearance&) const = default;
};
void validate_appearance(const TabAppearance& value);
}
