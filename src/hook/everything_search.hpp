#pragma once
#include "config/config.hpp"

namespace sinan::explorer_search {
Json configure(HMODULE module, DWORD owner, uint64_t started, bool enabled, bool all_drives);
}
