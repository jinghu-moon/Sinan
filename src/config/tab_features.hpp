#pragma once
#include "platform/windows.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <array>

namespace sinan {
using FeatureJson = nlohmann::json;
struct TabFlags {
    bool pinned=false, protected_tab=false, locked=false;
    bool close_protected() const { return pinned || protected_tab; }
    bool operator==(const TabFlags&) const = default;
};
struct Shortcut { UINT modifiers=0, key=0; bool operator==(const Shortcut&) const = default; };
Shortcut parse_shortcut(std::string_view chord);
inline constexpr std::array explorer_shortcut_keys{
    "toggle_pin","toggle_protection","toggle_directory_lock","open_terminal",
    "copy_directory","copy_selection","locate_clipboard","new_folder","new_file",
    "reopen_closed_tab","merge_duplicate_tabs","open_parent_tab","toggle_file_extensions","transfer_selection","extract_archive","locate_clipboard_global"
};
std::string archive_extension(std::string value);
std::string_view shell_shortcut_action(std::string_view key);
void validate_folder_name(std::wstring_view name);
FeatureJson default_features();
void validate_features(const FeatureJson& value, bool check_fonts=true);
void validate_style(const FeatureJson& value, bool check_fonts=true);
FeatureJson resolve_style(const FeatureJson& features, const TabFlags& flags,
                          const std::wstring& path, const FeatureJson& temporary={});
FeatureJson style_for_theme(const FeatureJson& style, bool dark, bool selected, bool hover, bool high_contrast);
double contrast_ratio(std::string_view foreground, std::string_view background);
bool installed_font(const std::wstring& family);
FeatureJson flags_json(const TabFlags& flags);
}
