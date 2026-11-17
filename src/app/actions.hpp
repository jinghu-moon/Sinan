#pragma once
#include "config/config.hpp"
namespace sinan { Json action_catalog(std::string query); Json execute_action(const fs::path& root,const Json& request); }
