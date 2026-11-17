#include "installer/environment.hpp"
#include <algorithm>
#include <cwctype>

namespace tabtail {
namespace {
struct Key { HKEY value = nullptr; ~Key() { if (value) RegCloseKey(value); } };
void check(LSTATUS result, std::string_view action) {
    if (result != ERROR_SUCCESS) throw Error(result == ERROR_ACCESS_DENIED ? 4 : 1, system_error(action, result));
}
}
RegistryValue registry_read(std::wstring_view key, std::wstring_view name) {
    Key k; std::wstring path(key), field(name);
    auto r = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &k.value);
    if (r == ERROR_FILE_NOT_FOUND) return {};
    check(r, "打开用户注册表");
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = 0, type = 0;
        r = RegQueryValueExW(k.value, field.c_str(), nullptr, &type, nullptr, &size);
        if (r == ERROR_FILE_NOT_FOUND) return {};
        check(r, "读取注册表值长度");
        if (type != REG_SZ && type != REG_EXPAND_SZ) throw Error(2, "注册表值不是文本类型");
        if (size > 1024 * 1024 || size % sizeof(wchar_t)) throw Error(2, "注册表文本长度无效");
        std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
        r = RegQueryValueExW(k.value, field.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size);
        if (r == ERROR_MORE_DATA) continue;
        check(r, "读取注册表值");
        value.resize(size / sizeof(wchar_t));
        while (!value.empty() && value.back() == L'\0') value.pop_back();
        if (value.find(L'\0') != value.npos) throw Error(2, "注册表文本含嵌入 NUL");
        return {true, type, value};
    }
    throw Error(1, "注册表被其他程序持续修改，请重试");
}
void registry_write(std::wstring_view key, std::wstring_view name, const RegistryValue& value) {
    Key k; std::wstring path(key), field(name);
    check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k.value, nullptr), "打开用户注册表");
    if (!value.exists) {
        auto r = RegDeleteValueW(k.value, field.c_str());
        if (r != ERROR_FILE_NOT_FOUND) check(r, "删除注册表值");
        return;
    }
    check(RegSetValueExW(k.value, field.c_str(), 0, value.type, reinterpret_cast<const BYTE*>(value.text.c_str()),
        static_cast<DWORD>((value.text.size() + 1) * sizeof(wchar_t))), "写入注册表值");
}
std::vector<std::wstring> split_path(std::wstring_view path) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        auto end = path.find(L';', start);
        parts.emplace_back(path.substr(start, end == path.npos ? path.size() - start : end - start));
        if (end == path.npos) break;
        start = end + 1;
    }
    return parts;
}
std::wstring normalize_path_entry(std::wstring_view entry) {
    while (!entry.empty() && iswspace(entry.front())) entry.remove_prefix(1);
    while (!entry.empty() && iswspace(entry.back())) entry.remove_suffix(1);
    if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"') { entry.remove_prefix(1); entry.remove_suffix(1); }
    std::wstring out(entry);
    DWORD needed = ExpandEnvironmentStringsW(out.c_str(), nullptr, 0);
    if (!needed) throw Error(1, system_error("展开环境变量"));
    std::wstring expanded(needed, L'\0');
    auto written = ExpandEnvironmentStringsW(out.c_str(), expanded.data(), needed);
    if (!written || written > needed) throw Error(1, "展开环境变量时发生并发修改");
    expanded.resize(written - 1); out = std::move(expanded);
    for (auto& ch : out) { if (ch == L'/') ch = L'\\'; ch = static_cast<wchar_t>(towlower(ch)); }
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}
bool path_contains(std::wstring_view path, std::wstring_view entry) {
    auto wanted = normalize_path_entry(entry);
    if (wanted.empty()) return false;
    for (const auto& p : split_path(path)) if (normalize_path_entry(p) == wanted) return true;
    return false;
}
bool notify_environment() {
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
        SMTO_ABORTIFHUNG, 1000, &result) != 0;
}
PathChange change_user_path(std::wstring_view entry, bool add, std::wstring_view key, bool broadcast) {
    if (entry.empty() || entry.find(L';') != entry.npos || entry.find(L'\0') != entry.npos) throw Error(2, "PATH 条目无效");
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto before = registry_read(key, L"Path");
        RegistryValue after = before;
        if (add) {
            if (path_contains(before.text, entry)) return {};
            after.exists = true;
            if (!after.text.empty()) after.text += L';';
            after.text += entry;
        } else {
            if (!path_contains(before.text, entry)) return {};
            auto parts = split_path(before.text);
            auto wanted = normalize_path_entry(entry);
            parts.erase(std::remove_if(parts.begin(), parts.end(), [&](const auto& p) { return normalize_path_entry(p) == wanted; }), parts.end());
            after.text.clear();
            for (size_t i = 0; i < parts.size(); ++i) { if (i) after.text += L';'; after.text += parts[i]; }
        }
        if (after.text.size() >= 32767) throw Error(2, "PATH 长度超出限制");
        if (!(registry_read(key, L"Path") == before)) continue;
        registry_write(key, L"Path", after);
        if (!(registry_read(key, L"Path") == after)) throw Error(5, "PATH 写入后被其他程序修改，请检查 path status");
        return {true, !broadcast || notify_environment()};
    }
    throw Error(1, "PATH 修改冲突，请重试");
}
}
