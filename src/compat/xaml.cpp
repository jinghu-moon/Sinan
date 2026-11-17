#include "compat/symbols.hpp"
#include <array>
#include <cstring>
#include <mutex>
#include <objbase.h>

namespace tabtail::compat {
namespace {
constexpr SymbolSpec view_specs[]{
    {"view_get","?get_FileExplorerTab@?$produce@UFileExplorerTabItemControlVM@implementation@FileExplorerExtensions@winrt@@UIFileExplorerTabItemControlVM@34@@impl@winrt@@UEAAHPEAPEAX@Z",true},
    {"view_vtable","??_7?$produce@UFileExplorerTabItemControlVM@implementation@FileExplorerExtensions@winrt@@UIFileExplorerTabItemControlVM@34@@impl@winrt@@6B@",false}
};
constexpr SymbolSpec ui_specs[]{
    {"tab_vtable","??_7?$produce@UFileExplorerTab@implementation@Shell@UI@WindowsUdk@winrt@@UIFileExplorerTab@3456@@impl@winrt@@6B@",false},
    {"tab_ctor","??0FileExplorerTab@implementation@Shell@UI@WindowsUdk@winrt@@QEAA@UFileExplorerTabData@@@Z",true},
    {"move_tab","?MoveTabIdToPosition@FileExplorerTabsController@implementation@Shell@UI@WindowsUdk@winrt@@UEAAJU_GUID@@I@Z",true}
};
bool known(const Image& image,Module module){return image.sha256==(module==Module::view?"abdab68ac72b9b45dbf2001e81232041c78cac4f33ea37db445e43df4919c6a3":"6753a3b5d1d25564eab5a4e785ce481d03cac83b25606f3c2305a1b4cdbb10ac");}
Profile cached(const Image& image,const fs::path& cache,Module module){
    auto path=manifest_path(cache,image);if(!fs::is_regular_file(path)){Profile p;p.identity=image.identity();p.reason="No verified module symbol profile";return p;}
    if(fs::file_size(path)>65536)throw Error(3,"Symbol manifest exceeds size limit");
    return load_manifest(image,Json::parse(read_file(path)),module);
}
template<class T>T read(uintptr_t address){T value{};SIZE_T size{};if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(value),&size)||size!=sizeof(value))throw Error(3,"Loaded module identity cannot be read");return value;}
}
const char* module_name(Module module){switch(module){case Module::frame:return "ExplorerFrame.dll";case Module::view:return "FileExplorerExtensions.dll";case Module::ui:return "Windows.UI.FileExplorer.dll";}throw Error(2,"Unknown symbol module");}
fs::path module_path(Module module){
    if(module==Module::frame)return frame_path();
    if(module==Module::ui)return frame_path().parent_path()/L"Windows.UI.FileExplorer.dll";
    wchar_t windows[32768]{};auto n=GetWindowsDirectoryW(windows,32768);if(!n||n>=32768)throw Error(1,"Cannot locate Windows directory");
    for(auto name:{L"MicrosoftWindows.Client.FileExp_cw5n1h2txyewy",L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy",L"MicrosoftWindows.Client.Core_cw5n1h2txyewy"}){
        auto path=fs::path(windows)/L"SystemApps"/name/L"FileExplorerExtensions.dll";if(fs::is_regular_file(path))return path;
    }
    throw Error(3,"FileExplorerExtensions.dll was not found in supported SystemApps locations");
}
std::span<const SymbolSpec> symbol_specs(Module module){if(module==Module::frame)return symbol_specs();if(module==Module::view)return view_specs;return ui_specs;}
std::shared_ptr<const Profile> current_xaml_profile(Module module){
    if(module==Module::frame)return current_profile();
    static std::mutex mutex;static std::array<std::shared_ptr<const Profile>,2> profiles;static std::array<uint64_t,2> retries{};
    std::lock_guard guard(mutex);auto index=module==Module::view?0:1;auto& selected=profiles[index];
    if(selected&&((selected->xaml_identity&& (module==Module::view||selected->reorder_extension))||GetTickCount64()<retries[index]))return selected;
    Profile p;
    try{Image image(read_file(module_path(module)));if(known(image,module)){
        p.identity=image.identity();p.baseline=p.xaml_identity=true;p.source="built-in exact module SHA256";p.reason="Known XAML module; live module and property readback remain required";
        if(module==Module::view){p.rvas={{"view_get",0xee230},{"view_vtable",0x4be510}};p.fields={{"view_get_slot",12}};}
        else{p.reorder_extension=true;p.rvas={{"tab_vtable",0x140d58},{"tab_ctor",0x6553c},{"move_tab",0x1015f0}};p.fields={{"tab_guid",0x78}};}
    }else p=cached(image,cache_directory(),module);}catch(const std::exception& ex){p.reason=ex.what();}
    retries[index]=GetTickCount64()+5000;
    if(selected&&(selected->xaml_identity||selected->reorder_extension)&&!compatible_extension(*selected,p))return selected;
    selected=std::make_shared<Profile>(std::move(p));return selected;
}
Json xaml_status(const fs::path& cache){
    Json reports=Json::object();for(auto module:{Module::view,Module::ui}){
        Json report;try{Image image(read_file(module_path(module)));report["image"]=image.identity();Profile p;
            try{p=cached(image,cache,module);report["cache_valid"]=p.xaml_identity||p.reorder_extension;}catch(const std::exception& ex){p.reason=ex.what();report["cache_valid"]=false;}
            auto baseline=known(image,module);report["identity_ready"]=baseline||p.xaml_identity;report["reorder_ready"]=module==Module::ui&&(baseline||p.reorder_extension);
            report["symbol_profile"]={{"identity",p.xaml_identity},{"reorder",p.reorder_extension},{"fields",p.fields},{"rvas",p.rvas},{"failures",p.failures},{"reason",p.reason}};
            report["adapter"]=baseline?"built-in exact module SHA256":p.source;
        }catch(const std::exception& ex){report["identity_ready"]=false;report["reorder_ready"]=false;report["error"]=ex.what();}
        reports[module_name(module)]=std::move(report);
    }return reports;
}
void verify_loaded_module(HMODULE module,const Profile& profile,const fs::path& expected_path){
    wchar_t path[32768]{};auto length=module?GetModuleFileNameW(module,path,32768):0;if(!length||length>=32768)throw Error(3,"Required Explorer module is not loaded");
    std::error_code ec;if(!fs::equivalent(fs::path(path),expected_path,ec)||ec)throw Error(3,"Loaded Explorer module path mismatch");
    auto base=reinterpret_cast<uintptr_t>(module);auto dos=read<IMAGE_DOS_HEADER>(base);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>0x100000)throw Error(3,"Invalid loaded module DOS header");
    auto nt=read<IMAGE_NT_HEADERS64>(base+dos.e_lfanew);auto size=profile.identity.at("image_size").get<uint32_t>();
    if(nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt.OptionalHeader.SizeOfImage!=size||nt.FileHeader.TimeDateStamp!=profile.identity.at("timestamp").get<uint32_t>())throw Error(3,"Loaded module PE identity mismatch");
    auto debug=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if(debug.Size>65536||debug.VirtualAddress>size||debug.Size>size-debug.VirtualAddress)throw Error(3,"Invalid loaded module debug directory");bool match=false;
    for(size_t i=0;i+sizeof(IMAGE_DEBUG_DIRECTORY)<=debug.Size;i+=sizeof(IMAGE_DEBUG_DIRECTORY)){
        auto entry=read<IMAGE_DEBUG_DIRECTORY>(base+debug.VirtualAddress+i);
        if(entry.Type!=IMAGE_DEBUG_TYPE_CODEVIEW||entry.SizeOfData<24||entry.AddressOfRawData>size||24>size-entry.AddressOfRawData)continue;
        auto bytes=read<std::array<BYTE,24>>(base+entry.AddressOfRawData);if(std::memcmp(bytes.data(),"RSDS",4))continue;
        GUID guid{};DWORD age{};std::memcpy(&guid,bytes.data()+4,16);std::memcpy(&age,bytes.data()+20,4);wchar_t text[40]{};StringFromGUID2(guid,text,40);
        match|=utf8(text)==profile.identity.at("pdb_guid").get<std::string>()&&age==profile.identity.at("pdb_age").get<DWORD>();
    }
    if(!match)throw Error(3,"Loaded module PDB identity mismatch; Windows may still be running an older image");
}
}
