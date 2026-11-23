#include "hook/native.hpp"
#include "compat/symbols.hpp"
#include <bcrypt.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <array>
#include <cstring>
#include <fstream>

namespace sinan {
namespace {
fs::path frame_path() {
    wchar_t path[32768]{}; auto n = GetSystemDirectoryW(path, 32768);
    if (!n || n >= 32768) throw Error(1, "无法读取系统目录");
    return fs::path(path) / L"explorerframe.dll";
}
template<class T> T read(HANDLE process, uintptr_t address) {
    T value{}; SIZE_T size = 0;
    if (!address || !ReadProcessMemory(process, reinterpret_cast<void*>(address), &value, sizeof(value), &size) || size != sizeof(value))
        throw Error(3, "Explorer 原生对象正在变化或布局不匹配");
    return value;
}
uintptr_t module_base(DWORD pid) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
    if (!snapshot) throw Error(4, system_error("读取 Explorer 模块"));
    MODULEENTRY32W item{sizeof(item)};
    if (!Module32FirstW(snapshot.get(), &item)) throw Error(3, "无法枚举 Explorer 模块");
    do {
        if (!_wcsicmp(item.szModule, L"explorerframe.dll")) {
            std::error_code error;
            if (!fs::equivalent(fs::path(item.szExePath), frame_path(), error) || error) throw Error(3, "ExplorerFrame 模块路径不匹配");
            return reinterpret_cast<uintptr_t>(item.modBaseAddr);
        }
    } while (Module32NextW(snapshot.get(), &item));
    throw Error(3, "ExplorerFrame 尚未加载");
}
void verify_image(HANDLE process, uintptr_t base, const compat::Profile& profile) {
    auto dos = read<IMAGE_DOS_HEADER>(process, base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x1000) throw Error(3, "ExplorerFrame PE 标头无效");
    auto nt = read<IMAGE_NT_HEADERS64>(process, base + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.TimeDateStamp != profile.identity.at("timestamp").get<DWORD>() || nt.OptionalHeader.SizeOfImage != profile.identity.at("image_size").get<DWORD>())
        throw Error(3, "ExplorerFrame 内存模块不匹配");
    auto debug = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (debug.Size > 4096 || debug.VirtualAddress > nt.OptionalHeader.SizeOfImage - debug.Size) throw Error(3, "ExplorerFrame 调试目录无效");
    bool matched = false;
    for (size_t off = 0; off + sizeof(IMAGE_DEBUG_DIRECTORY) <= debug.Size; off += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        auto entry = read<IMAGE_DEBUG_DIRECTORY>(process, base + debug.VirtualAddress + off);
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < 24 || entry.AddressOfRawData > nt.OptionalHeader.SizeOfImage - 24) continue;
        auto data = read<std::array<BYTE, 24>>(process, base + entry.AddressOfRawData);
        GUID guid{}; DWORD age{}; std::memcpy(&guid, data.data() + 4, 16); std::memcpy(&age, data.data() + 20, 4);
        if (!std::memcmp(data.data(), "RSDS", 4) && guid_text(guid) == profile.identity.at("pdb_guid").get<std::string>() && age == profile.identity.at("pdb_age").get<DWORD>()) matched = true;
    }
    if (!matched) throw Error(3, "ExplorerFrame PDB 身份不匹配");
}
}
std::string image_file_hash(const fs::path& path) {
    auto bytes = read_file(path);
    std::array<UCHAR, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0) throw Error(1, "计算 ExplorerFrame SHA-256 失败");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto b : digest) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}
std::string frame_file_hash(){return image_file_hash(frame_path());}
bool native_build_supported() {
    return compat::current_profile()->title;
}
bool native_inventory_supported(){return compat::current_profile()->inventory;}
bool native_features_supported(){auto p=compat::current_profile();return p->close_guard&&p->navigation_guard&&p->selection&&p->opening;}
bool native_selection_supported(){return compat::current_profile()->selection;}
uintptr_t native_rva(const char* key){return compat::current_profile()->rva(key);}
uint32_t native_field(const char* key){return compat::current_profile()->field(key);}
std::optional<GUID> read_active_tab(const NativeTab& tab) {
    if(!tab.frame||!native_selection_supported())return {};
    DWORD pid{};
    if(!GetWindowThreadProcessId(tab.window,&pid)||pid!=tab.process_id||
        static_cast<uintptr_t>(GetWindowLongPtrW(tab.window,0))!=tab.frame)return {};
    Handle process(OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    FILETIME created{},exited{},kernel{},user{};
    if(!process||!GetProcessTimes(process.get(),&created,&exited,&kernel,&user))return {};
    const auto started=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if(started!=tab.process_started)return {};
    try{return read<GUID>(process.get(),tab.frame+native_field("active_guid"));}
    catch(const Error&){return {};}
}
std::vector<NativeTab> read_native_tabs(HWND window, DWORD pid, uint64_t started) {
    auto profile=compat::current_profile();
    if (!profile->inventory) throw Error(3, "ExplorerFrame 兼容校验未通过: "+profile->reason);
    DWORD window_pid = 0; GetWindowThreadProcessId(window, &window_pid);
    if (!IsWindow(window) || window_pid != pid) throw Error(3, "Explorer 窗口身份已变化");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!process) throw Error(4, system_error("只读访问 Explorer"));
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process.get(), &created, &exit, &kernel, &user) ||
        (((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime) != started) throw Error(3, "Explorer 进程身份已变化");
    auto base = module_base(pid); verify_image(process.get(), base, *profile);
    const std::array<BYTE,11> location_code{0x48,0x83,0xc1,0x28,0x48,0x8b,0x01,0x48,0x8b,0x40,0x58};
    if(read<std::array<BYTE,11>>(process.get(),base+profile->rva("location"))!=location_code)throw Error(3,"Explorer location entry changed or is hooked");
    auto frame = static_cast<uintptr_t>(GetWindowLongPtrW(window, 0));
    if (read<uintptr_t>(process.get(), frame) != base + profile->rva("frame_vtable")) throw Error(3, "Explorer frame vtable 不匹配");
    auto bounds = read<std::array<uintptr_t, 2>>(process.get(), frame + 0x2f0);
    if (bounds[1] < bounds[0] || (bounds[1] - bounds[0]) % 40 || (bounds[1] - bounds[0]) / 40 > 512)
        throw Error(3, "Explorer 标签表边界无效");
    std::vector<NativeTab> out;
    for (auto entry = bounds[0]; entry < bounds[1]; entry += 40) {
        auto bytes = read<std::array<BYTE, 40>>(process.get(), entry);
        uintptr_t host{}; GUID guid{};
        std::memcpy(&host, bytes.data(), sizeof(host)); std::memcpy(&guid, bytes.data() + 16, sizeof(guid));
        if (read<uintptr_t>(process.get(), host) != base + profile->rva("host_vtable") ||
            read<uintptr_t>(process.get(), host + 0x10) != base + profile->rva("host_interface")) throw Error(3, "Explorer browser host vtable 不匹配");
        auto browser = read<HWND>(process.get(), host + 0x88);
        DWORD child_pid = 0; GetWindowThreadProcessId(browser, &child_pid);
        if (!IsWindow(browser) || child_pid != pid || !IsChild(window, browser) || guid == GUID_NULL) throw Error(3, "Explorer 原生标签对应关系无效");
        out.push_back({window, browser, pid, started, frame, host, base, guid});
        if (read<std::array<BYTE, 40>>(process.get(), entry) != bytes) throw Error(3, "Explorer 标签表在读取中变化");
    }
    if (read<std::array<uintptr_t, 2>>(process.get(), frame + 0x2f0) != bounds || static_cast<uintptr_t>(GetWindowLongPtrW(window, 0)) != frame)
        throw Error(3, "Explorer frame 在读取中变化");
    return out;
}
std::string guid_text(const GUID& guid) {
    wchar_t text[40]{}; StringFromGUID2(guid, text, 40); return utf8(text);
}
}
