#include "platform/windows.hpp"
#include <shlobj.h>
#include <sddl.h>
#include <fstream>
#include <vector>

namespace sinan {
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw Error(2, "无效的 UTF-16 文本");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) throw Error(2, "无效的 UTF-8 文本");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}
std::string system_error(std::string_view operation, DWORD code) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = buffer ? buffer : L"未知系统错误";
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) message.pop_back();
    return std::string(operation) + ": " + utf8(message) + " (" + std::to_string(code) + ")";
}
std::wstring env(std::wstring_view key) {
    std::wstring k(key);
    auto n = GetEnvironmentVariableW(k.c_str(), nullptr, 0);
    if (!n) return {};
    std::wstring out(n, L'\0');
    auto written = GetEnvironmentVariableW(k.c_str(), out.data(), n);
    if (written >= n) throw Error(1, "读取环境变量时发生并发修改");
    out.resize(written);
    return out;
}
fs::path executable_path() {
    std::wstring out(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, out.data(), static_cast<DWORD>(out.size()));
    if (!n || n >= out.size()) throw Error(1, system_error("获取可执行文件路径"));
    out.resize(n);
    return fs::path(out);
}
fs::path data_directory() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value))) throw Error(1, "无法获取 LocalAppData");
    fs::path out = fs::path(value) / L"Sinan";
    CoTaskMemFree(value);
    return out;
}
std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Error(1, "无法读取文件: " + utf8(path.wstring()));
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void atomic_write(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    auto temp = path;
    temp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    try {
        Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) throw Error(1, system_error("创建临时文件"));
        DWORD written = 0;
        if (!WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size())
            throw Error(1, system_error("写入临时文件"));
        if (!FlushFileBuffers(file.get())) throw Error(1, system_error("刷新临时文件"));
    } catch (...) { DeleteFileW(temp.c_str()); throw; }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        auto code = GetLastError(); DeleteFileW(temp.c_str()); throw Error(1, system_error("替换配置文件", code));
    }
}
std::wstring quote_argument(std::wstring_view value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        out += c; slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
std::wstring current_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw Error(4, system_error("查询用户身份"));
    Handle token(raw);
    DWORD size = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size)) throw Error(4, system_error("读取用户身份"));
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text)) throw Error(1, system_error("读取用户 SID"));
    std::wstring sid(text); LocalFree(text); return sid;
}
DWORD session_id() {
    DWORD id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &id)) throw Error(1, system_error("读取会话"));
    return id;
}
ProcessLifetime process_lifetime(DWORD pid, uint64_t created) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? ProcessLifetime::ended : ProcessLifetime::unknown;
    auto wait = WaitForSingleObject(process.get(), 0);
    if (wait == WAIT_OBJECT_0) return ProcessLifetime::ended;
    if (wait != WAIT_TIMEOUT) return ProcessLifetime::unknown;
    FILETIME birth{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process.get(), &birth, &exit, &kernel, &user)) return ProcessLifetime::unknown;
    auto actual = (uint64_t(birth.dwHighDateTime) << 32) | birth.dwLowDateTime;
    return actual == created ? ProcessLifetime::running : ProcessLifetime::ended;
}
FileLock::FileLock(const fs::path& path) : file_([&]() {
    fs::create_directories(path.parent_path());
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}()) {
    if (!file_) throw Error(1, system_error("创建写入锁"));
    auto until = GetTickCount64() + 5000;
    while (!LockFileEx(file_.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap_)) {
        auto e = GetLastError();
        if (e != ERROR_LOCK_VIOLATION || GetTickCount64() >= until) throw Error(1, system_error("获取写入锁", e));
        Sleep(20);
    }
}
FileLock::~FileLock() { if (file_) UnlockFileEx(file_.get(), 0, 1, 0, &overlap_); }
}
