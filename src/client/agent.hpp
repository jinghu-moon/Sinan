#pragma once
#include "ipc/pipe.hpp"

namespace sinan {
Json start_observer_agent(const fs::path& root);
Json stop_observer_agent(const fs::path& root);
Json observer_status(const fs::path& root);
Json require_agent_result(const fs::path& root, Json request, int& result_code);
}
