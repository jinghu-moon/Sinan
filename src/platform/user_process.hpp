#pragma once
#include "platform/windows.hpp"

namespace sinan {
struct UserProcess {
    Handle process;
    Handle thread;
    UserProcess(HANDLE p, HANDLE t) : process(p), thread(t) {}
};

// Prepare before ending Explorer: retain a verified desktop-user primary token
// and environment even if the source desktop process exits during restart.
class UserProcessLauncher {
    Handle token_;
    void* environment_ = nullptr;
    bool elevated_context_ = false;
public:
    UserProcessLauncher();
    ~UserProcessLauncher();
    bool uses_user_token() const { return static_cast<bool>(token_) && !elevated_context_; }
    bool launches_elevated() const { return elevated_context_; }
    UserProcess launch(const fs::path& executable, std::wstring command, DWORD flags = 0) const;
};
}
