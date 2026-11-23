#include "platform/shell_actions.hpp"
#include "policy/entries.hpp"
#include "platform/everything.hpp"
#include "platform/user_process.hpp"
#include "compat/symbols.hpp"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <tlhelp32.h>

namespace sinan {
using Microsoft::WRL::ComPtr;
namespace {
struct Clipboard {
    explicit Clipboard(HWND owner=nullptr) {
        if(!OpenClipboard(owner))throw Error(5,system_error("Open clipboard"));
    }
    ~Clipboard(){CloseClipboard();}
};
struct GlobalLockGuard {
    HGLOBAL memory;
    ~GlobalLockGuard(){GlobalUnlock(memory);}
};
struct WindowOwner {
    HWND window=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    ~WindowOwner(){if(window)DestroyWindow(window);}
};
void copy_text(const std::wstring& text) {
    WindowOwner owner;
    if(!owner.window)throw Error(5,system_error("Create clipboard owner"));
    Clipboard clipboard(owner.window);
    std::unique_ptr<void,decltype(&GlobalFree)> memory(GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t)),GlobalFree);
    if(!memory)throw Error(5,system_error("Allocate clipboard text"));
    auto out=GlobalLock(memory.get());
    if(!out)throw Error(5,system_error("Write clipboard text"));
    std::memcpy(out,text.c_str(),(text.size()+1)*sizeof(wchar_t));
    GlobalUnlock(memory.get());
    if(!EmptyClipboard()||!SetClipboardData(CF_UNICODETEXT,memory.get()))
        throw Error(5,system_error("Set clipboard text"));
    memory.release();
}
std::wstring view_path(IShellView& view) {
    TabLocation location;
    if(FAILED(shell_view_location(view,location))||!location.filesystem_path)throw Error(3,"Explorer tab has no filesystem directory");
    return *location.filesystem_path;
}
fs::path everything_executable() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    const auto session=session_id();
    const auto sid=current_sid();
    PROCESSENTRY32W entry{sizeof(entry)};
    if (snapshot&&Process32FirstW(snapshot.get(),&entry))do {
        if (_wcsicmp(entry.szExeFile,L"Everything.exe")!=0)continue;
        DWORD actual_session{};
        if(!ProcessIdToSessionId(entry.th32ProcessID,&actual_session)||actual_session!=session)continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID));
        if (!process)continue;
        try {
            if (process_user_sid(process.get())!=sid)continue;
        } catch (const Error&) { continue; }
        std::wstring path(32768,L'\0');
        DWORD length=static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process.get(),0,path.data(),&length)) {
            path.resize(length);
            return path;
        }
    } while (Process32NextW(snapshot.get(),&entry));

    for (const auto hive : {HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}) {
        wchar_t path[32768]{};
        DWORD bytes=sizeof(path);
        if (RegGetValueW(hive,L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Everything.exe",
            nullptr,RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,nullptr,path,&bytes)==ERROR_SUCCESS) {
            fs::path exe(path);
            if (exe.is_absolute()&&_wcsicmp(exe.filename().c_str(),L"Everything.exe")==0&&fs::is_regular_file(exe))return exe;
        }
    }
    for (const auto variable : {L"ProgramFiles",L"ProgramFiles(x86)"}) {
        fs::path base=env(variable);
        if (base.is_absolute()) {
            const auto exe=base/L"Everything"/L"Everything.exe";
            if (fs::is_regular_file(exe))return exe;
        }
    }
    throw Error(3,"找不到 Everything。请启动 Everything 后重试。 / Start Everything and retry.");
}

Json search_everything(const std::wstring& directory,std::wstring_view query) {
    directory_key(directory);
    const auto arguments=everything_search_argument(query);
    const auto exe=everything_executable();
    // Everything parses its own options. A trailing directory separator must stay literal.
    auto scope=directory;
    while(scope.size()>3&&(scope.back()==L'\\'||scope.back()==L'/'))scope.pop_back();
    UserProcessLauncher launcher;
    auto process=launcher.launch(exe,quote_argument(exe.wstring())+
        L" -new-window -path \""+scope+L"\" -search "+arguments);
    return {{"launch_requested",true},{"directory",utf8(directory)}};
}
void open_terminal(const std::wstring& path, HWND frame) {
    auto alias=fs::path(env(L"LOCALAPPDATA"))/L"Microsoft"/L"WindowsApps"/L"wt.exe";
    if(alias.is_relative()||GetFileAttributesW(alias.c_str())==INVALID_FILE_ATTRIBUTES)
        throw Error(3,"Windows Terminal app execution alias (wt.exe) is unavailable");
    // Windows Terminal parses semicolon-delimited commands inside its argument list.
    std::wstring escaped;
    for(auto c:path){if(c==L';')escaped+=L'\\';escaped+=c;}
    auto arguments=L"new-tab --startingDirectory "+quote_argument(escaped);
    SHELLEXECUTEINFOW request{sizeof(request)};
    request.fMask=SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
    request.hwnd=frame;request.lpFile=alias.c_str();request.lpParameters=arguments.c_str();
    request.lpDirectory=path.c_str();request.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&request))throw Error(5,system_error("Open Windows Terminal"));
}
}
ComPtr<IShellItemArray> shell_selection(IShellView& view, DWORD& count) {
    ComPtr<IFolderView2> folder;
    auto hr=view.QueryInterface(IID_PPV_ARGS(&folder));
    ComPtr<IShellItemArray> items;
    count=0;
    if(FAILED(hr))throw Error(3,"Explorer selection is unavailable");
    hr=folder->GetSelection(FALSE,&items);
    if(FAILED(hr)) {
        int selected{};
        if(SUCCEEDED(folder->ItemCount(SVGIO_SELECTION,&selected))&&selected==0)return {};
        throw Error(3,"Explorer selection is unavailable");
    }
    if(items&&FAILED(items->GetCount(&count)))
        throw Error(3,"Explorer selection is unavailable");
    if(count>10000)throw Error(2,"Too many selected items");
    return items;
}
std::vector<std::wstring> selected_paths(IShellView& view) {
    DWORD count{};
    const auto items=shell_selection(view,count);
    std::vector<std::wstring> paths;
    paths.reserve(count);
    for(DWORD index=0;index<count;++index) {
        ComPtr<IShellItem> item;
        PWSTR path{};
        auto hr=items->GetItemAt(index,&item);
        if(SUCCEEDED(hr))hr=item->GetDisplayName(SIGDN_FILESYSPATH,&path);
        std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> owned(path,CoTaskMemFree);
        if(FAILED(hr)||!path||!*path)throw Error(3,"A selected item has no filesystem path");
        paths.emplace_back(path);
    }
    return paths;
}
std::string selection_fingerprint(std::vector<std::wstring> paths) {
    std::sort(paths.begin(),paths.end());
    Json values=Json::array();
    for(const auto& path:paths)values.push_back(utf8(path));
    return compat::sha256(values.dump());
}
bool transfer_paths(const std::vector<std::wstring>& paths,const std::wstring& directory,bool move,HWND owner) {
    if(paths.empty())throw Error(2,"No selected files or folders");
    const auto target=directory_key(directory);
    auto check=[](HRESULT result,std::string_view operation) {
        if(FAILED(result))throw Error(5,system_error(operation,static_cast<DWORD>(result)));
    };
    ComPtr<IFileOperation> operation;
    check(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation)),"Open Windows file operation");
    check(operation->SetOwnerWindow(owner),"Set file operation owner");
    check(operation->SetOperationFlags(FOF_ALLOWUNDO|FOFX_ADDUNDORECORD|FOFX_EARLYFAILURE),"Set file operation flags");
    ComPtr<IShellItem> destination;
    check(SHCreateItemFromParsingName(directory.c_str(),nullptr,IID_PPV_ARGS(&destination)),"Open destination");
    SFGAOF attributes{};
    check(destination->GetAttributes(SFGAO_FOLDER|SFGAO_FILESYSTEM,&attributes),"Read destination");
    if((attributes&(SFGAO_FOLDER|SFGAO_FILESYSTEM))!=(SFGAO_FOLDER|SFGAO_FILESYSTEM))
        throw Error(3,"The destination must be a filesystem folder");
    for(const auto& path:paths) {
        const auto source=directory_key(path);
        const auto parent=directory_parent(path);
        if((parent&&directory_key(*parent)==target)||source==target||
            (target.starts_with(source)&&target.size()>source.size()&&(source.back()==L'\\'||target[source.size()]==L'\\')))
            throw Error(2,"The destination is the source folder or lies within a selected item");
        ComPtr<IShellItem> item;
        check(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item)),"Open selected item");
        const auto result=move?operation->MoveItem(item.Get(),destination.Get(),nullptr,nullptr):
            operation->CopyItem(item.Get(),destination.Get(),nullptr,nullptr);
        check(result,"Queue file operation");
    }
    check(operation->PerformOperations(),"Perform Windows file operation");
    BOOL aborted=FALSE;
    check(operation->GetAnyOperationsAborted(&aborted),"Read file operation result");
    return !aborted;
}
namespace {
Json copy_selection(IShellView& view) {
    const auto paths=selected_paths(view);
    if(paths.empty())throw Error(3,"Select at least one filesystem item in Explorer");
    std::wstring text;
    for(const auto& path:paths) {
        if(!text.empty())text+=L"\r\n";
        text+=path;
    }
    copy_text(text);
    return {{"copied",true},{"count",paths.size()}};
}
}
Json run_powershell_action(const fs::path& script, IShellView* view) {
    if(!fs::is_regular_file(script))throw Error(3,"PowerShell script is unavailable");
    wchar_t system_directory[MAX_PATH]{};
    const auto length=GetSystemDirectoryW(system_directory,MAX_PATH);
    if(!length||length>=MAX_PATH)throw Error(4,"Windows system directory is unavailable");
    const auto executable=fs::path(system_directory)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    const auto directory=view?view_path(*view):std::wstring{};
    Json paths=Json::array();
    if(view)for(const auto& path:selected_paths(*view))paths.push_back(utf8(path));
    const auto selected_count=paths.size();
    Json context{{"directory",directory.empty()?Json(nullptr):Json(utf8(directory))},{"selected_paths",std::move(paths)}};
    const std::pair<std::wstring,std::wstring> variable{L"SINAN_CONTEXT",wide(context.dump())};
    if(variable.second.size()+variable.first.size()+2>32767)
        throw Error(2,"PowerShell action context exceeds the Windows environment variable limit");
    UserProcessLauncher launcher;
    auto process=launcher.launch(executable,quote_argument(executable.wstring())+
        L" -NoLogo -NoProfile -NonInteractive -File "+quote_argument(script.wstring()),CREATE_NO_WINDOW,
        directory.empty()?script.parent_path():fs::path(directory),&variable);
    return {{"started",true},{"pid",GetProcessId(process.process.get())},
        {"directory",context.at("directory")},{"selected_count",selected_count}};
}
std::wstring everything_search_argument(std::wstring_view query) {
    std::wstring out=L"\"";
    for(auto ch:everything_search_group(query)) {
        if(ch==L'"')out+=L"\"\"";
        out+=ch;
    }
    return out+L'"';
}
std::wstring everything_search_group(std::wstring_view query) {
    if (query.size()>4096||query.find_first_not_of(L" \t\r\n")==query.npos)
        throw Error(2,"Enter an Everything search query of at most 4096 characters");
    bool quoted=false;
    unsigned depth=0;
    std::wstring out=L"<";
    for (size_t i=0;i<query.size();++i) {
        const auto ch=query[i];
        if (ch==L'\0'||ch==L'\r'||ch==L'\n')throw Error(2,"Search query must be one line");
        if (ch==L'"') {
            quoted=!quoted;
            out+=ch;
            continue;
        }
        // Numeric comparisons such as size:>1mb are not grouping tokens.
        const bool comparison=i>0&&i+1<query.size()&&query[i-1]==L':'&&
            ((query[i+1]>=L'0'&&query[i+1]<=L'9')||query[i+1]==L'=');
        if (!quoted&&!comparison) {
            if (ch==L'<')++depth;
            if (ch==L'>') {
                if (!depth)throw Error(2,"Unbalanced Everything search group");
                --depth;
            }
        }
        out+=ch;
    }
    if (quoted||depth)throw Error(2,"Unbalanced Everything search quotes or groups");
    return out+L'>';
}
Json toggle_file_extensions() {
    SHELLSTATE state{};
    SHGetSetSettings(&state, SSF_SHOWEXTENSIONS, FALSE);
    const bool show = !state.fShowExtensions;
    state.fShowExtensions = show;
    SHGetSetSettings(&state, SSF_SHOWEXTENSIONS, TRUE);
    SHELLFLAGSTATE observed{};
    SHGetSettings(&observed, SSF_SHOWEXTENSIONS);
    if (bool(observed.fShowExtensions) != show)
        throw Error(5, "Explorer did not confirm the file extension setting");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return {{"show_extensions",show}};
}
std::wstring clipboard_path() {
    std::wstring path;
    {
        Clipboard clipboard;
        if(auto drop=GetClipboardData(CF_HDROP)) {
            auto files=reinterpret_cast<HDROP>(drop);
            if(DragQueryFileW(files,0xFFFFFFFF,nullptr,0)!=1)throw Error(2,"Clipboard must contain exactly one path");
            auto length=DragQueryFileW(files,0,nullptr,0);
            path.resize(length+1);
            if(DragQueryFileW(files,0,path.data(),length+1)!=length)throw Error(5,"Could not read clipboard file");
            path.resize(length);
        }else {
            auto data=GetClipboardData(CF_UNICODETEXT);
            if(!data)throw Error(2,"Clipboard does not contain a path");
            auto text=static_cast<const wchar_t*>(GlobalLock(data));
            if(!text)throw Error(5,system_error("Read clipboard text"));
            GlobalLockGuard lock{data};
            const auto limit=GlobalSize(data)/sizeof(wchar_t);
            path.assign(text,wcsnlen_s(text,std::min<size_t>(limit,32768)));
        }
    }
    const auto start=path.find_first_not_of(L" \t\r\n");
    if(start==path.npos)throw Error(2,"Clipboard does not contain a path");
    path=path.substr(start,path.find_last_not_of(L" \t\r\n")-start+1);
    if(path.size()>=2&&path.front()==L'"'&&path.back()==L'"')path=path.substr(1,path.size()-2);
    directory_key(path);
    return fs::path(path).lexically_normal().make_preferred().wstring();
}
Json create_named_folder(const std::wstring& parent, const std::wstring& name) {
    directory_key(parent);
    validate_folder_name(name);
    auto path=(fs::path(parent).lexically_normal()/name).make_preferred();
    if(!CreateDirectoryW(path.c_str(),nullptr)) {
        const auto error=GetLastError();
        throw Error(error==ERROR_ALREADY_EXISTS||error==ERROR_FILE_EXISTS?2:5,system_error("Create folder",error));
    }
    return {{"created",true},{"path",utf8(path.wstring())}};
}
Json run_shell_view_action(IShellView& view, HWND frame, std::string_view action, const Json& parameters) {
    if(action=="explorer.copy_selection")return copy_selection(view);
    auto path=view_path(view);
    if(action=="explorer.search_everything")
        return search_everything(path,wide(parameters.at("query").get<std::string>()));
    if(action=="explorer.copy_directory") {
        copy_text(path);
        return {{"copied",true},{"path",utf8(path)}};
    }
    if(action=="explorer.open_terminal") {
        open_terminal(path,frame);
        return {{"launch_requested",true},{"path",utf8(path)}};
    }
    if(action!="explorer.new_folder")throw Error(2,"Unknown Explorer action");
    if(parameters.contains("name"))return create_named_folder(path,wide(parameters.at("name").get<std::string>()));
    const auto language=parameters.value("language",std::string("auto"));
    const bool chinese=language=="zh-CN"||(language=="auto"&&PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_CHINESE);
    const std::wstring base=chinese?L"新建文件夹":L"New folder";
    fs::path created;
    if(GetForegroundWindow()!=frame)throw Error(5,"Explorer left the foreground before folder creation");
    for(unsigned index=1;index<=1000;++index) {
        auto name=index==1?base:base+L" ("+std::to_wstring(index)+L")";
        auto candidate=fs::path(path)/name;
        if(CreateDirectoryW(candidate.c_str(),nullptr)) {
            created=std::move(candidate);
            break;
        }
        auto error=GetLastError();
        if(error!=ERROR_ALREADY_EXISTS&&error!=ERROR_FILE_EXISTS)throw Error(5,system_error("Create folder",error));
    }
    if(created.empty())throw Error(5,"No available default folder name");
    SHChangeNotify(SHCNE_MKDIR,SHCNF_PATHW|SHCNF_FLUSH,created.c_str(),nullptr);
    bool rename=false;
    PIDLIST_ABSOLUTE raw{};
    const auto hr=SHParseDisplayName(created.c_str(),nullptr,&raw,0,nullptr);
    std::unique_ptr<ITEMIDLIST,decltype(&CoTaskMemFree)> item(raw,CoTaskMemFree);
    if(SUCCEEDED(hr)&&item&&GetForegroundWindow()==frame)try {
        if(directory_key(view_path(view))==directory_key(path))
            rename=view.SelectItem(ILFindLastID(item.get()),SVSI_EDIT|SVSI_DESELECTOTHERS|SVSI_ENSUREVISIBLE|SVSI_FOCUSED)==S_OK;
    }catch(const std::exception&){}
    return {{"created",true},{"path",utf8(created.wstring())},{"rename_requested",rename},{"partial",!rename},
        {"message",rename?(chinese?"已创建文件夹，并请求进入重命名。":"Folder created and rename requested."):
            (chinese?"已创建文件夹，无法启动重命名。":"Folder created, but Explorer could not start rename.")}};
}
void reveal_file(const std::wstring& file, IShellView* existing_view) {
    PIDLIST_ABSOLUTE raw{};
    const auto hr=SHParseDisplayName(file.c_str(),nullptr,&raw,0,nullptr);
    std::unique_ptr<ITEMIDLIST,decltype(&CoTaskMemFree)> item(raw,CoTaskMemFree);
    if(FAILED(hr)||!item)throw Error(3,"Could not resolve file in Explorer");
    HRESULT selected{};
    if(existing_view) {
        if(directory_key(view_path(*existing_view))!=directory_key(fs::path(file).parent_path().wstring()))
            throw Error(5,"Explorer tab navigated before file selection");
        selected=existing_view->SelectItem(ILFindLastID(item.get()),SVSI_SELECT|SVSI_DESELECTOTHERS|SVSI_ENSUREVISIBLE|SVSI_FOCUSED);
    }else selected=SHOpenFolderAndSelectItems(item.get(),0,nullptr,0);
    if(FAILED(selected))throw Error(5,system_error("Reveal file in Explorer",static_cast<DWORD>(selected)));
}
}
