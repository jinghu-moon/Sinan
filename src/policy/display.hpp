#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sinan {
enum class PolicyErrorCode { invalid_argument, unknown_tab, retired_identity, stale_update, counter_exhausted };
class PolicyError : public std::runtime_error {
    PolicyErrorCode code_;
public:
    PolicyError(PolicyErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
    PolicyErrorCode code() const noexcept { return code_; }
};

// A missing depth means the full path. It does NOT mean inheritance.
struct DirectoryEntry {
    std::string id;
    std::wstring name, path;
    std::string shortcut;
    bool enabled=true, descendants=false;
    std::string title="alias_depth";
    std::string scope="global";
    bool operator==(const DirectoryEntry&) const = default;
};
struct DisplayOptions {
    std::optional<int> depth = 2;
    std::wstring separator = L"\\";
    std::vector<DirectoryEntry> entries;
    bool operator==(const DisplayOptions&) const = default;
};
struct DepthOverride {
    std::optional<int> depth;
    bool operator==(const DepthOverride&) const = default;
};
enum class PolicySource { global, rule, tab };
struct EffectiveDisplay {
    DisplayOptions display;
    PolicySource source = PolicySource::global;
    bool native = false;
    std::string rule_id;
    std::wstring matched_pattern;
    bool operator==(const EffectiveDisplay&) const = default;
};

void validate_depth(std::optional<int> depth);
void validate_display(const DisplayOptions& display);
EffectiveDisplay resolve_policy(const DisplayOptions& defaults, const std::optional<DepthOverride>& override);
bool same_title_policy(const DisplayOptions& a, const DisplayOptions& b);
}
