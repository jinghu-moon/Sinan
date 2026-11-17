#include "policy/display.hpp"

namespace sinan {
void validate_depth(std::optional<int> depth) {
    if (depth && (*depth < 1 || *depth > 32))
        throw PolicyError(PolicyErrorCode::invalid_argument, "depth 必须为 1..32 或 all");
}
void validate_display(const DisplayOptions& display) {
    validate_depth(display.depth);
    if (display.separator != L"\\" && display.separator != L" / " && display.separator != L" · ")
        throw PolicyError(PolicyErrorCode::invalid_argument, "separator 必须为 \\、 / 或 ·（后两者两侧各一个空格）");
}
EffectiveDisplay resolve_policy(const DisplayOptions& defaults, const std::optional<DepthOverride>& override) {
    validate_display(defaults);
    EffectiveDisplay result{defaults, PolicySource::global};
    if (override) {
        validate_depth(override->depth);
        result.display.depth = override->depth;
        result.source = PolicySource::tab;
    }
    return result;
}
bool same_title_policy(const DisplayOptions& a, const DisplayOptions& b) {
    return a.depth == b.depth && (!a.depth || a.separator == b.separator) && a.entries == b.entries;
}
}
