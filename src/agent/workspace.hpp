#pragma once
#include "config/config.hpp"
namespace sinan {
Json save_workspace(const fs::path& root,const Json& tabs,int limit);
Json load_workspace(const fs::path& root,int limit);
bool workspace_directory_available(const std::wstring& path,DWORD timeout_ms=800);
void open_workspace_window(const std::wstring& path);
std::string workspace_path_key(std::string_view path);
}
