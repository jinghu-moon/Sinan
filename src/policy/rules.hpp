#pragma once
#include "policy/display.hpp"
#include <memory>
#include <vector>

namespace sinan {
struct DirectoryRule {
    std::string id;
    std::wstring name;
    bool enabled = true;
    bool exact = false;
    bool case_sensitive = false;
    bool native = false;
    std::optional<int> depth = 2;
    std::vector<std::wstring> patterns;
    bool operator==(const DirectoryRule&) const = default;
};
class RuleSet {
    struct Impl;
    std::shared_ptr<const Impl> impl_;
public:
    explicit RuleSet(std::vector<DirectoryRule> rules = {});
    const std::vector<DirectoryRule>& definitions() const;
    EffectiveDisplay resolve(const DisplayOptions& defaults, const std::optional<DepthOverride>& override,
        const std::optional<std::wstring>& path) const;
};
struct RuntimePolicy {
    DisplayOptions defaults;
    std::shared_ptr<const RuleSet> rules;
    std::optional<DepthOverride> override;
    EffectiveDisplay resolve(const std::optional<std::wstring>& path) const;
};
}
