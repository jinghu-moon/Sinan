#include "hook/identity.hpp"
#include "hook/layout.hpp"
#include "hook/selection_status.hpp"
#include "compat/symbols.hpp"
#include "platform/folder_size_cache.hpp"
#include <MinHook.h>
#include <intrin.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <atomic>
#include <memory>

namespace sinan::explorer_folder_sizes {
using namespace folder_size_ipc;
namespace {
HMODULE bridge_module{};
using ItemGetValue = HRESULT(*)(void*, const void*, int, REFPROPERTYKEY, PROPVARIANT*, int*);
using CompareItems = HRESULT(*)(IShellFolder2*, LPARAM, PCUIDLIST_RELATIVE, PCUIDLIST_RELATIVE);
using Format = decltype(&PSFormatForDisplayAlloc);
ItemGetValue item_get_value{};
CompareItems compare_items{};
Format format_for_display{};
const std::byte* storage_base{};
const std::byte* frame_base{};

// Microsoft PDBs for the admitted Windows 11 24H2 images.
constexpr uint32_t collection_get_value_rva = 0x99e90;
constexpr uint32_t folder_compare_rva = 0x38290;
constexpr uint32_t def_item_call_return_rva = 0x1bb917;
constexpr uint32_t ui_property_get_value_return_rva = 0x2478d;
constexpr uint32_t ui_property_format_return_rva = 0xd4117;

struct Channel {
    Cache* view{};
    FolderSizeSettings settings;
    std::wstring decimal;
    ~Channel() { if (view) UnmapViewOfFile(view); }
};
std::atomic<std::shared_ptr<const Channel>> channel;
struct PendingFormat {
    const PROPVARIANT* value{};
    uint64_t bytes{};
    std::weak_ptr<const Channel> channel;
};
thread_local PendingFormat pending_format;

bool leased(const Channel& candidate) {
    return GetTickCount64() < static_cast<uint64_t>(InterlockedCompareExchange64(&candidate.view->expires, 0, 0));
}

uint64_t find_or_request(Cache& cache, std::wstring_view path) {
    if (path.size() < 4 || path.size() >= cache_path_capacity || path[1] != L':' || path[2] != L'\\')
        return unknown;
    CacheLock lock(&cache);
    if (!lock || cache.count > cache_capacity) return unknown;
    const auto now = GetTickCount64();
    unsigned oldest = 0;
    for (unsigned i = 0; i < cache.count; ++i) {
        auto& entry = cache.entries[i];
        if (entry.path == path) {
            entry.requested = now;
            return entry.bytes;
        }
        if (entry.requested < cache.entries[oldest].requested) oldest = i;
    }
    auto& entry = cache.entries[cache.count < cache_capacity ? cache.count++ : oldest];
    std::copy(path.begin(), path.end(), entry.path);
    entry.path[path.size()] = L'\0';
    entry.bytes = unknown;
    entry.requested = now;
    entry.queried = 0;
    entry.successful_at = 0;
    entry.dirty = false;
    return unknown;
}

HRESULT collection_value(void* collection, const void* item, int mode, REFPROPERTYKEY key,
                         PROPVARIANT* value, int* value_state) noexcept {
    const auto caller = _ReturnAddress();
    const auto hr = item_get_value(collection, item, mode, key, value, value_state);
    if (key != PKEY_Size || !value || FAILED(hr) || (value->vt != VT_EMPTY && value->vt != VT_UI8) ||
        caller != storage_base + def_item_call_return_rva) return hr;
    try {
        const auto current = channel.load();
        if (!current || !leased(*current)) return hr;
        void* frames[2]{};
        if (CaptureStackBackTrace(1, 2, frames, nullptr) != 2 ||
            frames[1] != frame_base + ui_property_get_value_return_rva) return hr;
        struct Property {
            PROPVARIANT value{};
            ~Property() { PropVariantClear(&value); }
        } attributes, path;
        int property_state{};
        if (FAILED(item_get_value(collection, item, 0, PKEY_FileAttributes, &attributes.value, &property_state)) ||
            attributes.value.vt != VT_UI4 ||
            (attributes.value.ulVal & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY)
            return hr;
        if (FAILED(item_get_value(collection, item, 0, PKEY_ItemPathDisplay, &path.value, &property_state)) ||
            path.value.vt != VT_LPWSTR || !path.value.pwszVal) return hr;
        const auto bytes = find_or_request(*current->view, path.value.pwszVal);
        if (bytes == unknown) return hr;
        value->vt = VT_UI8;
        value->uhVal.QuadPart = bytes;
        pending_format = {value, bytes, current};
    } catch (...) {}
    return hr;
}

HRESULT compare_size(IShellFolder2* folder, LPARAM column, PCUIDLIST_RELATIVE left,
                     PCUIDLIST_RELATIVE right) noexcept {
    const auto native = compare_items(folder, column, left, right);
    const auto current = channel.load();
    if (FAILED(native) || !current || !leased(*current) || !left || !right ||
        (static_cast<uintptr_t>(column) & ~static_cast<uintptr_t>(SHCIDS_COLUMNMASK))) return native;
    PROPERTYKEY key{};
    if (FAILED(folder->MapColumnToSCID(static_cast<UINT>(column), &key)) || key != PKEY_Size) return native;
    auto path_for = [&](PCUIDLIST_RELATIVE item) -> std::wstring {
        const auto child = reinterpret_cast<PCUITEMID_CHILD>(item);
        STRRET display{};
        if (FAILED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &display))) return {};
        PWSTR path{};
        if (FAILED(StrRetToStrW(&display, child, &path)) || !path) return {};
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(path, CoTaskMemFree);
        return path;
    };
    try {
        const auto left_path = path_for(left);
        const auto right_path = path_for(right);
        uint64_t left_bytes = unknown, right_bytes = unknown;
        {
            CacheLock lock(current->view);
            if (!lock || current->view->count > cache_capacity) return native;
            for (unsigned i = 0; i < current->view->count; ++i) {
                const auto& entry = current->view->entries[i];
                if (entry.path == left_path) left_bytes = entry.bytes;
                if (entry.path == right_path) right_bytes = entry.bytes;
            }
        }
        if (left_bytes == right_bytes) return native;
        if (left_bytes == unknown || right_bytes == unknown) {
            auto child = reinterpret_cast<PCUITEMID_CHILD>(left_bytes == unknown ? left : right);
            SFGAOF attributes = SFGAO_FOLDER | SFGAO_FILESYSTEM;
            if (FAILED(folder->GetAttributesOf(1, &child, &attributes)) ||
                attributes != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) return native;
        }
        return left_bytes < right_bytes ? 0xffff : 1;
    } catch (...) {}
    return native;
}

HRESULT WINAPI display_size(REFPROPERTYKEY key, REFPROPVARIANT value,
                            PROPDESC_FORMAT_FLAGS flags, PWSTR* text) noexcept {
    if (text && key == PKEY_Size && value.vt == VT_UI8 && pending_format.value == &value &&
        pending_format.bytes == value.uhVal.QuadPart &&
        _ReturnAddress() == frame_base + ui_property_format_return_rva) {
        const auto current = pending_format.channel.lock();
        pending_format = {};
        if (current && leased(*current)) try {
            const auto formatted = format_folder_size(value.uhVal.QuadPart, current->settings, current->decimal);
            const auto bytes = (formatted.size() + 1) * sizeof(wchar_t);
            auto result = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
            if (result) {
                memcpy(result, formatted.c_str(), bytes);
                *text = result;
                return S_OK;
            }
        } catch (...) {}
    }
    return format_for_display(key, value, flags, text);
}

std::byte* checked_module(const wchar_t* name, std::string_view hash,
                          std::initializer_list<uint32_t> call_returns,
                          std::initializer_list<uint32_t> entries = {}) {
    const auto module = GetModuleHandleW(name);
    if (!module) throw Error(3, "Explorer 文件夹大小所需的模块尚未加载");
    wchar_t path[32768]{};
    const auto count = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!count || count == std::size(path)) throw Error(3, "无法验证 Explorer 模块路径");
    const compat::Image image(read_file(path));
    if (image.sha256 != hash) throw Error(3, "当前 Explorer 构建尚未通过文件夹大小兼容性验证");
    compat::Profile profile;
    profile.identity = image.identity();
    compat::verify_loaded_module(module, profile, path);
    for (auto rva : call_returns)
        if (!image.executable(rva) || memcmp(image.at(rva, 4).data(),
            reinterpret_cast<const std::byte*>(module) + rva, 4))
            throw Error(3, "Explorer 文件夹大小调用位置校验失败");
    for (auto entry : entries)
        if (!image.executable(entry) || image.function_size(entry, 8192) < 16 ||
            memcmp(image.at(entry, 16).data(), reinterpret_cast<const std::byte*>(module) + entry, 16))
            throw Error(3, "Explorer 文件夹大小属性入口校验失败");
    return reinterpret_cast<std::byte*>(module);
}

void install(HMODULE self) {
    const auto storage = checked_module(L"windows.storage.dll",
        "3e004a8a74e5cb4c637016b7bf7c5a8d1ae7c37f7bb4c577826ba378cedf2e1a",
        {def_item_call_return_rva}, {collection_get_value_rva});
    frame_base = checked_module(L"explorerframe.dll",
        "4e28d2076608df5525ec1b7b2ed8dc0e866f43bf2ded6a5e4213ccd1c2284cbe",
        {ui_property_get_value_return_rva, ui_property_format_return_rva});
    storage_base = storage;
    void* targets[]{storage + collection_get_value_rva, reinterpret_cast<void*>(PSFormatForDisplayAlloc),
                    storage + folder_compare_rva};
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        throw Error(3, "无法初始化文件夹大小适配");
    if (MH_CreateHook(targets[0], reinterpret_cast<void*>(collection_value),
                      reinterpret_cast<void**>(&item_get_value)) != MH_OK)
        throw Error(3, "无法接入 Explorer 大小属性");
    if (MH_CreateHook(targets[1], reinterpret_cast<void*>(display_size),
                      reinterpret_cast<void**>(&format_for_display)) != MH_OK) {
        MH_RemoveHook(targets[0]);
        throw Error(3, "无法接入 Explorer 大小单元格格式化");
    }
    // Another Explorer extension can already own the comparison entry. The
    // cell property's formatter and cache do not depend on that hook.
    constexpr unsigned char compare_prologue[]{0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
        0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac};
    const bool has_compare = !memcmp(targets[2], compare_prologue, sizeof(compare_prologue)) &&
        MH_CreateHook(targets[2], reinterpret_cast<void*>(compare_size),
                      reinterpret_cast<void**>(&compare_items)) == MH_OK;
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(self), &pinned) ||
        MH_QueueEnableHook(targets[0]) != MH_OK || MH_QueueEnableHook(targets[1]) != MH_OK ||
        (has_compare && MH_QueueEnableHook(targets[2]) != MH_OK) ||
        MH_ApplyQueued() != MH_OK) {
        if (has_compare) MH_RemoveHook(targets[2]);
        MH_RemoveHook(targets[1]);
        MH_RemoveHook(targets[0]);
        throw Error(3, "无法启用 Explorer 文件夹大小适配");
    }
}
}

void configure(HMODULE self, DWORD owner, uint64_t started, const FolderSizeSettings& settings) {
    const auto current = channel.load();
    if (current && current->settings == settings && leased(*current)) return;
    static const bool installed = (install(self), true);
    (void)installed;
    const Handle mapping(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                          cache_name(owner, started, GetCurrentProcessId()).c_str()));
    if (!mapping) throw Error(5, "无法打开代理的文件夹大小缓存");
    wchar_t decimal[12]{};
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, std::size(decimal)))
        decimal[0] = L'.';
    auto next = std::make_shared<Channel>();
    next->settings = settings;
    next->decimal = decimal;
    next->view = static_cast<Cache*>(MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Cache)));
    if (!next->view) throw Error(5, "无法映射代理的文件夹大小缓存");
    channel.store(std::move(next));
}

void process(LayoutPacket& packet, HWND window, DWORD sender, uint64_t nonce) noexcept {
    if (packet.magic != 0x54544c59 || packet.version != 2 || packet.size != sizeof(packet) ||
        (packet.action != LayoutAction::folder_sizes && packet.action != LayoutAction::selection_status) ||
        packet.sender_pid != sender ||
        packet.nonce != nonce || packet.window != reinterpret_cast<uintptr_t>(window)) return;
    try {
        verify_layout_sender(bridge_module, sender, packet.sender_started);
        DWORD process{};
        if (GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() ||
            process != GetCurrentProcessId() || bridge_process_started(GetCurrentProcess()) != packet.target_started)
            throw Error(4, "Explorer 视图身份发生变化");
        Json report{{"active", false}};
        try {
            const auto size = strnlen_s(packet.report, sizeof(packet.report));
            const auto request = Json::parse(packet.report, packet.report + size);
            if (packet.action == LayoutAction::selection_status) {
                report = update_selection_status(bridge_module, window, sender, request);
            } else {
                configure(bridge_module, sender, packet.sender_started, folder_sizes_from_json(request));
                report["active"] = true;
                report["numeric_sort"] = compare_items != nullptr;
            }
        } catch (const std::exception& error) {
            report["error"] = error.what();
        }
        strcpy_s(packet.report, report.dump().c_str());
        packet.result = S_OK;
    } catch (...) {
        packet.report[0] = 0;
        packet.result = E_ACCESSDENIED;
    }
    InterlockedExchange(&packet.completed, 1);
}
}

extern "C" __declspec(dllexport) LRESULT CALLBACK SinanLayoutHook(int code, WPARAM wparam, LPARAM lparam) {
    using namespace sinan;
    if (code >= 0 && lparam) {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (message.message == RegisterWindowMessageW(folder_size_message)) {
            try {
                const auto sender = static_cast<DWORD>(message.wParam);
                const auto nonce = static_cast<uint64_t>(message.lParam);
                const Handle mapping(OpenFileMappingW(FILE_MAP_WRITE, FALSE, layout_mapping(sender, nonce).c_str()));
                if (mapping) {
                    auto* packet = static_cast<LayoutPacket*>(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(LayoutPacket)));
                    if (packet) {
                        explorer_folder_sizes::process(*packet, message.hwnd, sender, nonce);
                        UnmapViewOfFile(packet);
                    }
                }
            } catch (...) {}
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) sinan::explorer_folder_sizes::bridge_module = module;
    return TRUE;
}
