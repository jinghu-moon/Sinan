#pragma once
#include "config/config.hpp"
#include <vector>
namespace tabtail {
struct ExplorerProcess { DWORD pid; ULONGLONG created; };
std::vector<ExplorerProcess> explorer_processes();
Json explorer_status();
Json system_diagnostics();
Json restart_explorer(bool force);
}
