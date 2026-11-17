#pragma once
#include "config/config.hpp"
#include <functional>
#include <memory>

namespace tabtail {
std::wstring agent_endpoint(const fs::path& root);
// nullopt means no server exists; access/transport/protocol failures are errors.
std::optional<Json> agent_request(const fs::path& root, Json request, DWORD timeout_ms = 5000);
void verify_peer_process(DWORD pid);
class PipeServer {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit PipeServer(const fs::path& root);
    ~PipeServer();
    bool poll(const std::function<Json(const Json&)>& handler, DWORD wait_ms = 100);
};
}
