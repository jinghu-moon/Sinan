#pragma once
#include "platform/windows.hpp"
#include "policy/tab_registry.hpp"
#include "hook/native.hpp"
#include <wrl/client.h>
#include <vector>
#include <map>

namespace tabtail {
struct ShellTabObservation {
    HWND window = nullptr;
    HWND browser_window = nullptr;
    DWORD process_id = 0;
    uint64_t process_started = 0;
    Microsoft::WRL::ComPtr<IUnknown> com_identity;
    TabLocation location;
    bool location_resolved = false;
    std::string warning;
    std::optional<NativeTab> native;
};
struct ShellTabScan {
    std::vector<ShellTabObservation> tabs;
    bool complete = true;
    std::vector<std::string> warnings;
};
// Own this object and every returned COM pointer on the same STA thread.
class ShellTabEnumerator {
    bool initialized_ = false;
    std::map<std::string,Microsoft::WRL::ComPtr<IUnknown>> native_identities_;
public:
    ShellTabEnumerator();
    ~ShellTabEnumerator();
    ShellTabEnumerator(const ShellTabEnumerator&) = delete;
    ShellTabEnumerator& operator=(const ShellTabEnumerator&) = delete;
    ShellTabScan scan(bool include_native=false);
};
}
