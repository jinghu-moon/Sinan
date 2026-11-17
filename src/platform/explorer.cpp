#include "hook/controller.hpp"
#include "platform/explorer.hpp"
#include "compat/symbols.hpp"
#include "installer/environment.hpp"
#include "hook/native.hpp"
#include "platform/user_process.hpp"
#include <tlhelp32.h>
#include <sddl.h>
#include <vector>

namespace sinan {
namespace {
fs::path system_explorer() {
    std::wstring path(32768, L'\0');
    auto n = GetWindowsDirectoryW(path.data(), static_cast<UINT>(path.size()));
    if (!n || n >= path.size()) throw Error(1, system_error("获取 Windows 目录"));
    path.resize(n); return fs::path(path) / L"explorer.exe";
}
bool matches(HANDLE process, ULONGLONG* created = nullptr) {
    std::wstring path(32768, L'\0'); DWORD n = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &n)) return false;
    path.resize(n);
    if (normalize_path_entry(path) != normalize_path_entry(system_explorer().wstring())) return false;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return false;
    Handle token(raw); DWORD size = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size)) return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) return false;
    std::wstring value(sid); LocalFree(sid);
    if (value != current_sid()) return false;
    FILETIME birth, exit, kernel, user;
    if (!GetProcessTimes(process, &birth, &exit, &kernel, &user)) return false;
    if (created) *created = (static_cast<ULONGLONG>(birth.dwHighDateTime) << 32) | birth.dwLowDateTime;
    return true;
}
std::string version(const fs::path& file) {
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size) return "unknown";
    std::vector<BYTE> bytes(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, bytes.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info || length < sizeof(*info)) return "unknown";
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}
}
std::vector<ExplorerProcess> explorer_processes() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) throw Error(1, system_error("枚举进程"));
    PROCESSENTRY32W entry{sizeof(entry)};
    std::vector<ExplorerProcess> out;
    if (!Process32FirstW(snapshot.get(), &entry)) throw Error(1, system_error("读取进程列表"));
    do {
        if (_wcsicmp(entry.szExeFile, L"explorer.exe")) continue;
        DWORD session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != session_id()) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        ULONGLONG created = 0;
        if (process && matches(process.get(), &created)) out.push_back({entry.th32ProcessID, created});
    } while (Process32NextW(snapshot.get(), &entry));
    return out;
}
Json explorer_status() {
    Json out = Json::array();
    for (auto p : explorer_processes()) out.push_back({{"pid", p.pid}, {"created", p.created}, {"hook", "query_agent_for_state"}});
    return out;
}
Json system_diagnostics() {
    std::wstring system(32768, L'\0');
    auto n = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (!n || n >= system.size()) throw Error(1, system_error("获取系统目录"));
    system.resize(n);
    auto frame = fs::path(system) / L"explorerframe.dll";
    OSVERSIONINFOW os{sizeof(os)};
    using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto get_version = reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    std::string build = "unknown";
    if (get_version && get_version(&os) == 0) build = std::to_string(os.dwBuildNumber);
    Json title_conflicts=Json::array();
    for(auto process:explorer_processes())try{auto modules=conflicting_title_modules(process.pid,executable_path().parent_path()/L"sinan-hook.dll");if(!modules.empty()){Json paths=Json::array();for(auto& path:modules)paths.push_back(utf8(path.wstring()));title_conflicts.push_back({{"pid",process.pid},{"modules",paths}});}}catch(const std::exception& ex){title_conflicts.push_back({{"pid",process.pid},{"inspection_error",ex.what()}});}
    return {{"compatibility",compat::status()},{"title_adapter_conflicts",title_conflicts},{"windows_build", build},
        {"explorerframe_version", version(frame)}, {"architecture", "x64"}, {"session_id", session_id()},
        {"explorer", explorer_status()}, {"hook_available", native_build_supported()}, {"runtime_available", native_build_supported()},
        {"explorerframe_sha256", frame_file_hash()}, {"supported_sha256", supported_frame_sha256},
        {"hook_dll_present", fs::is_regular_file(executable_path().parent_path() / L"sinan-hook.dll")},
        {"message", native_build_supported() ? "标题适配已通过本地校验；start 时仍需校验目标模块、对象布局及标题入口。" : "当前无可用标题适配；可运行 symbols prepare 准备符号，观察与配置仍可用。"}};
}
Json restart_explorer(bool force) {
    if (!force) throw Error(3, "正常退出 Shell 的机制尚未验证；此次未重启。若接受强制结束 Explorer，请显式使用 --force。");
    UserProcessLauncher launcher;
    auto targets = explorer_processes();
    Json terminated = Json::array();
    for (auto target : targets) {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, target.pid));
        if (!process) { if (GetLastError() == ERROR_INVALID_PARAMETER) continue; throw Error(4, system_error("打开 Explorer")); }
        ULONGLONG created = 0;
        DWORD session = 0;
        if (!ProcessIdToSessionId(target.pid, &session) || session != session_id() || !matches(process.get(), &created) || created != target.created)
            throw Error(4, "Explorer 身份已变化，已中止重启");
        if (!TerminateProcess(process.get(), 0)) throw Error(4, system_error("结束 Explorer"));
        if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) throw Error(1, "等待 Explorer 结束超时");
        terminated.push_back(target.pid);
    }
    // Shell can restart automatically. Observe first, and never launch from PATH.
    auto until = GetTickCount64() + 3000;
    while (!GetShellWindow() && GetTickCount64() < until) Sleep(100);
    bool launched = false;
    if (!GetShellWindow()) {
        auto exe = system_explorer(); auto command = quote_argument(exe.wstring());
        try { auto process = launcher.launch(exe, std::move(command)); launched = true; }
        catch (const Error& e) { throw Error(e.code, std::string(e.what()) + "；可通过任务管理器运行 explorer.exe 恢复"); }
    }
    until = GetTickCount64() + 10000;
    while (!GetShellWindow() && GetTickCount64() < until) Sleep(100);
    if (!GetShellWindow()) throw Error(1, "Shell 恢复超时；可通过任务管理器运行 explorer.exe 恢复");
    return {{"shell_ready", true}, {"launched", launched}, {"terminated", terminated}, {"hook_restored", false},
        {"user_token_launch", launched && launcher.uses_user_token()},
        {"launch_elevated", launched ? Json(launcher.launches_elevated()) : Json(nullptr)},
        {"message", "Explorer 已重启；运行中的定制代理会重新扫描，窗口和标签恢复由 Windows 决定。"}};
}
}
