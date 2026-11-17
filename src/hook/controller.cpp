#include "hook/controller.hpp"
#include "platform/explorer.hpp"
#include "config/config.hpp"
#include <sddl.h>
#include <atomic>
#include <cstring>
#include <tlhelp32.h>

namespace sinan {
std::vector<fs::path> conflicting_title_modules(DWORD pid,const fs::path& expected) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));
    if(!snapshot)throw Error(4,system_error("检查 Explorer 标题适配器"));
    MODULEENTRY32W entry{sizeof(entry)};std::vector<fs::path> conflicts;
    if(!Module32FirstW(snapshot.get(),&entry))throw Error(4,system_error("读取 Explorer 已加载模块"));
    do {
        if(_wcsicmp(entry.szModule,L"sinan-hook.dll")!=0)continue;
        std::error_code ec;
        if(!fs::equivalent(fs::path(entry.szExePath),expected,ec)||ec)conflicts.emplace_back(entry.szExePath);
    }while(Module32NextW(snapshot.get(),&entry));
    return conflicts;
}
BridgeResult send_title_request(const NativeTab& tab, BridgeAction action, const RuntimePolicy& policy) {
    validate_display(policy.defaults);
    auto serialized = action == BridgeAction::apply ? runtime_policy_json(policy) : std::string{};
    if (serialized.size() >= sizeof(BridgePacket::policy)) throw Error(2, "Runtime policy is too large");
    bool allowed = false;
    for (auto p : explorer_processes()) if (p.pid == tab.process_id && p.created == tab.process_started) allowed = true;
    if (!allowed) {
        if (process_lifetime(tab.process_id, tab.process_started) == ProcessLifetime::ended)
            throw Error(4, "目标 Explorer 已退出或进程已更换，请刷新标签列表");
        throw Error(4, "无法验证目标 Explorer 的用户、会话或进程身份");
    }
    if (!native_build_supported()) throw Error(3, "当前 ExplorerFrame 无标题适配");
    auto dll = executable_path().parent_path() / L"sinan-hook.dll";
    if(action==BridgeAction::apply) {
        auto conflicts=conflicting_title_modules(tab.process_id,dll);
        if(!conflicts.empty()) {
            std::string message="Explorer 中仍加载着其他路径的 Sinan 标题钩子：";
            for(auto& path:conflicts)message+="\n"+utf8(path.wstring());
            message+="\n请先用旧代理原路径停止代理，再重启 Explorer 释放驻留钩子，最后启动新版。";
            throw Error(3,message);
        }
    }
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw Error(1, system_error("加载 sinan-hook.dll"));
    struct Module { HMODULE value; ~Module() { FreeLibrary(value); } } module_owner{module};
    auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "SinanWindowHook"));
    if (!callback) throw Error(1, "标题 DLL 缺少桥接导出");
    DWORD pid{}; auto thread = GetWindowThreadProcessId(tab.window, &pid);
    if (pid != tab.process_id) throw Error(4, "目标窗口身份已变化");
    auto hook = SetWindowsHookExW(WH_CALLWNDPROC, callback, module, thread);
    if (!hook) throw Error(4, system_error("接入 Explorer UI 线程"));
    struct Hook { HHOOK value; ~Hook() { UnhookWindowsHookEx(value); } } hook_owner{hook};
    static std::atomic<uint64_t> counter{GetTickCount64() << 16};
    auto nonce = ++counter;
    PSECURITY_DESCRIPTOR descriptor{};
    auto sddl = L"D:P(A;;GA;;;" + current_sid() + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw Error(4, "创建桥接权限失败");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(BridgePacket), bridge_mapping_name(GetCurrentProcessId(), nonce).c_str()));
    auto mapping_error = GetLastError(); LocalFree(descriptor);
    if (!mapping || mapping_error == ERROR_ALREADY_EXISTS) throw Error(1, "创建唯一标题请求映射失败");
    auto* packet = static_cast<BridgePacket*>(MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(BridgePacket)));
    if (!packet) throw Error(1, system_error("映射标题请求"));
    struct View { void* value; ~View() { UnmapViewOfFile(value); } } view{packet};
    new(packet) BridgePacket;
    packet->nonce = nonce; packet->sender_pid = GetCurrentProcessId();
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) throw Error(1, "无法读取代理进程身份");
    packet->sender_started = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
    packet->target_started = tab.process_started; packet->window = reinterpret_cast<uintptr_t>(tab.window);
    packet->browser_window = reinterpret_cast<uintptr_t>(tab.browser_window); packet->guid = tab.guid;
    packet->action = action; packet->policy_size = static_cast<uint32_t>(serialized.size());
    std::copy(serialized.begin(), serialized.end(), packet->policy);
    DWORD_PTR ignored{};
    if (!SendMessageTimeoutW(tab.window, RegisterWindowMessageW(bridge_message_name), GetCurrentProcessId(), static_cast<LPARAM>(nonce),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 4000, &ignored)) throw Error(5, "Explorer 标题请求超时，结果未确认");
    if (InterlockedCompareExchange(&packet->completed, 0, 0) != 1) throw Error(3, "Explorer 未确认标题桥接请求");
    if (FAILED(packet->result)) {
        std::string detail(packet->error,strnlen_s(packet->error,sizeof(packet->error)));
        throw Error(3,system_error("Explorer 标题适配失败",static_cast<DWORD>(packet->result))+(detail.empty()?"":"\n"+detail));
    }
    return {packet->result, packet->title};
}
}
