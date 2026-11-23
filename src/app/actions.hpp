#pragma once
#include "config/config.hpp"
namespace sinan {
Json action_catalog(const Json& scripts, std::string query);
Json execute_action(const fs::path& root,const Json& request);
}
