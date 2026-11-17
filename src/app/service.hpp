#pragma once
#include "config/config.hpp"
namespace sinan {
// Shared in-process application actions for both CLI and the asynchronous TUI.
Json application_action(const fs::path& root, const Json& request);
Json test_rules(const Config& config, const std::wstring& path);
}
