#pragma once
#include "platform/windows.hpp"
#include <vector>

namespace sinan {
inline uint64_t bridge_process_started(HANDLE process) {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(process, &c, &e, &k, &u)) throw Error(4, "无法验证进程创建时间");
    return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}
inline void verify_layout_sender(HMODULE module, DWORD pid, uint64_t started,bool read_only=false) {
    DWORD session{}, current{};
    if (!ProcessIdToSessionId(pid, &session) || !ProcessIdToSessionId(GetCurrentProcessId(), &current) || session != current)
        throw Error(4, "布局请求不属于当前会话");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process || bridge_process_started(process.get()) != started) throw Error(4, "布局客户端身份已变化");
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module, path, 32768)) throw Error(4, "无法验证布局 DLL 路径");
    std::wstring exe(32768, L'\0'); DWORD n = static_cast<DWORD>(exe.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, exe.data(), &n)) throw Error(4, "无法读取布局客户端路径");
    exe.resize(n);
    std::error_code ec;
    auto adjacent=fs::path(path).parent_path();
    bool agent=fs::equivalent(fs::path(exe),adjacent/L"sinan-agent.exe",ec)&&!ec;ec.clear();
    bool diagnostic=fs::equivalent(fs::path(exe),adjacent/L"sinan.exe",ec)&&!ec;
    if (!agent&&!(read_only&&diagnostic))
        throw Error(4, "Bridge requires adjacent sinan-agent.exe; CLI is limited to read-only probes");
    HANDLE peer_raw{}, own_raw{};
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &peer_raw)) throw Error(4, "无法验证布局客户端用户");
    Handle peer(peer_raw);
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own_raw)) throw Error(4, "无法读取布局进程用户");
    Handle own(own_raw);
    DWORD a{}, b{};
    GetTokenInformation(peer.get(), TokenUser, nullptr, 0, &a); GetTokenInformation(own.get(), TokenUser, nullptr, 0, &b);
    std::vector<BYTE> x(a), y(b);
    if (!GetTokenInformation(peer.get(), TokenUser, x.data(), a, &a) || !GetTokenInformation(own.get(), TokenUser, y.data(), b, &b) ||
        !EqualSid(reinterpret_cast<TOKEN_USER*>(x.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(y.data())->User.Sid))
        throw Error(4, "布局客户端用户不匹配");
}
}
