#include "hook/selection_status.hpp"
#include "hook/native.hpp"
#include <commctrl.h>
#include <memory>

namespace sinan {
namespace {
// CDUIViewFrame and its root element were verified against Microsoft's
// shell32 PDB. Reject other images before inspecting the private fields.
constexpr char shell_hash[] = "a1154ba9741a147ca34426143ad4a7c251aedf1e0b6e137a2036304980dab14b";
constexpr char direct_ui_hash[] = "b7331e6572f7bcdc3d11b68bbae734a3a46a04074cf298583e906648e308cc18";
constexpr uintptr_t view_frame_vtable = 0x5d9e78;
constexpr uintptr_t view_vtable = 0x5e9128;
constexpr uintptr_t label_vtable = 0x5c95f8;
constexpr uintptr_t root_offset = 0x80;
constexpr UINT_PTR subclass_id = 0x534e5353;

struct DirectUi {
    uintptr_t shell;
    ATOM (*id)(PCWSTR);
    void* (*find)(void*, ATOM);
    PCWSTR (*get)(void*, void**);
    void (*release)(void*);
    HRESULT (*set)(void*, PCWSTR);
};

HMODULE verified_module(const wchar_t* name, const char* hash) {
    const auto module = GetModuleHandleW(name);
    wchar_t path[MAX_PATH]{}, system[MAX_PATH]{};
    const auto length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!module || !length || length >= MAX_PATH || !GetSystemDirectoryW(system, MAX_PATH) ||
        _wcsicmp(path, (fs::path(system) / name).c_str()) || image_file_hash(path) != hash)
        throw Error(3, "当前 Windows 版本尚未验证选中项状态栏");
    return module;
}

template<class Function>
Function exported(HMODULE module, const char* name) {
    const auto function = GetProcAddress(module, name);
    if (!function) throw Error(3, "DirectUI 状态栏接口不可用");
    return reinterpret_cast<Function>(function);
}

const DirectUi& direct_ui() {
    static const auto api = [] {
        const auto shell = verified_module(L"shell32.dll", shell_hash);
        const auto dui = verified_module(L"dui70.dll", direct_ui_hash);
        return DirectUi{reinterpret_cast<uintptr_t>(shell),
            exported<decltype(DirectUi::id)>(dui, "StrToID"),
            exported<decltype(DirectUi::find)>(dui, "DUI70_ElementFindDescendent"),
            exported<decltype(DirectUi::get)>(dui, "?GetContentString@Element@DirectUI@@QEAAPEBGPEAPEAVValue@2@@Z"),
            exported<decltype(DirectUi::release)>(dui, "DUI70_ValueRelease"),
            exported<decltype(DirectUi::set)>(dui, "DUI70_ElementSetContentString")};
    }();
    return api;
}

bool has_vtable(const void* object, uintptr_t address) {
    return object && *static_cast<const uintptr_t*>(object) == address;
}

void* status_label(HWND window, const DirectUi& api) {
    wchar_t name[64]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (std::wstring_view(name) != L"DUIViewWndClassName") return nullptr;
    const auto frame = GetWindowLongPtrW(window, GWLP_USERDATA);
    if (!has_vtable(reinterpret_cast<void*>(frame), api.shell + view_frame_vtable)) return nullptr;
    const auto root = *reinterpret_cast<void**>(frame + root_offset);
    if (!has_vtable(root, api.shell + view_vtable)) return nullptr;
    const auto field = api.find(root, api.id(L"System.StatusBarSelectedItemCount"));
    const auto label = field ? api.find(field, api.id(L"PropertyValue")) : nullptr;
    return has_vtable(label, api.shell + label_vtable) ? label : nullptr;
}

std::wstring content(const DirectUi& api, void* label) {
    void* value{};
    const auto text = api.get(label, &value);
    const std::unique_ptr<void, decltype(api.release)> holder(value, api.release);
    return text ? text : L"";
}

struct StatusText {
    explicit StatusText(DWORD pid) : owner(OpenProcess(SYNCHRONIZE, FALSE, pid)), sender(pid) {}
    Handle owner;
    DWORD sender{};
    void* element{};
    std::wstring original;
    std::wstring applied;
};

bool restore(HWND window, const StatusText& state) {
    const auto& api = direct_ui();
    const auto label = status_label(window, api);
    return !label || label != state.element || content(api, label) != state.applied ||
        SUCCEEDED(api.set(label, state.original.c_str()));
}

LRESULT CALLBACK status_subclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR id, DWORD_PTR ref) {
    const auto state = reinterpret_cast<StatusText*>(ref);
    const bool closing = message == WM_NCDESTROY;
    bool remove = closing;
    if (message == WM_TIMER && wparam == id) {
        try {
            if (WaitForSingleObject(state->owner.get(), 0) == WAIT_OBJECT_0) remove = restore(window, *state);
        } catch (...) {}
    }
    if (remove) {
        KillTimer(window, id);
        RemoveWindowSubclass(window, status_subclass, id);
        delete state;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
}

Json update_selection_status(HMODULE self, HWND window, DWORD sender, const Json& request) {
    DWORD_PTR ref{};
    GetWindowSubclass(window, status_subclass, subclass_id, &ref);
    auto state = reinterpret_cast<StatusText*>(ref);
    if (state && state->sender != sender)
        throw Error(5, "状态栏由另一个代理控制");
    const auto text = wide(request.at("text").get<std::string>());
    if (text.empty()) {
        if (state) {
            if (!restore(window, *state)) throw Error(5, "无法恢复资源管理器状态栏");
            KillTimer(window, subclass_id);
            RemoveWindowSubclass(window, status_subclass, subclass_id);
            delete state;
        }
        return {{"active", false}};
    }
    if (text.size() > 256 || text.find_first_of(L"\r\n") != text.npos)
        throw Error(2, "无效的选中项状态栏文本");
    const auto& api = direct_ui();
    const auto label = status_label(window, api);
    if (!label) return {{"active", false}};
    auto native = content(api, label);
    if (native.empty()) return {{"active", false}};
    if (!state) {
        auto next = std::make_unique<StatusText>(sender);
        HMODULE pinned{};
        if (!next->owner || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<PCWSTR>(self), &pinned) ||
            !SetWindowSubclass(window, status_subclass, subclass_id, reinterpret_cast<DWORD_PTR>(next.get())))
            throw Error(5, "无法监视资源管理器状态栏");
        if (!SetTimer(window, subclass_id, 1000, nullptr)) {
            RemoveWindowSubclass(window, status_subclass, subclass_id);
            throw Error(5, "无法安排状态栏恢复");
        }
        state = next.release();
    }
    if (state->element == label && native == text && native == state->applied) return {{"active", true}};
    if (state->element != label || native != state->applied) state->original = std::move(native);
    if (FAILED(api.set(label, text.c_str()))) throw Error(5, "无法写入资源管理器状态栏");
    state->element = label;
    state->applied = text;
    return {{"active", true}};
}
}
