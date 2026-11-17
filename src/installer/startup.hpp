#pragma once
#include "config/config.hpp"
namespace sinan {
Json manage_restore_startup(const fs::path& root,bool enabled,const fs::path& executable=executable_path());
}
