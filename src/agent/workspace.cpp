#include "agent/workspace.hpp"
#include <map>
#include <algorithm>
#include <set>
#include <future>
#include <thread>
#include <atomic>
#include "platform/user_process.hpp"

namespace tabtail {
bool workspace_directory_available(const std::wstring& path,DWORD timeout_ms) {
    // UNC and mapped drives can block indefinitely. Cap concurrent probes and
    // let timed-out requests finish without retaining any agent/COM objects.
    static std::atomic<unsigned> outstanding{};
    if(outstanding.fetch_add(1)>=4){--outstanding;return false;}
    std::promise<bool> promise;auto future=promise.get_future();
    try{std::thread([path,p=std::move(promise)]()mutable{auto attrs=GetFileAttributesW(path.c_str());p.set_value(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY));--outstanding;}).detach();}
    catch(...){--outstanding;throw;}
    auto end=GetTickCount64()+timeout_ms;
    while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){if(GetTickCount64()>=end)return false;MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}
    return future.get();
}
void open_workspace_window(const std::wstring& path) {
    wchar_t windows[32768]{};if(!GetWindowsDirectoryW(windows,32768))throw Error(1,"Cannot resolve Explorer");
    fs::path exe=fs::path(windows)/L"explorer.exe";UserProcessLauncher launcher;
    auto process=launcher.launch(exe,quote_argument(exe.wstring())+L" /n,"+quote_argument(path));
}

std::string workspace_path_key(std::string_view path) {
    auto p=wide(path);std::replace(p.begin(),p.end(),L'/',L'\\');
    while(p.size()>3&&p.back()==L'\\')p.pop_back();
    if(!p.empty())CharLowerBuffW(p.data(),static_cast<DWORD>(p.size()));return utf8(p);
}
Json save_workspace(const fs::path& root,const Json& tabs,int limit) {
    if(limit<1||limit>100)throw Error(2,"Invalid workspace limit");
    Json entries=Json::array(),skipped=Json::array();std::map<uint64_t,int> windows;std::set<std::pair<int,std::string>> seen;
    for(auto& t:tabs) {
        auto f=t.value("behavior",Json::object());
        if(!f.value("pinned",false)&&!f.value("protected",false)&&!f.value("locked",false))continue;
        if(!t.at("path").is_string()){skipped.push_back({{"reason","Virtual location"}});continue;}
        auto path=t.at("path").get<std::string>();auto window=t.at("window").get<uint64_t>();
        auto [it,added]=windows.emplace(window,static_cast<int>(windows.size()));auto group=it->second;
        if(!seen.insert({group,workspace_path_key(path)}).second){for(auto& existing:entries)if(existing.at("window_group")==group&&workspace_path_key(existing.at("path").get<std::string>())==workspace_path_key(path)){for(auto flag:{"pinned","protected","locked"})existing[flag]=existing.at(flag).get<bool>()||f.value(flag,false);break;}skipped.push_back({{"path",path},{"reason","Duplicate directory merged in window"}});continue;}
        if(entries.size()>=static_cast<size_t>(limit)){skipped.push_back({{"path",path},{"reason","Workspace limit"}});continue;}
        entries.push_back({{"path",path},{"window_group",group},{"pinned",f.value("pinned",false)},{"protected",f.value("protected",false)},
            {"locked",f.value("locked",false)},{"style",t.value("temporary_style",Json::object())}});
    }
    FileLock lock(root/L"workspace.lock");
    atomic_write(root/L"workspace.json",Json{{"schema_version",1},{"entries",entries}}.dump(2));
    return {{"saved",true},{"count",entries.size()},{"skipped",skipped}};
}
Json load_workspace(const fs::path& root,int limit) {
    if(limit<1||limit>100)throw Error(2,"Invalid workspace limit");
    auto path=root/L"workspace.json";if(!fs::exists(path))return Json::array();
    auto bytes=read_file(path);if(bytes.size()>1024*1024)throw Error(2,"Workspace exceeds 1 MiB");
    auto j=Json::parse(bytes);if(j.at("schema_version")!=1||!j.at("entries").is_array()||j.at("entries").size()>100)throw Error(2,"Invalid workspace file");
    Json result=Json::array();std::set<std::pair<int,std::string>> seen;
    for(auto& e:j.at("entries")) {
        auto p=e.at("path").get<std::string>();if(!e.at("window_group").is_number_integer()||e.at("window_group")<0||e.at("window_group")>=100)throw Error(2,"Invalid workspace group");auto group=e.at("window_group").get<int>();
        if(p.empty()||p.find(char(0))!=p.npos||p.size()>32760||group<0||group>=100)throw Error(2,"Invalid workspace directory");
        auto w=wide(p);if(!(w.size()>=3&&w[1]==L':'&&w[2]==L'\\')&&!w.starts_with(L"\\\\"))throw Error(2,"Workspace requires an absolute path");
        e.at("pinned").get<bool>();e.at("protected").get<bool>();e.at("locked").get<bool>();validate_style(e.at("style"));
        if(result.size()>=static_cast<size_t>(limit)||!seen.insert({group,workspace_path_key(p)}).second)continue;
        // Never deserialize an old native GUID, HWND or runtime token as identity.
        Json clean{{"path",p},{"window_group",group},{"pinned",e.at("pinned")},{"protected",e.at("protected")},{"locked",e.at("locked")},{"style",e.at("style")}};
        result.push_back(clean);
    }
    return result;
}
}
