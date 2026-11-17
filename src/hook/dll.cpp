#include "hook/native.hpp"
#include "hook/protocol.hpp"
#include "config/config.hpp"
#include "formatter/formatter.hpp"
#include <MinHook.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>
#include <memory>

namespace tabtail {
namespace {
HMODULE self_module = nullptr;
using SetTitle = void(WINAPI*)(void*, PCWSTR, GUID);
SetTitle original_set_title = nullptr;
std::mutex policy_mutex;
std::mutex setup_mutex;
thread_local bool in_title = false;
thread_local UINT_PTR lease_timer_id = 0;
struct LivePolicy {
    NativeTab tab;
    RuntimePolicy display;
    DWORD owner = 0;
    uint64_t owner_started = 0;
    uint64_t expires = 0;
    DWORD thread = 0;
};
std::vector<LivePolicy> policies;
uint64_t process_started(HANDLE process) {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(process, &c, &e, &k, &u)) throw Error(4, "无法验证进程创建时间");
    return ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
}
void verify_sender(const BridgePacket& packet) {
    DWORD session{}, current{};
    if (!ProcessIdToSessionId(packet.sender_pid, &session) || !ProcessIdToSessionId(GetCurrentProcessId(), &current) || session != current)
        throw Error(4, "桥接请求不属于当前会话");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, packet.sender_pid));
    if (!process || process_started(process.get()) != packet.sender_started) throw Error(4, "桥接请求进程身份已变化");
    wchar_t dll_path[32768]{}; if (!GetModuleFileNameW(self_module, dll_path, 32768)) throw Error(4, "无法验证桥接 DLL 路径");
    std::wstring exe(32768, L'\0'); DWORD n = static_cast<DWORD>(exe.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, exe.data(), &n)) throw Error(4, "无法读取桥接客户端路径");
    exe.resize(n);
    std::error_code ec;
    if (!fs::equivalent(fs::path(exe), fs::path(dll_path).parent_path() / L"tabtail.exe", ec) || ec) throw Error(4, "桥接客户端路径不匹配");
    HANDLE peer_raw{}, current_raw{};
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &peer_raw)) throw Error(4, "无法验证桥接用户");
    Handle peer(peer_raw);
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_raw)) throw Error(4, "无法读取当前用户");
    Handle own(current_raw);
    DWORD a{}, b{}; GetTokenInformation(peer.get(), TokenUser, nullptr, 0, &a); GetTokenInformation(own.get(), TokenUser, nullptr, 0, &b);
    std::vector<BYTE> x(a), y(b);
    if (!GetTokenInformation(peer.get(), TokenUser, x.data(), a, &a) || !GetTokenInformation(own.get(), TokenUser, y.data(), b, &b) ||
        !EqualSid(reinterpret_cast<TOKEN_USER*>(x.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(y.data())->User.Sid)) throw Error(4, "桥接用户不匹配");
}
NativeTab locate(HWND window,HWND browser_window,uint64_t target_started,const GUID& guid) {
    DWORD pid = 0; auto thread = GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || thread != GetCurrentThreadId()) throw Error(4, "标题调用不在目标 UI 线程");
    auto tabs = read_native_tabs(window, pid, target_started);
    auto tab = std::find_if(tabs.begin(), tabs.end(), [&](const auto& t) {
        return t.browser_window == browser_window && t.guid == guid;
    });
    if (tab == tabs.end()) throw Error(3, "原生标签已关闭或身份变化");
    return *tab;
}
NativeTab locate(const BridgePacket& request) {
    return locate(reinterpret_cast<HWND>(request.window),reinterpret_cast<HWND>(request.browser_window),request.target_started,request.guid);
}
struct LiveLocation { std::wstring path; std::wstring original; };
LiveLocation location(const NativeTab& tab) {
    // Validated IBrowserHost vtable slot, resolved from the pinned build's public symbols.
    using GetItem = HRESULT(WINAPI*)(void*, REFIID, void**);
    auto get_item = reinterpret_cast<GetItem>(tab.module_base + native_rva("location"));
    Microsoft::WRL::ComPtr<IShellItem> item;
    auto result = get_item(reinterpret_cast<void*>(tab.host + 0x10), IID_PPV_ARGS(&item));
    if (FAILED(result) || !item) throw Error(3, "当前标签位置暂不可读取");
    LiveLocation out;
    PWSTR value = nullptr;
    result = item->GetDisplayName(SIGDN_NORMALDISPLAY, &value);
    if (SUCCEEDED(result) && value) out.original = value;
    CoTaskMemFree(value); value = nullptr;
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &value);
    if (SUCCEEDED(result) && value) out.path = value;
    CoTaskMemFree(value);
    if (out.original.empty()) throw Error(3, "当前标签原始显示名不可读取");
    return out;
}
std::wstring computed_title(const NativeTab& tab, const RuntimePolicy& display) {
    auto current = location(tab);
    if (current.path.empty()) return current.original;
    auto effective = display.resolve(current.path);
    if (effective.native) return current.original;
    return format_title(current.path, effective.display, current.original).text;
}
void restore_policy(const LivePolicy& policy) {
    auto tab = locate(policy.tab.window,policy.tab.browser_window,policy.tab.process_started,policy.tab.guid);
    auto current = location(tab);
    original_set_title(reinterpret_cast<void*>(tab.frame), current.original.c_str(), tab.guid);
}
void handle_title(void* frame, PCWSTR title, GUID guid) {
    if (in_title) { original_set_title(frame, title, guid); return; }
    struct Guard { Guard() { in_title = true; } ~Guard() { in_title = false; } } guard;
    std::optional<LivePolicy> policy;
    {
        std::lock_guard lock(policy_mutex);
        auto found = std::find_if(policies.begin(), policies.end(), [&](const auto& p) {
            return p.tab.frame == reinterpret_cast<uintptr_t>(frame) && p.tab.guid == guid && p.thread == GetCurrentThreadId();
        });
        if (found != policies.end() && found->expires > GetTickCount64()) policy = *found;
    }
    if (policy) {
        try {
            auto tab = locate(policy->tab.window,policy->tab.browser_window,policy->tab.process_started,guid);
            auto text = computed_title(tab, policy->display);
            original_set_title(frame, text.c_str(), guid); return;
        } catch (...) {} // Preserve Explorer's incoming title if the current location is unavailable.
    }
    original_set_title(frame, title, guid);
}
void WINAPI title_detour(void* frame, PCWSTR title, GUID guid) {
    // SEH is outside the function owning C++ objects.
    __try { handle_title(frame, title, guid); }
    __except(EXCEPTION_EXECUTE_HANDLER) { original_set_title(frame, title, guid); }
}
void ensure_hook(const NativeTab& tab) {
    std::lock_guard lock(setup_mutex);
    if (original_set_title) return;
    const BYTE expected[] = {0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    auto target = reinterpret_cast<void*>(tab.module_base + native_rva("title"));
    if (std::memcmp(target, expected, sizeof(expected))) throw Error(3, "标题入口已改变或被其他插件 Hook，未覆盖");
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) throw Error(1, "初始化 MinHook 失败");
    if (MH_CreateHook(target, reinterpret_cast<void*>(title_detour), reinterpret_cast<void**>(&original_set_title)) != MH_OK)
        throw Error(1, "创建标题 Hook 失败");
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<PCWSTR>(title_detour), &pinned)) throw Error(1, "固定标题 DLL 生命周期失败");
    if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); original_set_title = nullptr; throw Error(1, "启用标题 Hook 失败"); }
}
void CALLBACK lease_timer(HWND, UINT, UINT_PTR id, DWORD) {
    try {
        std::vector<LivePolicy> expired;
        {
            std::lock_guard lock(policy_mutex);
            auto now = GetTickCount64();
            for (auto it = policies.begin(); it != policies.end();) {
                if (it->thread == GetCurrentThreadId() && it->expires <= now) { expired.push_back(*it); it = policies.erase(it); }
                else ++it;
            }
        }
        for (auto& policy : expired) { try { restore_policy(policy); } catch (...) {} }
        bool active = false;
        { std::lock_guard lock(policy_mutex); for (const auto& p : policies) if (p.thread == GetCurrentThreadId()) active = true; }
        if (!active) { KillTimer(nullptr, id); lease_timer_id = 0; }
    } catch (...) {}
}
void execute(BridgePacket& request) {
    verify_sender(request);
    auto tab = locate(request);
    if (request.action == BridgeAction::probe) {
        auto current = location(tab);
        wcsncpy_s(request.title, current.original.c_str(), _TRUNCATE); request.result = S_OK; return;
    }
    if (request.action == BridgeAction::apply) {
        RuntimePolicy display = runtime_policy_from_json(std::string_view(request.policy, request.policy_size));
        auto text = computed_title(tab, display);
        ensure_hook(tab);
        if (!lease_timer_id) {
            lease_timer_id = SetTimer(nullptr, 0, 1000, lease_timer);
            if (!lease_timer_id) throw Error(1, "无法创建标题恢复计时器，未应用策略");
        }
        {
            std::lock_guard lock(policy_mutex);
            auto found = std::find_if(policies.begin(), policies.end(), [&](const auto& p) { return p.tab.frame == tab.frame && p.tab.guid == tab.guid; });
            if (found != policies.end() && (found->owner != request.sender_pid || found->owner_started != request.sender_started) && found->expires > GetTickCount64())
                throw Error(4, "该标签由另一个代理控制");
            LivePolicy policy{tab, display, request.sender_pid, request.sender_started, GetTickCount64() + 10000, GetCurrentThreadId()};
            if (found == policies.end()) policies.push_back(policy); else *found = policy;
        }
        original_set_title(reinterpret_cast<void*>(tab.frame), text.c_str(), tab.guid);
        wcsncpy_s(request.title, text.c_str(), _TRUNCATE);
        request.result = S_OK; return;
    }
    if (request.action == BridgeAction::restore) {
        bool owned = false;
        {
            std::lock_guard lock(policy_mutex);
            auto found = std::find_if(policies.begin(), policies.end(), [&](const auto& p) { return p.tab.frame == tab.frame && p.tab.guid == tab.guid; });
            if (found != policies.end()) {
                if (found->owner != request.sender_pid || found->owner_started != request.sender_started) throw Error(4, "不能恢复其他代理控制的标签");
                policies.erase(found); owned = true;
            }
        }
        auto current = location(tab);
        if (owned && original_set_title) original_set_title(reinterpret_cast<void*>(tab.frame), current.original.c_str(), tab.guid);
        wcsncpy_s(request.title, current.original.c_str(), _TRUNCATE); request.result = S_OK; return;
    }
    throw Error(2, "未知标题桥接操作");
}
void process_packet(BridgePacket* packet) {
    try {
        auto request = std::make_unique<BridgePacket>(*packet);
        if (request->magic != 0x54544252 || request->version != 3 || request->size != sizeof(BridgePacket) ||
            request->policy_size >= sizeof(request->policy)) throw Error(2, "桥接报文格式无效");
        execute(*request);
        packet->result = request->result;
        std::copy(std::begin(request->title), std::end(request->title), std::begin(packet->title));
    } catch (const Error& e) {
        packet->result = HRESULT_FROM_WIN32(e.code == 4 ? ERROR_ACCESS_DENIED : e.code==2 ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED);
        strncpy_s(packet->error,e.what(),_TRUNCATE);
    } catch (const std::exception& e) { packet->result=E_FAIL;strncpy_s(packet->error,e.what(),_TRUNCATE); }
    catch (...) { packet->result = E_FAIL;strcpy_s(packet->error,"标题适配器遇到未知错误"); }
    InterlockedExchange(&packet->completed, 1);
}
void protected_packet(BridgePacket* packet) {
    __try { process_packet(packet); }
    __except(EXCEPTION_EXECUTE_HANDLER) { packet->result = E_UNEXPECTED; InterlockedExchange(&packet->completed, 1); }
}
}
extern "C" __declspec(dllexport) LRESULT CALLBACK TabTailWindowHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (message && message->message == RegisterWindowMessageW(bridge_message_name)) {
            try {
                auto sender = static_cast<DWORD>(message->wParam); auto nonce = static_cast<uint64_t>(message->lParam);
                Handle mapping(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, bridge_mapping_name(sender, nonce).c_str()));
                if (mapping) {
                    auto* packet = static_cast<BridgePacket*>(MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(BridgePacket)));
                    if (packet) {
                        if (packet->nonce == nonce && packet->sender_pid == sender && packet->window == reinterpret_cast<uintptr_t>(message->hwnd)) protected_packet(packet);
                        UnmapViewOfFile(packet);
                    }
                }
            } catch (...) {}
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) tabtail::self_module = module;
    return TRUE;
}
