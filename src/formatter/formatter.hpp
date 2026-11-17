#pragma once
#include "policy/tab_registry.hpp"
namespace tabtail {
struct Title {
    std::wstring text;
    bool filesystem = false;
    bool root = false;
};
Title format_title(std::wstring_view path, const DisplayOptions& display, std::wstring_view original = {});
Title format_tab_title(const TabSnapshot& tab);
}
