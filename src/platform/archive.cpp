#include "platform/archive.hpp"
#include "platform/user_process.hpp"
#include "policy/entries.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>

namespace sinan {
namespace {
std::wstring folded(std::wstring value) {
    if(!value.empty())CharLowerBuffW(value.data(),static_cast<DWORD>(value.size()));
    return value;
}

ArchiveSource classify(const fs::path& path) {
    static const std::wregex numbered(LR"((.+)\.(7z|zip)\.([0-9]{3,}))");
    static const std::wregex rar_parts(LR"((.+)\.part([0-9]+)\.rar)");
    static const std::wregex zip_parts(LR"((.+)\.z([0-9]{2,}))");
    static const std::wregex rar_old(LR"((.+)\.r([0-9]{2}))");
    const auto name=path.filename().wstring();
    const auto lower=folded(name);
    std::wsmatch match;
    ArchiveSource source;
    source.selected=path;
    auto split=[&](std::string_view scheme,std::string_view format,int index) {
        const auto digits=utf8(match[index].str());
        auto [end,error]=std::from_chars(digits.data(),digits.data()+digits.size(),source.part);
        if(error!=std::errc{}||end!=digits.data()+digits.size())throw Error(2,"Invalid archive volume number");
        source.stem=name.substr(0,match[1].length());
        source.scheme=scheme;
        source.format=format;
    };
    if(std::regex_match(lower,match,numbered))split("numbered","."+utf8(match[2].str()),3);
    else if(std::regex_match(lower,match,rar_parts))split("rar-parts",".rar",2);
    else if(std::regex_match(lower,match,zip_parts))split("zip-disks",".zip",2);
    else if(std::regex_match(lower,match,rar_old)) { split("rar-old",".rar",2); ++source.part; }
    else {
        static constexpr std::wstring_view formats[]{L".tar.gz",L".tar.bz2",L".tar.xz",L".tar.zst",L".tgz",
            L".tbz2",L".txz",L".tar",L".zipx",L".zip",L".7z",L".rar",L".cab"};
        for(auto format:formats)if(lower.size()>format.size()&&lower.ends_with(format)) {
            source.stem=name.substr(0,name.size()-format.size());
            source.format=utf8(format);
            break;
        }
        if(source.format==".zip")source.scheme="zip-disks";
        if(source.format==".rar")source.scheme="rar-old";
    }
    if(source.format.empty())throw Error(2,"Choose a supported archive or archive volume");
    if(!source.part&&(std::regex_match(lower,numbered)||std::regex_match(lower,rar_parts)||std::regex_match(lower,zip_parts)))
        throw Error(2,"Archive volume numbering starts at 1");
    validate_folder_name(source.stem);
    source.key=directory_key(path.parent_path().wstring())+L"|"+folded(source.stem)+L"|"+wide(source.format)+L"|"+wide(source.scheme);
    return source;
}

std::optional<unsigned> zip_disks(const fs::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file||file.tellg()<22)return {};
    const auto length=static_cast<size_t>(std::min<std::streamoff>(file.tellg(),65557));
    std::vector<unsigned char> tail(length);
    file.seekg(-static_cast<std::streamoff>(length),std::ios::end);
    if(!file.read(reinterpret_cast<char*>(tail.data()),static_cast<std::streamsize>(length)))return {};
    const auto word=[&](size_t index){return static_cast<unsigned>(tail[index])|(static_cast<unsigned>(tail[index+1])<<8);};
    for(size_t end=length-21;end-- > 0;) {
        if(word(end)==0x4b50&&word(end+2)==0x0605&&end+22+word(end+20)==length) {
            const auto disk=word(end+4);
            if(disk!=0xffff)return disk+1;
            return {};
        }
    }
    return {};
}

bool legacy_rar_has_volumes(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    std::array<unsigned char,12> header{};
    constexpr std::array<unsigned char,7> signature{'R','a','r','!',0x1a,0x07,0x00};
    return file.read(reinterpret_cast<char*>(header.data()),static_cast<std::streamsize>(header.size()))&&
        std::equal(signature.begin(),signature.end(),header.begin())&&header[9]==0x73&&(header[10]&1)!=0;
}

std::vector<fs::path> volume_paths(const ArchiveSource& source) {
    if(source.scheme.empty())return {source.selected};
    std::map<unsigned,fs::path> candidates;
    for(const auto& file:fs::directory_iterator(source.selected.parent_path())) {
        std::optional<ArchiveSource> member;
        try { member=classify(file.path()); } catch(const Error&) { continue; }
        if(member->key==source.key&&!candidates.emplace(member->part,member->selected).second)
            throw Error(2,"Duplicate archive volume "+std::to_string(member->part));
    }
    if(source.scheme=="zip-disks"&&candidates.contains(0)) {
        if(const auto disks=zip_disks(candidates.at(0))) {
            if(*disks==1) {
                if(source.part)throw Error(2,"The matching ZIP is not a split ZIP archive");
                return {candidates.at(0)};
            }
            if(candidates.size()<*disks)throw Error(2,"Missing split ZIP volumes");
            if(candidates.size()>*disks)throw Error(2,"More split ZIP volumes were found than the archive declares");
        }
    }
    if(source.scheme=="rar-old"&&candidates.contains(0)&&!legacy_rar_has_volumes(candidates.at(0))) {
        if(source.part)throw Error(2,"The matching RAR is not a legacy split archive");
        return {candidates.at(0)};
    }
    const unsigned first=(source.scheme=="rar-parts"||source.scheme=="numbered")?1:0;
    std::vector<fs::path> paths;
    paths.reserve(candidates.size());
    unsigned expected=first;
    for(const auto& [part,path]:candidates) {
        if(part!=expected)throw Error(2,"Missing archive entry or volume "+std::to_string(expected));
        paths.push_back(path);
        ++expected;
    }
    if(paths.empty())throw Error(2,"Archive source no longer exists");
    return paths;
}

void canceled(std::stop_token stop) {
    if(stop.stop_requested())throw Error(7,"Archive job canceled");
}

class Stage {
    fs::path path_;
public:
    explicit Stage(const fs::path& parent) {
        GUID id{};
        if(FAILED(CoCreateGuid(&id)))throw Error(1,"Could not allocate a temporary archive directory");
        wchar_t name[40]{};
        StringFromGUID2(id,name,40);
        auto path=parent/(std::wstring(L".sinan-extract-")+name);
        if(!fs::create_directory(path))throw Error(1,"Temporary archive directory already exists");
        path_=std::move(path);
    }
    ~Stage() {std::error_code error;fs::remove_all(path_,error);}
    const fs::path& path()const{return path_;}
};

struct ToolOutput {
    DWORD code{};
    std::string bytes;
};
class SevenZipProgress {
    std::atomic<int>* value_;
    std::array<char, 8> prefix_{};
    size_t length_{};
public:
    explicit SevenZipProgress(std::atomic<int>* value):value_(value) {}
    void read(std::string_view bytes) {
        if(!value_)return;
        for(const auto byte:bytes) {
            if(byte=='\r'||byte=='\n'||byte=='\b') {
                std::string_view frame(prefix_.data(),length_);
                const auto start=frame.find_first_not_of(' ');
                if(start!=frame.npos) {
                    int percent{};
                    const auto end=frame.data()+frame.size();
                    const auto [next,error]=std::from_chars(frame.data()+start,end,percent);
                    if(error==std::errc{}&&next!=end&&*next=='%'&&
                        (next+1==end||next[1]==' ')&&percent>=0&&percent<=100&&
                        percent>value_->load(std::memory_order_relaxed))
                        value_->store(percent,std::memory_order_relaxed);
                }
                length_=0;
            } else if(length_<prefix_.size())prefix_[length_++]=byte;
        }
    }
};
ToolOutput run_tool(const fs::path& exe,const std::vector<std::wstring>& args,const fs::path& work,
                    std::stop_token stop,bool listing,std::atomic<int>* progress=nullptr) {
    canceled(stop);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE read_raw{},write_raw{};
    if(!CreatePipe(&read_raw,&write_raw,&inherit,0))throw Error(1,system_error("Create archive output pipe"));
    Handle read(read_raw);
    auto write=std::make_unique<Handle>(write_raw);
    if(!SetHandleInformation(read.get(),HANDLE_FLAG_INHERIT,0))throw Error(1,system_error("Configure archive output pipe"));
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,0,nullptr));
    Handle job(CreateJobObjectW(nullptr,nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!input||!job||!SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
        throw Error(1,system_error("Prepare archive child process"));

    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<std::byte> storage(size);
    auto list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(list,1,0,&size))throw Error(1,system_error("Prepare archive process handles"));
    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~Attributes(){DeleteProcThreadAttributeList(list);}
    } attributes{list};
    HANDLE handles[]{input.get(),write->get()};
    if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr))
        throw Error(1,system_error("Set archive process handles"));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow=SW_HIDE;
    startup.StartupInfo.hStdInput=input.get();
    startup.StartupInfo.hStdOutput=write->get();
    startup.StartupInfo.hStdError=write->get();
    startup.lpAttributeList=list;
    auto command=quote_argument(exe.wstring());
    for(const auto& arg:args)command+=L" "+quote_argument(arg);
    const UserProcessLauncher launcher;
    const auto child=launcher.launch_redirected(exe,std::move(command),work,startup);
    const auto& process=child.process;
    const auto& thread=child.thread;
    write.reset();
    if(!AssignProcessToJobObject(job.get(),process.get())||ResumeThread(thread.get())==DWORD(-1)) {
        const auto error=GetLastError();
        TerminateProcess(process.get(),1);
        WaitForSingleObject(process.get(),INFINITE);
        throw Error(1,system_error("Start cancellable archive tool",error));
    }
    std::stop_callback cancel(stop,[&]{TerminateJobObject(job.get(),7);});
    ToolOutput result;
    SevenZipProgress parser(progress);
    char buffer[8192];
    DWORD received{};
    while(ReadFile(read.get(),buffer,sizeof(buffer),&received,nullptr)&&received) {
        parser.read(std::string_view(buffer,received));
        result.bytes.append(buffer,received);
        if(listing&&result.bytes.size()>32*1024*1024) {
            TerminateJobObject(job.get(),1);
            WaitForSingleObject(process.get(),INFINITE);
            throw Error(2,"Archive listing exceeds 32 MiB");
        }
        if(!listing&&result.bytes.size()>256*1024)result.bytes.erase(0,result.bytes.size()-256*1024);
    }
    WaitForSingleObject(process.get(),INFINITE);
    canceled(stop);
    if(!GetExitCodeProcess(process.get(),&result.code))throw Error(1,system_error("Read archive tool result"));
    return result;
}

std::wstring decode(const std::string& bytes,UINT codepage) {
    if(bytes.empty())return {};
    if(codepage==1200) {
        if(bytes.size()%2)throw Error(2,"Incomplete Unicode output from archive tool");
        std::wstring text(bytes.size()/2,L'\0');
        memcpy(text.data(),bytes.data(),bytes.size());
        if(text.starts_with(L'\xfeff'))text.erase(0,1);
        return text;
    }
    const auto count=static_cast<int>(bytes.size());
    const auto length=MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,bytes.data(),count,nullptr,0);
    if(!length)throw Error(2,"Invalid character encoding in archive tool output");
    std::wstring text(length,L'\0');
    MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,bytes.data(),count,text.data(),length);
    return text;
}

template<class Fn> void each_line(std::wstring_view text,Fn fn) {
    while(!text.empty()) {
        const auto end=text.find(L'\n');
        auto line=text.substr(0,end);
        if(line.ends_with(L'\r'))line.remove_suffix(1);
        fn(line);
        if(end==text.npos)break;
        text.remove_prefix(end+1);
    }
}

void check_entry(std::wstring_view name) {
    if(name.starts_with(L'/')||name.starts_with(L'\\'))throw Error(2,"Archive contains an absolute path");
    while(!name.empty()) {
        const auto end=name.find_first_of(L"/\\");
        auto item=name.substr(0,end);
        if(!item.empty()&&item!=L".")validate_folder_name(item);
        if(end==name.npos)break;
        name.remove_prefix(end+1);
    }
}
void check_tree(const fs::path& root) {
    for(const auto& entry:fs::recursive_directory_iterator(root)) {
        const auto attributes=GetFileAttributesW(entry.path().c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||attributes&FILE_ATTRIBUTE_REPARSE_POINT)
            throw Error(2,"Extracted output contains a link or unreadable entry");
        validate_folder_name(entry.path().filename().wstring());
        if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)&&fs::hard_link_count(entry.path())>1)
            throw Error(2,"Extracted output contains a hard link");
    }
}

void check_exit(const ToolOutput& result,UINT codepage,std::string_view operation) {
    if(!result.code)return;
    std::string detail;
    try { detail=utf8(decode(result.bytes,codepage)); } catch(const Error&) {}
    if(detail.size()>1600) {
        detail.erase(0,detail.size()-1600);
        while(!detail.empty()&&(static_cast<unsigned char>(detail.front())&0xc0)==0x80)detail.erase(0,1);
    }
    std::replace(detail.begin(),detail.end(),'\r',' ');
    std::replace(detail.begin(),detail.end(),'\n',' ');
    throw Error(3,std::string(operation)+" (exit "+std::to_string(result.code)+")"+(detail.empty()?"":": "+detail));
}

struct Snapshot {
    fs::path path;
    BY_HANDLE_FILE_INFORMATION identity{};
    std::unique_ptr<Handle> lock;
    explicit Snapshot(fs::path file):path(std::move(file)),lock(std::make_unique<Handle>(
        CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ, nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr))) {
        if(!*lock||!GetFileInformationByHandle(lock->get(),&identity)||
           identity.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
            throw Error(3,system_error("Open archive volume "+utf8(path.filename().wstring())));
    }
    bool matches()const {
        Handle file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION current{};
        return file&&GetFileInformationByHandle(file.get(),&current)&&
            current.dwVolumeSerialNumber==identity.dwVolumeSerialNumber&&current.nFileIndexHigh==identity.nFileIndexHigh&&
            current.nFileIndexLow==identity.nFileIndexLow&&current.nFileSizeHigh==identity.nFileSizeHigh&&
            current.nFileSizeLow==identity.nFileSizeLow&&CompareFileTime(&current.ftLastWriteTime,&identity.ftLastWriteTime)==0;
    }
};

bool recycle_file(const fs::path& path) {
    wchar_t volume[MAX_PATH]{};
    if(!GetVolumePathNameW(path.c_str(),volume,MAX_PATH)||GetDriveTypeW(volume)!=DRIVE_FIXED)return false;
    SHQUERYRBINFO bin{sizeof(bin)};
    if(FAILED(SHQueryRecycleBinW(volume,&bin)))return false;
    Microsoft::WRL::ComPtr<IFileOperation> operation;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if(FAILED(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation)))||
       FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item))))return false;
    constexpr DWORD flags=FOFX_RECYCLEONDELETE|FOFX_ADDUNDORECORD|FOF_NOERRORUI|FOF_SILENT|FOF_WANTNUKEWARNING;
    BOOL aborted=TRUE;
    return SUCCEEDED(operation->SetOperationFlags(flags))&&SUCCEEDED(operation->DeleteItem(item.Get(),nullptr))&&
        SUCCEEDED(operation->PerformOperations())&&SUCCEEDED(operation->GetAnyOperationsAborted(&aborted))&&!aborted&&
        GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES;
}

std::vector<fs::path> children(const fs::path& path) {
    std::vector<fs::path> items;
    for(const auto& entry:fs::directory_iterator(path)) {
        items.push_back(entry.path());
        if(items.size()==2)break;
    }
    return items;
}
}

ArchiveSource archive_source(const std::vector<std::wstring>& selected) {
    if(selected.empty())throw Error(2,"Select an archive in Explorer");
    auto source=classify(selected.front());
    for(size_t i=1;i<selected.size();++i)
        if(classify(selected[i]).key!=source.key)throw Error(2,"Select one archive or one set of archive volumes");
    return source;
}

Json archive_tool(const Json& settings,std::wstring_view name,std::string_view format,bool split) {
    auto key=folded(std::wstring(name));
    if(split)key+=wide(std::string(format));
    auto id=settings.at("default_tool").get<std::string>();
    size_t longest{};
    for(const auto& [suffix,tool]:settings.at("formats").items()) {
        const auto extension=archive_extension(suffix);
        if(extension.size()>longest&&key.ends_with(wide(extension))) {
            id=tool.get<std::string>();
            longest=extension.size();
        }
    }
    for(const auto& tool:settings.at("tools"))if(tool.at("id")==id) {
        const auto type=tool.at("type").get<std::string>();
        if((type=="rar"&&format!=".rar")||(type=="tar"&&(split||format==".rar")))
            throw Error(2,"The configured archive tool does not support this format or volume layout");
        return tool;
    }
    throw Error(2,"Configure an archive tool for this format or choose a default archive tool");
}

ArchiveResult extract_archive(const ArchiveSource& source,const Json& settings,std::stop_token stop,std::atomic<int>* progress) {
    canceled(stop);
    auto volumes=volume_paths(source);
    const bool split=volumes.size()>1||source.part!=0;
    const auto name=split?source.stem:source.selected.filename().wstring();
    const auto tool=archive_tool(settings,name,source.format,split);
    const fs::path exe=wide(tool.at("path").get<std::string>());
    const auto type=tool.at("type").get<std::string>();
    if(!fs::is_regular_file(exe))throw Error(2,"Configured archive tool was not found");
    std::vector<Snapshot> snapshots;
    snapshots.reserve(volumes.size());
    for(const auto& path:volumes)snapshots.emplace_back(path);
    Stage stage(source.selected.parent_path());
    const auto data=stage.path()/L"output";
    fs::create_directory(data);
    const auto entry=volumes.front().wstring();
    bool verified_set=!split;
    if(type=="7zip") {
        const auto list=run_tool(exe,{L"l",L"-slt",L"-sccUTF-8",L"-spd",L"--",entry},stage.path(),stop,true);
        check_exit(list,CP_UTF8,"Could not list archive");
        bool entries=false;
        size_t volume_count{};
        each_line(decode(list.bytes,CP_UTF8),[&](std::wstring_view line) {
            if(line==L"----------"){entries=true;return;}
            if(!entries&&line.starts_with(L"Volumes = ")) {
                const auto count=utf8(line.substr(10));
                std::from_chars(count.data(),count.data()+count.size(),volume_count);
            }
            if(entries&&line.starts_with(L"Path = "))check_entry(line.substr(7));
            if(entries&&(line.starts_with(L"Symbolic Link = ")||line.starts_with(L"Hard Link = ")||line.starts_with(L"Alternate Stream = +")||
                         (line.starts_with(L"Attributes = ")&&line.find(L" lrwx")!=line.npos)))
                throw Error(2,"Archive contains a link or alternate stream");
            if(entries&&line==L"Encrypted = +")throw Error(2,"Archive requires a password");
        });
        if(!entries)throw Error(2,"Archive could not be inspected");
        check_exit(run_tool(exe,{L"x",L"-y",L"-sccUTF-8",L"-bsp1",L"-bso0",L"-bse2",L"-spd",L"-snl-",L"-snh-",L"-sns-",
            L"-o"+data.wstring(),L"--",entry},stage.path(),stop,false,progress),CP_UTF8,"Extraction failed");
        verified_set=!split||(volume_count==volumes.size()&&volume_count>=2);
    } else if(type=="rar") {
        const auto list=run_tool(exe,{L"lb",L"-v",L"-cfg-",L"-p-",L"-scur",L"--",entry},stage.path(),stop,true);
        check_exit(list,1200,"Could not list RAR archive");
        each_line(decode(list.bytes,1200),[](std::wstring_view line){if(!line.empty())check_entry(line);});
        const auto extraction=run_tool(exe,{L"x",L"-cfg-",L"-p-",L"-y",L"-o-",L"-ol-",L"-os-",L"-scur",
            L"--",entry,data.wstring()+L"\\"},stage.path(),stop,false);
        check_exit(extraction,1200,"Extraction failed");
        if(split) {
            const auto output=folded(decode(extraction.bytes,1200));
            verified_set=std::all_of(volumes.begin(),volumes.end(),[&](const auto& volume){
                return output.find(L" "+folded(volume.wstring())+L" ")!=output.npos;
            });
        }
    } else {
        auto listing=run_tool(exe,{L"-tvf",entry},stage.path(),stop,true);
        const auto codepage=GetACP();
        check_exit(listing,codepage,"Could not list archive");
        each_line(decode(listing.bytes,codepage),[](std::wstring_view line){
            if(!line.empty()&&line.front()!=L'-'&&line.front()!=L'd')throw Error(2,"Archive contains an unsupported entry");
        });
        listing=run_tool(exe,{L"-tf",entry},stage.path(),stop,true);
        check_exit(listing,codepage,"Could not list archive paths");
        each_line(decode(listing.bytes,codepage),[](std::wstring_view line){if(!line.empty())check_entry(line);});
        check_exit(run_tool(exe,{L"--no-same-owner",L"--no-same-permissions",L"-xf",entry,L"-C",data.wstring()},
            stage.path(),stop,false),codepage,"Extraction failed");
    }
    check_tree(data);
    auto top=children(data);
    if(top.empty())throw Error(2,"Archive is empty");
    const bool promote=top.size()==1&&fs::is_directory(top.front());
    const auto staged=promote?top.front():data;
    const auto stem=promote?staged.filename().wstring():source.stem;
    auto location=staged;
    fs::path leaf;
    for(auto entries=children(location);entries.size()==1;entries=children(location)) {
        if(!fs::is_directory(entries.front())) {leaf=entries.front();break;}
        location=entries.front();
    }
    const auto relative=location.lexically_relative(staged);
    canceled(stop);
    ArchiveResult result;
    for(unsigned index=1;;++index) {
        const auto suffix=index==1?L"":L" ("+std::to_wstring(index)+L")";
        result.output=source.selected.parent_path()/(stem+suffix);
        if(MoveFileExW(staged.c_str(),result.output.c_str(),0))break;
        const auto error=GetLastError();
        if(error!=ERROR_ALREADY_EXISTS&&error!=ERROR_FILE_EXISTS&&GetFileAttributesW(result.output.c_str())==INVALID_FILE_ATTRIBUTES)
            throw Error(1,system_error("Publish extracted files",error));
    }
    result.directory=relative==L"."?result.output:result.output/relative;
    if(!leaf.empty())result.file=result.directory/leaf.filename();
    if(!settings.at("recycle").get<bool>())return result;
    result.retained=volumes;
    bool same_set=false;
    try {same_set=volume_paths(source)==volumes;} catch(const std::exception&) {}
    if(!verified_set||!same_set||!std::all_of(snapshots.begin(),snapshots.end(),[](const auto& file){return file.matches();})) {
        result.recycle="unverified";
        return result;
    }
    result.retained.clear();
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Com { HRESULT status; ~Com(){if(SUCCEEDED(status))CoUninitialize();} } com_scope{com};
    for(auto& file:snapshots) {
        file.lock.reset();
        auto& paths=SUCCEEDED(com)&&file.matches()&&recycle_file(file.path)?result.recycled:result.retained;
        paths.push_back(file.path);
    }
    result.recycle=result.retained.empty()?"recycled":result.recycled.empty()?"retained":"partial";
    return result;
}
}
