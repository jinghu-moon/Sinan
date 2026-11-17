#pragma once
#include "installer/environment.hpp"
namespace sinan {
Json path_status(const fs::path& root);
Json manage_path(const fs::path& root, bool add);
Json install(const fs::path& root);
Json uninstall(const fs::path& root, bool purge);
}
