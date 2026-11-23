#pragma once
#include "config/config.hpp"
namespace sinan {
Json login_startup_status(const fs::path& root,const Config& config);
Json sync_login_startup(const fs::path& root,const fs::path& executable=executable_path());
void remove_login_startup(const fs::path& root);
}
