#pragma once
#include "config/config.hpp"
#include <shobjidl.h>

namespace sinan {
struct CommandTemplate {
    fs::path path;
    std::wstring label;
    bool folder{};
};
struct CommandTemplates {
    fs::path directory;
    bool hide_extension=true;
    bool hide_digits=true;
    bool variables=false;
};
CommandTemplates command_templates(const fs::path& root,const Json& options);
std::vector<CommandTemplate> list_command_templates(const CommandTemplates& options);
std::wstring template_name(std::wstring name,const fs::path& directory,bool variables);
fs::path shell_create_item(HWND owner,const fs::path& directory,const std::wstring& name,bool folder,
    const fs::path& source={},bool collision_suffix=false);
bool select_created_item(IShellView& view, const fs::path& path, bool rename);
}
