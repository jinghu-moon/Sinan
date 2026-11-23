#pragma once
#include "platform/shell_tabs.hpp"
#include "config/config.hpp"

namespace sinan {
Json toggle_file_extensions();
bool file_extensions_visible();
std::wstring everything_search_argument(std::wstring_view query);
std::wstring clipboard_path();
Microsoft::WRL::ComPtr<IShellItemArray> shell_selection(IShellView& view, DWORD& count);
std::vector<std::wstring> selected_paths(IShellView& view);
std::vector<std::wstring> selected_names(IShellView& view);
std::string selection_fingerprint(std::vector<std::wstring> paths);
bool matches_selection_rule(IShellView& view, const Json& rule);
bool transfer_paths(const std::vector<std::wstring>& paths,const std::wstring& directory,bool move,HWND owner);
Json create_named_folder(const std::wstring& parent, const std::wstring& name);
Json run_shell_view_action(IShellView& view, HWND frame, std::string_view action, const Json& parameters);
Json run_powershell_action(const fs::path& script, IShellView* view);
std::vector<std::wstring> command_arguments(std::string_view source,const std::wstring& directory,
    const std::vector<std::pair<std::wstring,bool>>& selections);
Json run_command_application(const Json& application, IShellView& view, HWND frame);
std::wstring command_application_icon(const fs::path& cache, const std::wstring& target);
void reveal_file(const std::wstring& file, IShellView* existing_view=nullptr);
}
