#pragma once
#include "platform/windows.hpp"
namespace sinan {
int run_tui(const fs::path& root,const std::string& language={},bool snapshot=false,int width=110,int height=36);
}
