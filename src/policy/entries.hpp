#pragma once
#include "policy/display.hpp"
#include <vector>

namespace sinan {
// Lexical normalization only; no filesystem/network access on Explorer's UI thread.
std::wstring directory_key(std::wstring_view path);
void validate_entries(const std::vector<DirectoryEntry>& entries);
std::optional<std::wstring> alias_title(const std::vector<DirectoryEntry>& entries,
    std::wstring_view path,const DisplayOptions& display);
// Stable partition; an unpinned tab joins the ordinary group at its pinned boundary.
std::vector<std::string> dock_order(const std::vector<std::string>& current,
    const std::vector<std::string>& pinned,std::string_view side,
    const std::vector<std::string>& just_unpinned={});
}
