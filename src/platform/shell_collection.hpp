#pragma once
#include "platform/windows.hpp"
#include <wrl/client.h>
#include <exdisp.h>
#include <vector>

namespace sinan {
struct ShellCollectionSnapshot {
    std::vector<Microsoft::WRL::ComPtr<IDispatch>> windows;
    bool complete = true;
    std::vector<std::string> warnings;
    long registration_count = 0;
};
// Count can include vacant registrations; enumerate objects instead of indexing slots.
ShellCollectionSnapshot read_shell_collection(IShellWindows& collection);
}
