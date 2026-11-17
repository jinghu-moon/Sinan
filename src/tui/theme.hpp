#pragma once
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

namespace tabtail::nord {
// Construct FTXUI colors after terminal capabilities are configured.
struct Rgb {
    uint8_t r,g,b;
    operator ftxui::Color() const {return ftxui::Color::RGB(r,g,b);}
};
inline constexpr Rgb background{0x2e,0x34,0x40};
inline constexpr Rgb surface{0x3b,0x42,0x52};
inline constexpr Rgb selection{0x4c,0x56,0x6a};
inline constexpr Rgb foreground{0xd8,0xde,0xe9};
inline constexpr Rgb white{0xe5,0xe9,0xf0};
inline constexpr Rgb red{0xbf,0x61,0x6a};
inline constexpr Rgb green{0xa3,0xbe,0x8c};
inline constexpr Rgb yellow{0xeb,0xcb,0x8b};
inline constexpr Rgb blue{0x81,0xa1,0xc1};
inline constexpr Rgb purple{0xb4,0x8e,0xad};
inline constexpr Rgb cyan{0x88,0xc0,0xd0};

inline ftxui::Element canvas(ftxui::Element element) {
    return element|ftxui::color(foreground)|ftxui::bgcolor(background);
}
inline ftxui::Element panel(const std::string& title,ftxui::Element content) {
    return ftxui::window(ftxui::text(" "+title+" ")|ftxui::color(cyan),
        content|ftxui::color(foreground),ftxui::LIGHT)|ftxui::color(blue)|ftxui::bgcolor(background);
}
inline ftxui::Element divider() {return ftxui::separatorLight()|ftxui::color(selection);}
inline ftxui::Element section(const std::string& title) {
    return ftxui::hbox({ftxui::text(" ◆ "+title+" ")|ftxui::bold|ftxui::color(cyan),divider()|ftxui::flex});
}
}
