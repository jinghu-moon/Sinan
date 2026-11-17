#pragma once
#include "hook/native.hpp"
#include "hook/protocol.hpp"
#include "policy/rules.hpp"

namespace tabtail {
struct BridgeResult { HRESULT result = E_FAIL; std::wstring title; };
// Read-only preflight: old pinned hooks survive agent exit until Explorer exits.
std::vector<fs::path> conflicting_title_modules(DWORD pid,const fs::path& expected);
BridgeResult send_title_request(const NativeTab& tab, BridgeAction action, const RuntimePolicy& policy = {});
}
