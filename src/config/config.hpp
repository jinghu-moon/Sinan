#pragma once
#include "platform/windows.hpp"
#include "policy/display.hpp"
#include "policy/folder_sizes.hpp"
#include "policy/rules.hpp"
#include "config/appearance.hpp"
#include "config/tab_features.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <optional>

namespace sinan {
using Json = nlohmann::json;
struct SelectionSummarySettings {
    bool show_files = true;
    bool show_folders = true;
    bool show_size = true;
    bool operator==(const SelectionSummarySettings&) const = default;
};

struct Config {
    int64_t revision = 1;
    bool enabled = false;
    bool autostart = false;
    bool show_tray_icon = false;
    std::optional<int> depth = 2;
    std::wstring separator = L"\\";
    std::string language = "auto";
    std::vector<DirectoryRule> rules;
    TabAppearance tabs;
    bool search_box_hidden = false;
    bool double_click_parent = false;
    bool everything_search = false;
    bool everything_search_all_drives = false;
    bool external_tabs = false;
    FolderSizeSettings folder_sizes;
    SelectionSummarySettings selection_summary;
    Json features = default_features();
    DisplayOptions display_options() const;
};
Json entry_json(const DirectoryEntry& entry);
std::vector<DirectoryEntry> entries_from_json(const Json& entries);
std::optional<int> parse_depth(std::string_view text);
void validate_config(const Config& config);
Json config_json(const Config& config);
Json folder_sizes_json(const FolderSizeSettings& settings, bool text_bytes = false);
FolderSizeSettings folder_sizes_from_json(const Json& settings);
Json selection_summary_json(const SelectionSummarySettings& settings);
SelectionSummarySettings selection_summary_from_json(const Json& settings);
std::string config_toml(const Config& config);
Json rule_json(const DirectoryRule& rule);
DirectoryRule rule_from_json(const Json& value);
Config config_from_json(const Json& value);
Json appearance_json(const TabAppearance& value);
TabAppearance appearance_from_json(const Json& value);
int parse_tab_width(std::string_view text);
std::string runtime_policy_json(const RuntimePolicy& policy);
RuntimePolicy runtime_policy_from_json(std::string_view value);
class ConfigStore {
    fs::path root_;
public:
    explicit ConfigStore(fs::path root) : root_(std::move(root)) {}
    fs::path path() const { return root_ / L"sinan.toml"; }
    Config load() const;
    Config update(const std::function<void(Config&)>& edit) const;
};
}
