#pragma once
#include "platform/folder_size_cache.hpp"
#include <memory>
#include <vector>

namespace sinan {
class FolderSizeHover {
    struct State;
    std::unique_ptr<State> state_;
public:
    struct Target {
        HWND frame;
        HWND browser;
        std::shared_ptr<folder_size_ipc::Cache> cache;
        bool operator==(const Target&) const = default;
    };

    FolderSizeHover();
    ~FolderSizeHover();
    void update(std::vector<Target> targets, bool chinese);
};
}
