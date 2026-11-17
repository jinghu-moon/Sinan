#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sinan {
namespace fs = std::filesystem;
struct Error : std::runtime_error {
    int code;
    Error(int c, const std::string& text) : std::runtime_error(text), code(c) {}
};
class Handle {
    HANDLE value_ = nullptr;
public:
    explicit Handle(HANDLE v = nullptr) : value_(v) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
};
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
std::string system_error(std::string_view operation, DWORD code = GetLastError());
std::wstring env(std::wstring_view key);
fs::path executable_path();
fs::path data_directory();
std::string read_file(const fs::path& path);
void atomic_write(const fs::path& path, std::string_view content);
std::wstring quote_argument(std::wstring_view value);
std::wstring current_sid();
DWORD session_id();
// Unknown includes access/query failures; only ended confirms a lifetime is gone.
enum class ProcessLifetime { running, ended, unknown };
ProcessLifetime process_lifetime(DWORD pid, uint64_t created);
class FileLock {
    Handle file_;
    OVERLAPPED overlap_{};
public:
    explicit FileLock(const fs::path& path);
    ~FileLock();
};
}
