#pragma once
#include "config/config.hpp"
#include <vector>

namespace tabtail {
struct RegistryValue {
    bool exists = false;
    DWORD type = REG_EXPAND_SZ;
    std::wstring text;
    bool operator==(const RegistryValue&) const = default;
};
RegistryValue registry_read(std::wstring_view key, std::wstring_view name);
void registry_write(std::wstring_view key, std::wstring_view name, const RegistryValue& value);
std::wstring normalize_path_entry(std::wstring_view entry);
std::vector<std::wstring> split_path(std::wstring_view path);
bool path_contains(std::wstring_view path, std::wstring_view entry);
struct PathChange { bool changed = false; bool notified = false; };
PathChange change_user_path(std::wstring_view entry, bool add,
    std::wstring_view key = L"Environment", bool broadcast = true);
bool notify_environment();
}
