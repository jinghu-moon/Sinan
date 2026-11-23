#pragma once
#include "hook/layout.hpp"
#include <memory>
#include <vector>

namespace sinan {
class ShellTabEnumerator;
class FolderSizes {
    struct State;
    std::unique_ptr<State> state_;
public:
    FolderSizes();
    ~FolderSizes();
    void synchronize(const FolderSizeSettings& settings, const std::vector<NativeTab>& windows,
                     const Json& tabs, ShellTabEnumerator& enumerator, bool chinese);
    Json status() const;
};
}
