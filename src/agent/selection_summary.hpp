#pragma once
#include "config/config.hpp"
#include "platform/shell_tabs.hpp"
#include <memory>

namespace sinan {
class SelectionSummary {
    struct State;
    std::unique_ptr<State> state_;

public:
    SelectionSummary();
    ~SelectionSummary();
    void synchronize(const std::vector<NativeTab>& windows, const Config& config, bool chinese);
    uint64_t selection_revision(HWND frame) const;
    void poll(ShellTabEnumerator& enumerator);
    DWORD wait_time(DWORD limit) const;
    Json status() const;
    void stop();
};
}
