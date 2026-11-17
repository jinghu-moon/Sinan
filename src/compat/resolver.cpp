// Independent implementation using documented Windows APIs. No ExplorerPatcher code.
// Linked only into the symbol worker, never into injected Explorer adapters.
#include "compat/symbols.hpp"
#include <dbghelp.h>
#include <objbase.h>
#include <winhttp.h>
#include <array>
#include <cstring>
#include <fstream>

namespace sinan::compat {
namespace {
struct Internet{HINTERNET value{};~Internet(){if(value)WinHttpCloseHandle(value);}operator HINTERNET()const{return value;}};
std::string download(const Image& image){
    Internet session{WinHttpOpen(L"Sinan/" L"0.11 symbols",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.value)throw Error(5,system_error("Open Microsoft symbol connection"));
    WinHttpSetTimeouts(session,8000,8000,8000,8000);
    Internet connection{WinHttpConnect(session,L"msdl.microsoft.com",INTERNET_DEFAULT_HTTPS_PORT,0)};
    if(!connection.value)throw Error(5,system_error("Connect Microsoft symbols"));
    auto path=wide("/download/symbols/"+image.pdb_name+"/"+image.pdb_key+"/"+image.pdb_name);
    Internet request{WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    if(!request.value)throw Error(5,system_error("Request Microsoft PDB"));
    // Microsoft may redirect to its HTTPS CDN. WinHTTP's default policy rejects HTTPS->HTTP.
    DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if(!WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy)))throw Error(5,system_error("Set HTTPS redirect policy"));
    if(!WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,nullptr))throw Error(5,system_error("Download Microsoft PDB"));
    DWORD code{},size=sizeof(code);
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX)||code!=200)throw Error(5,"Microsoft symbol server HTTP "+std::to_string(code)+"; PDB may be unavailable for this build");
    std::string bytes;std::array<char,65536> chunk{};auto deadline=GetTickCount64()+70000;
    for(;;){DWORD got{};if(GetTickCount64()>deadline)throw Error(5,"Microsoft PDB transfer timed out");
        if(!WinHttpReadData(request,chunk.data(),static_cast<DWORD>(chunk.size()),&got))throw Error(5,system_error("Read PDB transfer"));
        if(!got)break;if(bytes.size()+got>128*1024*1024)throw Error(5,"PDB download exceeds 128 MiB limit");bytes.append(chunk.data(),got);
    }
    if(bytes.size()<32||!bytes.starts_with("Microsoft C/C++ MSF 7.00\r\n"))throw Error(5,"Symbol server did not return an MSF PDB");
    return bytes;
}
struct Symbols {
    HANDLE process=GetCurrentProcess();bool initialized=false;
    Symbols(const fs::path& directory){
        SymSetOptions(SYMOPT_EXACT_SYMBOLS|SYMOPT_FAIL_CRITICAL_ERRORS|SYMOPT_NO_PROMPTS|SYMOPT_IGNORE_NT_SYMPATH|SYMOPT_NO_UNQUALIFIED_LOADS);
        if(!SymInitializeW(process,directory.c_str(),FALSE))throw Error(5,system_error("Initialize DbgHelp"));initialized=true;
    }
    ~Symbols(){if(initialized)SymCleanup(process);}
};
struct Enumeration{Module target{};DWORD64 base{};uint32_t image_size{};Rvas rvas;std::map<std::string,unsigned> matches;};
BOOL CALLBACK collect(PSYMBOL_INFO symbol,ULONG,void* context){
    auto& e=*static_cast<Enumeration*>(context);std::string_view name(symbol->Name,symbol->NameLen);
    for(auto& spec:symbol_specs(e.target))if(name==spec.name){
        ++e.matches[spec.id];
        if(symbol->ModBase==e.base&&symbol->Address>=e.base&&symbol->Address-e.base<e.image_size)e.rvas[spec.id]=static_cast<uint32_t>(symbol->Address-e.base);
    }return TRUE;
}
}
Json resolve_module(const fs::path& cache,bool offline,bool force,Module target,const fs::path& local_pdb){
    bool network=false;try{
    auto image_path=module_path(target);Image image(read_file(image_path));auto directory=manifest_path(cache,image).parent_path();fs::create_directories(directory);
    FileLock lock(directory/L"prepare.lock");
    auto ready=[&](const Profile& p){return target==Module::frame?p.title&&p.selection&&p.close_guard&&p.opening&&p.navigation_guard&&p.reorder:target==Module::view?p.xaml_identity:p.xaml_identity&&p.reorder_extension;};
    auto report_for=[&](const Profile& p){auto report=profile_report(p);report["module"]=module_name(target);report["image"]=image.identity();report["xaml_identity"]=p.xaml_identity;report["reorder_extension"]=p.reorder_extension;report["prepared"]=ready(p);report["worker_isolated"]=true;return report;};
    if(!force&&local_pdb.empty()&&fs::exists(manifest_path(cache,image))){
        try{auto manifest=Json::parse(read_file(manifest_path(cache,image)));auto p=load_manifest(image,manifest,target);if(ready(p)&&manifest.value("contract_revision",0)==2){auto report=report_for(p);report["reused"]=true;report["network_used"]=false;report["missing_symbols"]=Json::array();return report;}}catch(...){}
    }
    auto pdb=directory/wide(image.pdb_name);
    // Stage PDB in an isolated directory so DbgHelp cannot silently select another file.
    auto staging=directory/(L"stage-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    if(!fs::create_directory(staging))throw Error(5,"Cannot create a private symbol staging directory");
    // DbgHelp can create search-cache children, especially for invalid input.
    // Own the entire newly created staging tree, not just the expected PDB.
    // Never recursively clean the shared module/cache directory.
    struct Cleanup{fs::path path;~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}} cleanup{staging};
    auto expected_pdb=fs::path(wide(module_name(target))).replace_extension(L".pdb");
    if(_wcsicmp(wide(image.pdb_name).c_str(),expected_pdb.c_str()))throw Error(3,"Unexpected module PDB name");
    auto staged=staging/wide(image.pdb_name);
    if(!local_pdb.empty()||((offline||!force)&&fs::is_regular_file(pdb))){
        auto source=local_pdb.empty()?pdb:local_pdb;
        if(!fs::is_regular_file(source)||fs::file_size(source)>128*1024*1024)throw Error(5,"Offline PDB missing or larger than 128 MiB");
        {
            std::ifstream input(source,std::ios::binary);std::array<char,32> header{};
            if(!input.read(header.data(),header.size())||!std::string_view(header.data(),header.size()).starts_with("Microsoft C/C++ MSF 7.00\r\n"))
                throw Error(5,"Offline PDB does not have an MSF 7.00 header");
        }
        fs::copy_file(source,staged,fs::copy_options::overwrite_existing);
    }else{if(offline)throw Error(5,"Offline PDB is missing; use sinan-symbols --pdb <file> or retry without --offline");network=true;atomic_write(staged,download(image));}
    Rvas rvas;Json missing=Json::array();
    {
        // All DbgHelp calls occur synchronously on this worker's only thread.
        Symbols session(fs::absolute(staging));
        auto base=SymLoadModuleExW(session.process,nullptr,image_path.c_str(),image_path.stem().c_str(),0x180000000,image.image_size,nullptr,0);
        if(!base)throw Error(5,system_error("Load exact module PDB"));
        IMAGEHLP_MODULEW64 module{};module.SizeOfStruct=sizeof(module);
        if(!SymGetModuleInfoW64(session.process,base,&module)||module.SymType!=SymPdb||module.PdbUnmatched||module.PdbSig70!=image.pdb_guid||module.PdbAge!=image.pdb_age)throw Error(3,"DbgHelp rejected exact PDB GUID/Age identity");
        std::error_code ec;if(!fs::equivalent(fs::path(module.LoadedPdbName),staged,ec)||ec)throw Error(3,"DbgHelp selected an unexpected PDB file");
        Enumeration enumeration{target,base,image.image_size,{},{}};
        if(!SymEnumSymbols(session.process,base,"*",collect,&enumeration))throw Error(5,system_error("Enumerate exact public symbols"));
        for(auto& spec:symbol_specs(target))if(enumeration.matches[spec.id]!=1||!enumeration.rvas.contains(spec.id)){missing.push_back({{"symbol",spec.id},{"matches",enumeration.matches[spec.id]}});enumeration.rvas.erase(spec.id);}
        rvas=std::move(enumeration.rvas);
    }
    // Re-read the system file before committing, in case servicing replaced it during download.
    if(sha256(read_file(image_path))!=image.sha256)throw Error(3,"Explorer module changed during symbol preparation; retry");
    auto manifest=make_manifest(image,rvas,sha256(read_file(staged)));auto profile=load_manifest(image,manifest,target);
    // A valid PDB with an unknown layout is useful evidence, but is never admitted as a working adapter.
    atomic_write(pdb,read_file(staged));atomic_write(manifest_path(cache,image),manifest.dump(2)+"\n");
    auto report=report_for(profile);report["network_used"]=network;report["missing_symbols"]=missing;report["worker_isolated"]=true;
    report["message"]=ready(profile)?"符号已准备，身份及布局校验通过；运行中的目标仍需逐次校验。":profile.reason;return report;
    }catch(const std::exception& ex){return {{"module",module_name(target)},{"prepared",false},{"network_used",network},{"worker_isolated",true},{"error",ex.what()},{"message",ex.what()}};}
}
Json resolve(const fs::path& cache,bool offline,bool force,const fs::path& local_pdb){return resolve_module(cache,offline,force,Module::frame,local_pdb);}
Json resolve_all(const fs::path& cache,bool offline,bool force){
    Json modules=Json::object();bool complete=true,any=false,network=false;
    for(auto target:{Module::frame,Module::view,Module::ui}){
        Json report;try{report=resolve_module(cache,offline,force,target);}catch(const std::exception& ex){report={{"prepared",false},{"error",ex.what()},{"message",ex.what()}};}
        bool ok=report.value("prepared",false);complete&=ok;any|=ok;network|=report.value("network_used",false);modules[module_name(target)]=std::move(report);
    }
    auto report=compat::status(cache);report["modules"]=modules;report["prepared"]=complete;report["complete"]=complete;report["partial"]=any&&!complete;report["network_used"]=network;report["worker_isolated"]=true;
    report["message"]=complete?"三个模块的符号及适配契约均已通过；实际功能仍需目标对象校验及读回确认。":"部分模块未准备完成；已保留通过的模块，请查看 modules 中的原因。";
    return report;
}
}
