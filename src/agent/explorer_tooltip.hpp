#pragma once
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <string>
#include <utility>

namespace sinan {
class ExplorerTooltip {
    HWND window{};
    TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
    std::wstring text;

public:
    ExplorerTooltip() = default;
    ~ExplorerTooltip() { if (window) DestroyWindow(window); }
    ExplorerTooltip(const ExplorerTooltip&) = delete;
    ExplorerTooltip& operator=(const ExplorerTooltip&) = delete;
    bool visible() const { return window && IsWindowVisible(window); }

    void hide() {
        if (text.empty()) return;
        SendMessageW(window, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tool));
        text.clear();
    }

    void show(POINT point, std::wstring value) {
        if (value.empty()) { hide(); return; }
        if (text == value) return;
        const bool visible = !text.empty();
        text = std::move(value);
        tool.lpszText = text.data();
        if (!window) {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
            InitCommonControlsEx(&controls);
            window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (!window) { text.clear(); return; }
            SendMessageW(window, TTM_SETWINDOWTHEME, 0, reinterpret_cast<LPARAM>(L"Explorer"));
            const auto corners = DWMWCP_ROUNDSMALL;
            DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
            tool.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_TRANSPARENT;
            tool.hwnd = window;
            tool.uId = 1;
            if (!SendMessageW(window, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
                DestroyWindow(window);
                window = nullptr;
                text.clear();
                return;
            }
            const auto dpi = GetDpiForWindow(window);
            SendMessageW(window, TTM_SETMAXTIPWIDTH, 0, MulDiv(420, dpi, 96));
        } else {
            SendMessageW(window, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        if (!visible) {
            HIGHCONTRASTW contrast{sizeof(contrast)};
            const bool use_system_border = !SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) ||
                (contrast.dwFlags & HCF_HIGHCONTRASTON);
            const COLORREF border = use_system_border ? DWMWA_COLOR_DEFAULT : DWMWA_COLOR_NONE;
            DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &border, sizeof(border));
            const auto dpi = GetDpiForWindow(window);
            SendMessageW(window, TTM_TRACKPOSITION, 0,
                         MAKELPARAM(point.x + MulDiv(12, dpi, 96), point.y + MulDiv(20, dpi, 96)));
            SendMessageW(window, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
        }
    }
};
}
