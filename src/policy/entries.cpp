#include "policy/entries.hpp"
#include <algorithm>
#include <cwctype>
#include <set>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace tabtail {
namespace {
[[noreturn]] void invalid(const char* text){throw PolicyError(PolicyErrorCode::invalid_argument,text);}
std::wstring normalize(std::wstring_view input) {
    std::wstring p(input);for(auto& c:p)if(c==L'/')c=L'\\';
    if(p.starts_with(L"\\\\?\\UNC\\"))p=L"\\\\"+p.substr(8);
    else if(p.starts_with(L"\\\\?\\"))p.erase(0,4);
    if(p.empty()||p.find(L'\0')!=p.npos||p.size()>32760)invalid("Invalid directory path");
    size_t begin=0;std::wstring root;
    if(p.size()>=3&&((p[0]>=L'A'&&p[0]<=L'Z')||(p[0]>=L'a'&&p[0]<=L'z'))&&p[1]==L':'&&p[2]==L'\\'){root=p.substr(0,3);begin=3;}
    else if(p.starts_with(L"\\\\")&&!p.starts_with(L"\\\\.\\")) {
        auto server=p.find(L'\\',2);auto share=server==p.npos?p.npos:p.find(L'\\',server+1);
        if(server==p.npos||server==2||server+1==p.size()||share==server+1)invalid("UNC directory needs server and share");
        root=p.substr(0,share);begin=share==p.npos?p.size():share+1;
        if(root.substr(2).find_first_of(L"<>:\"|?*")!=root.npos||std::any_of(root.begin(),root.end(),[](auto c){return c<32;}))invalid("Invalid UNC root");
    }else invalid("Directory must be an absolute drive or UNC path");
    std::vector<std::wstring> parts;
    while(begin<p.size()) {
        auto end=p.find(L'\\',begin);if(end==p.npos)end=p.size();auto part=p.substr(begin,end-begin);
        if(part==L".."){if(!parts.empty())parts.pop_back();}
        else if(!part.empty()&&part!=L"."){
            if(part.find_first_of(L"<>:\"|?*")!=part.npos||part.back()==L'.'||part.back()==L' '||std::any_of(part.begin(),part.end(),[](auto c){return c<32;}))invalid("Invalid directory component");
            parts.push_back(part);
        }begin=end+1;
    }
    for(auto& part:parts){if(root.back()!=L'\\')root+=L'\\';root+=part;}
    return root;
}
}
std::wstring directory_key(std::wstring_view path) {
    auto p=normalize(path);CharUpperBuffW(p.data(),static_cast<DWORD>(p.size()));return p;
}
void validate_entries(const std::vector<DirectoryEntry>& entries) {
    if(entries.size()>128)invalid("At most 128 directory entries are supported");
    std::set<std::string> ids;std::set<std::wstring> paths;
    for(auto& e:entries) {
        if(e.id.empty()||e.id.size()>64||!std::all_of(e.id.begin(),e.id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';})||!ids.insert(e.id).second)invalid("Entry IDs must be unique ASCII letters, digits, - or _ (1..64)");
        if(e.name.empty()||e.name.size()>128||std::any_of(e.name.begin(),e.name.end(),[](auto c){return c<32;}))invalid("Entry name must be 1..128 characters without control characters");
        auto path=directory_key(e.path);if(e.enabled&&!paths.insert(path).second)invalid("Enabled directory entries must have unique paths");
        if(e.title!="off"&&e.title!="alias"&&e.title!="alias_depth")invalid("Title mode must be off, alias or alias_depth");
    }
}
std::optional<std::wstring> alias_title(const std::vector<DirectoryEntry>& entries,std::wstring_view input,const DisplayOptions& display) {
    std::wstring path,key;try{path=normalize(input);key=directory_key(path);}catch(const PolicyError&){return {};}
    const DirectoryEntry* best=nullptr;size_t length=0;
    for(auto& e:entries)if(e.enabled) {
        auto root=directory_key(e.path);bool exact=key==root;
        bool child=e.descendants&&key.size()>root.size()&&key.starts_with(root)&&(root.back()==L'\\'||key[root.size()]==L'\\');
        if((exact||child)&&root.size()>length){best=&e;length=root.size();}
    }
    if(!best||best->title=="off")return {};
    if(best->title=="alias"||length==path.size())return best->name;
    auto relative=path.substr(length);if(relative.starts_with(L"\\"))relative.erase(0,1);
    std::vector<std::wstring> parts;size_t pos=0;
    while(pos<relative.size()){auto end=relative.find(L'\\',pos);if(end==relative.npos)end=relative.size();parts.push_back(relative.substr(pos,end-pos));pos=end+1;}
    size_t first=display.depth&&parts.size()>static_cast<size_t>(*display.depth)?parts.size()-*display.depth:0;
    std::wstring suffix;for(size_t i=first;i<parts.size();++i){if(!suffix.empty())suffix+=display.separator;suffix+=parts[i];}
    return best->name+L" · "+suffix;
}
std::vector<std::string> dock_order(const std::vector<std::string>& current,const std::vector<std::string>& pinned,std::string_view side,const std::vector<std::string>& unpinned) {
    if(side=="keep")return current;if(side!="left"&&side!="right")invalid("Pinned position must be keep, left or right");
    std::set<std::string> pins(pinned.begin(),pinned.end()),released(unpinned.begin(),unpinned.end()),seen;
    std::vector<std::string> p,n,u;
    for(auto& id:current){if(!seen.insert(id).second)invalid("Duplicate tab in docking order");(pins.contains(id)?p:released.contains(id)?u:n).push_back(id);}
    std::vector<std::string> out;auto add=[&](const auto& v){out.insert(out.end(),v.begin(),v.end());};
    if(side=="left"){add(p);add(u);add(n);}else{add(n);add(u);add(p);}return out;
}
}
