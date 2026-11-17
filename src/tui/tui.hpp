#pragma once
#include "platform/windows.hpp"
namespace tabtail {
int run_tui(const fs::path& root,const std::string& language={},bool snapshot=false,int width=110,int height=36);
}
