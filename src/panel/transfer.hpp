#pragma once
#include "panel/new_file.hpp"

namespace sinan::panel {
int run_transfer_dialog(HINSTANCE instance,const fs::path& root,const fs::path& source_directory,
    Theme theme,bool chinese,NewFileSource source,std::string_view selection_check);
}
