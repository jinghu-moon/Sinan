#pragma once
#include "panel/theme.hpp"
#include "platform/windows.hpp"

namespace sinan::panel {
struct NewFileSource {
    HWND frame{}, tab{};
};
int run_new_file_dialog(HINSTANCE instance, fs::path directory, Theme theme, bool chinese, NewFileSource source);
}
