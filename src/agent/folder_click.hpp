#pragma once
#include "platform/shell_tabs.hpp"
#include "config/config.hpp"
#include <UIAutomation.h>

namespace sinan {
struct FolderDoubleClick {
    HWND frame{};
    HWND target{};
    POINT first{}, second{};
    DWORD time{};
};
class FolderClickListener {
    struct Click { HWND frame{},target{}; POINT point{}; DWORD time{}; };
    HWND window_{};
    std::optional<Click> down_, previous_;
    std::optional<FolderDoubleClick> ready_;
    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
    std::string error_;
    void input(HRAWINPUT raw);
    static LRESULT CALLBACK procedure(HWND,UINT,WPARAM,LPARAM);
public:
    ~FolderClickListener();
    void update(bool enabled);
    std::optional<FolderDoubleClick> take();
    bool empty_items_view(const FolderDoubleClick& click,IShellView& view);
    Json status() const;
};
}
