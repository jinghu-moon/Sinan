#pragma once
#include "config/config.hpp"

namespace sinan {
Json update_selection_status(HMODULE self, HWND window, DWORD sender, const Json& request);
}
