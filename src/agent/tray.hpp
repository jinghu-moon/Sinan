#pragma once
#include "config/config.hpp"
#include <optional>

namespace sinan {
enum class TrayAction : UINT { open_tui=1, toggle_customization, toggle_startup, exit };

struct TrayState {
    bool chinese{};
    bool customizing{};
    bool applied{};
    bool autostart{};
    bool tui_available{};
    bool customization_available{};
};

class TrayIcon {
    HWND window_{};
    UINT taskbar_created_{};
    bool registered_{},retry_registration_{},menu_open_{},version4_{};
    TrayState state_;
    std::wstring tooltip_;
    std::optional<POINT> next_menu_;
    std::optional<TrayAction> next_action_;
    std::string error_;
    void add_icon();
    void close();
    void menu(POINT point);
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam);
public:
    ~TrayIcon();
    void update(bool enabled,TrayState state={});
    std::optional<TrayAction> take();
    Json status() const;
    void report(std::wstring_view message,bool error=false) const;
    bool confirm_exit() const;
};
}
