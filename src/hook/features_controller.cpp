#include "hook/features.hpp"
#include "hook/feature_admission.hpp"
#include "hook/identity.hpp"
#include "platform/explorer.hpp"
#include <sddl.h>
#include <shellapi.h>
#include <winternl.h>
#include <atomic>

namespace sinan {
bool is_shell_windows_broker(HANDLE process) {
    wchar_t system[MAX_PATH]{}, image[MAX_PATH]{};
    auto system_length=GetSystemDirectoryW(system,MAX_PATH);
    DWORD image_length=MAX_PATH;
    if(!system_length||system_length>=MAX_PATH||!QueryFullProcessImageNameW(process,0,image,&image_length)||
        _wcsicmp(image,(fs::path(system)/L"rundll32.exe").c_str())!=0)return false;
    using QueryProcess=NTSTATUS(NTAPI*)(HANDLE,PROCESSINFOCLASS,PVOID,ULONG,PULONG);
    const auto query=reinterpret_cast<QueryProcess>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess"));
    constexpr auto command_line=static_cast<PROCESSINFOCLASS>(60);
    alignas(UNICODE_STRING) BYTE buffer[4096]{};
    if(!query||query(process,command_line,buffer,sizeof(buffer),nullptr)<0)return false;
    const auto& value=*reinterpret_cast<const UNICODE_STRING*>(buffer);
    if(value.Buffer!=reinterpret_cast<const wchar_t*>(buffer+sizeof(value))||value.Length>sizeof(buffer)-sizeof(value))return false;
    const std::wstring command(value.Buffer,value.Length/sizeof(wchar_t));
    int count{};
    const auto args=CommandLineToArgvW(command.c_str(),&count);
    if(!args)return false;
    const bool broker=count==4&&_wcsicmp(args[1],L"shell32.dll,SHCreateLocalServerRunDll")==0&&
        _wcsicmp(args[2],L"{9BA05972-F6A8-11CF-A442-00A0C90A8F39}")==0&&_wcsicmp(args[3],L"-Embedding")==0;
    LocalFree(args);
    return broker;
}

namespace {
Json send_request(const NativeTab& tab, FeatureAction action, const Json& payload,bool require_active) {
    if(auto reason=feature_admission_error(*compat::current_profile(),action,payload,require_active);!reason.empty())throw Error(3,reason);
    auto serialized=payload.dump();
    if(serialized.size()>=sizeof(FeaturePacket::payload))throw Error(2,"Feature request too large");
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

Json send_feature_request(const NativeTab& tab, FeatureAction action, const Json& payload,bool require_active) {
    for(const auto& process:explorer_processes())
        if(process.pid==tab.process_id&&process.created==tab.process_started)return send_request(tab,action,payload,require_active);
    throw Error(4,"无法验证功能目标 Explorer 的用户、会话或进程身份");
}

Json send_external_activation_request(bool enabled,uint64_t cookie) {
    const auto sid=current_sid();
    const auto own_session=session_id();
    for(HWND window=nullptr;(window=FindWindowExW(nullptr,window,L"RunDLL",nullptr))!=nullptr;) {
        DWORD pid{}, session{};
        GetWindowThreadProcessId(window,&pid);
        if(!pid||!ProcessIdToSessionId(pid,&session)||session!=own_session)continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        if(!process||!is_shell_windows_broker(process.get())||process_user_sid(process.get())!=sid)continue;
        NativeTab target;
        target.window=window;
        target.process_id=pid;
        target.process_started=bridge_process_started(process.get());
        return send_request(target,FeatureAction::external_activation,{{"enabled",enabled},{"cookie",cookie}},false);
    }
    return {{"active",false},{"error","ShellWindows broker is unavailable"}};
}
}
