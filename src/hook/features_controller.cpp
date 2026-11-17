#include "hook/features.hpp"
#include "hook/feature_admission.hpp"
#include "hook/identity.hpp"
#include "platform/explorer.hpp"
#include <sddl.h>
#include <atomic>

namespace sinan {
Json send_feature_request(const NativeTab& tab, FeatureAction action, const Json& payload,bool require_active) {
    auto serialized=payload.dump();
    if(serialized.size()>=sizeof(FeaturePacket::payload))throw Error(2,"Feature request too large");
    bool allowed = false;
    for (const auto& process : explorer_processes())
        if (process.pid == tab.process_id && process.created == tab.process_started) allowed = true;
    if (!allowed) throw Error(4, "无法验证功能目标 Explorer 的用户、会话或进程身份");
    if(auto reason=feature_admission_error(*compat::current_profile(),action,payload,require_active);!reason.empty())throw Error(3,reason);
    auto dll = executable_path().parent_path() / L"sinan-features.dll";
    auto module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw Error(1, system_error("加载 sinan-features.dll"));
    struct Module { HMODULE value; ~Module() { FreeLibrary(value); } } module_owner{module};
    auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "SinanFeatureHook"));
    if (!callback) throw Error(1, "功能 DLL 缺少桥接导出");
    DWORD pid{}; auto thread = GetWindowThreadProcessId(tab.window, &pid);
    if (pid != tab.process_id) throw Error(4, "功能目标窗口身份已变化");
    auto hook = SetWindowsHookExW(WH_CALLWNDPROC, callback, module, thread);
    if (!hook) throw Error(4, system_error("接入 Explorer 功能线程"));
    struct Hook { HHOOK value; ~Hook() { UnhookWindowsHookEx(value); } } hook_owner{hook};
    static std::atomic<uint64_t> counter{GetTickCount64() << 16}; auto nonce = ++counter;
    PSECURITY_DESCRIPTOR descriptor{};
    auto sddl = L"D:P(A;;GA;;;" + current_sid() + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        throw Error(4, "创建功能桥接权限失败");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(FeaturePacket), feature_mapping(GetCurrentProcessId(), nonce).c_str()));
    auto error = GetLastError(); LocalFree(descriptor);
    if (!mapping || error == ERROR_ALREADY_EXISTS) throw Error(1, "创建唯一功能映射失败");
    auto* packet = static_cast<FeaturePacket*>(MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(FeaturePacket)));
    if (!packet) throw Error(1, system_error("映射功能请求"));
    struct View { void* value; ~View() { UnmapViewOfFile(value); } } view{packet};
    new(packet) FeaturePacket;
    packet->nonce = nonce; packet->sender_pid = GetCurrentProcessId(); packet->sender_started = bridge_process_started(GetCurrentProcess());
    packet->target_started = tab.process_started; packet->window = reinterpret_cast<uintptr_t>(tab.window); packet->action = action;
    packet->browser_window=reinterpret_cast<uintptr_t>(tab.browser_window);packet->guid=tab.guid;packet->require_active=require_active;
    strcpy_s(packet->payload,serialized.c_str());
    DWORD_PTR ignored{};
    if (!SendMessageTimeoutW(tab.window, RegisterWindowMessageW(feature_message), GetCurrentProcessId(), static_cast<LPARAM>(nonce),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 4000, &ignored)) throw Error(5, "Explorer 功能请求超时，结果未确认");
    if (InterlockedCompareExchange(&packet->completed, 0, 0) != 1) throw Error(3, "Explorer 未确认功能请求");
    if (FAILED(packet->result))
        throw Error(3, system_error("Explorer 功能适配失败", static_cast<DWORD>(packet->result))+": "+std::string(packet->report,strnlen_s(packet->report,sizeof(packet->report))));
    auto report = Json::parse(std::string(packet->report, strnlen_s(packet->report, sizeof(packet->report))));
    report["pending"] = packet->result == E_PENDING;
    return report;
}
}
