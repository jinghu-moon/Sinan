#include "compat/symbols.hpp"
#include <bcrypt.h>
#include <objbase.h>
#include <array>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace sinan::compat {
namespace {
template<class T> T value(std::string_view data, size_t offset) {
    if(offset > data.size() || sizeof(T) > data.size()-offset) throw Error(3,"Truncated PE image");
    T out; std::memcpy(&out,data.data()+offset,sizeof(out)); return out;
}
}
std::string sha256(std::string_view bytes) {
    if(bytes.size()>ULONG_MAX)throw Error(3,"Image exceeds hash limit");
    std::array<UCHAR,32> digest{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),digest.data(),static_cast<ULONG>(digest.size()))<0)throw Error(1,"SHA256 failed");
    constexpr char hex[]="0123456789abcdef";std::string out;
    for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;
}
Image::Image(std::string data):bytes(std::move(data)) {
    if(bytes.size()>256*1024*1024)throw Error(3,"PE image exceeds size limit");
    auto dos=value<IMAGE_DOS_HEADER>(bytes,0);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>0x100000)throw Error(3,"Invalid DOS header");
    auto offset=static_cast<size_t>(dos.e_lfanew);
    if(value<DWORD>(bytes,offset)!=IMAGE_NT_SIGNATURE)throw Error(3,"Invalid PE signature");
    auto file=value<IMAGE_FILE_HEADER>(bytes,offset+4);machine=file.Machine;timestamp=file.TimeDateStamp;
    if(machine!=IMAGE_FILE_MACHINE_AMD64)throw Error(3,"Native adapter requires AMD64 (ARM64 is not x64)");
    if(file.SizeOfOptionalHeader<sizeof(IMAGE_OPTIONAL_HEADER64)||file.NumberOfSections==0||file.NumberOfSections>96)throw Error(3,"Invalid PE section table");
    auto opt=value<IMAGE_OPTIONAL_HEADER64>(bytes,offset+24);
    if(opt.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||opt.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_DEBUG||!opt.SizeOfImage||opt.SizeOfImage>0x40000000)throw Error(3,"Invalid PE64 optional header");
    image_size=opt.SizeOfImage;preferred_base=opt.ImageBase;
    exception_rva_=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
    exception_size_=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;
    for(size_t i=0;i<file.NumberOfSections;++i){
        auto s=value<IMAGE_SECTION_HEADER>(bytes,offset+24+file.SizeOfOptionalHeader+i*sizeof(IMAGE_SECTION_HEADER));
        auto size=(std::max)(s.Misc.VirtualSize,s.SizeOfRawData);
        if(s.VirtualAddress>image_size||size>image_size-s.VirtualAddress||s.PointerToRawData>bytes.size()||s.SizeOfRawData>bytes.size()-s.PointerToRawData)throw Error(3,"Invalid PE section bounds");
        for(auto& prior:sections_)if(size&&prior.size&&uint64_t(s.VirtualAddress)<uint64_t(prior.rva)+prior.size&&uint64_t(prior.rva)<uint64_t(s.VirtualAddress)+size)throw Error(3,"Overlapping PE sections");
        sections_.push_back({s.VirtualAddress,size,s.PointerToRawData,s.SizeOfRawData,s.Characteristics});
    }
    auto debug=opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if(debug.Size>65536||debug.Size%sizeof(IMAGE_DEBUG_DIRECTORY)||debug.VirtualAddress>image_size||debug.Size>image_size-debug.VirtualAddress)throw Error(3,"Invalid PE debug directory");
    for(size_t i=0;i<debug.Size;i+=sizeof(IMAGE_DEBUG_DIRECTORY)){
        IMAGE_DEBUG_DIRECTORY entry{};auto raw=at(debug.VirtualAddress+static_cast<uint32_t>(i),sizeof(entry));std::memcpy(&entry,raw.data(),sizeof(entry));
        if(entry.Type!=IMAGE_DEBUG_TYPE_CODEVIEW||entry.SizeOfData<25||entry.SizeOfData>4096)continue;
        auto cv=at(entry.AddressOfRawData,entry.SizeOfData);
        if(std::memcmp(cv.data(),"RSDS",4))continue;
        if(pdb_age)throw Error(3,"Ambiguous RSDS identity");
        std::memcpy(&pdb_guid,cv.data()+4,16);std::memcpy(&pdb_age,cv.data()+20,4);
        auto begin=reinterpret_cast<const char*>(cv.data()+24);auto end=static_cast<const char*>(memchr(begin,0,cv.size()-24));
        if(!end||!pdb_age||pdb_guid==GUID_NULL)throw Error(3,"Invalid RSDS record");
        pdb_name=fs::path(wide(std::string(begin,end))).filename().string();
        if(pdb_name.empty()||pdb_name.size()>100||pdb_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=std::string::npos)throw Error(3,"Unsafe PDB name");
    }
    if(!pdb_age)throw Error(3,"PE image has no RSDS identity");
    std::ostringstream key;key<<std::uppercase<<std::hex<<std::setfill('0')<<std::setw(8)<<pdb_guid.Data1<<std::setw(4)<<pdb_guid.Data2<<std::setw(4)<<pdb_guid.Data3;
    for(auto b:pdb_guid.Data4)key<<std::setw(2)<<unsigned(b);key<<pdb_age;pdb_key=key.str();sha256=compat::sha256(bytes);
}
std::span<const unsigned char> Image::at(uint32_t rva,size_t size) const {
    if(rva>image_size||size>image_size-rva)throw Error(3,"RVA outside image");
    for(auto& s:sections_)if(rva>=s.rva&&rva-s.rva<s.raw_size&&size<=s.raw_size-(rva-s.rva))
        return {reinterpret_cast<const unsigned char*>(bytes.data()+s.raw+(rva-s.rva)),size};
    throw Error(3,"RVA does not map to file-backed section");
}
bool Image::executable(uint32_t rva) const {for(auto& s:sections_)if(rva>=s.rva&&rva-s.rva<s.size)return (s.flags&IMAGE_SCN_MEM_EXECUTE)&&!(s.flags&IMAGE_SCN_MEM_WRITE);return false;}
bool Image::readonly_data(uint32_t rva) const {for(auto& s:sections_)if(rva>=s.rva&&rva-s.rva<s.size)return !(s.flags&(IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_WRITE))&&(s.flags&IMAGE_SCN_MEM_READ);return false;}
uint32_t Image::function_size(uint32_t rva)const {
    if(exception_size_>16*1024*1024||exception_size_%sizeof(RUNTIME_FUNCTION))throw Error(3,"Invalid x64 unwind table");
    auto table=at(exception_rva_,exception_size_);
    for(size_t i=0;i<table.size();i+=sizeof(RUNTIME_FUNCTION)){
        RUNTIME_FUNCTION f{};std::memcpy(&f,table.data()+i,sizeof(f));
        if(f.BeginAddress==rva&&f.EndAddress>rva&&f.EndAddress-rva<=4096)return f.EndAddress-rva;
    }throw Error(3,"Function has no bounded x64 unwind entry");
}
Json Image::identity()const {wchar_t guid[40]{};StringFromGUID2(pdb_guid,guid,40);return {{"sha256",sha256},{"machine",machine},{"image_size",image_size},{"timestamp",timestamp},{"pdb_name",pdb_name},{"pdb_key",pdb_key},{"pdb_guid",utf8(guid)},{"pdb_age",pdb_age}};}
fs::path frame_path(){wchar_t path[32768]{};auto n=GetSystemDirectoryW(path,32768);if(!n||n>=32768)throw Error(1,"Cannot locate System32");return fs::path(path)/L"ExplorerFrame.dll";}
fs::path cache_directory(){return data_directory()/L"cache"/L"symbols-v1";}
fs::path manifest_path(const fs::path& cache,const Image& image){return cache/wide(image.sha256)/L"profile.json";}
}
