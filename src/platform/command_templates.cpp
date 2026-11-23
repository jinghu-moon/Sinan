#include "platform/command_templates.hpp"
#include "platform/shell_tabs.hpp"
#include "policy/entries.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>
#include <algorithm>

namespace sinan {
using Microsoft::WRL::ComPtr;
namespace {
Json new_plus_settings(const fs::path& location) {
    std::error_code error;
    const auto size=fs::file_size(location,error);
    if(error||size>65536)return Json::object();
    try {
        auto settings=Json::parse(read_file(location));
        if(settings.contains("properties"))return settings.at("properties");
        return settings;
    }catch(const std::exception&) {return Json::object();}
}
Json preference(const Json& settings,const char* key) {
    if(!settings.contains(key))return nullptr;
    auto value=settings.at(key);
    if(value.is_object()&&value.contains("value"))return value.at("value");
    return value;
}
bool boolean_preference(const Json& settings,const char* key,bool fallback) {
    const auto value=preference(settings,key);
    if(value.is_boolean())return value.get<bool>();
    if(value.is_number())return value!=0;
    if(value=="true"||value=="1")return true;
    if(value=="false"||value=="0")return false;
    return fallback;
}
void check(HRESULT hr,std::string_view detail) {
    if(FAILED(hr))throw Error(5,system_error(detail,static_cast<DWORD>(hr)));
}
class CreatedItem final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IFileOperationProgressSink> {
public:
    HRESULT result=E_PENDING;
    ComPtr<IShellItem> item;
    ComPtr<IShellItem> source;
    HRESULT STDMETHODCALLTYPE StartOperations() override {return S_OK;}
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD,IShellItem*,PCWSTR) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD,IShellItem*,PCWSTR,HRESULT,IShellItem*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD,IShellItem*,IShellItem*,PCWSTR) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD,IShellItem*,IShellItem*,PCWSTR,HRESULT,IShellItem*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD,IShellItem*,IShellItem*,PCWSTR) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD,IShellItem* original,IShellItem*,PCWSTR,HRESULT hr,IShellItem* created) override {
        int comparison=0;
        if(original&&source&&SUCCEEDED(original->Compare(source.Get(),SICHINT_CANONICAL,&comparison))&&comparison==0) {
            result=hr;
            item=created;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD,IShellItem*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD,IShellItem*,HRESULT,IShellItem*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD,IShellItem*,PCWSTR) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD,IShellItem*,PCWSTR,PCWSTR,DWORD,HRESULT hr,IShellItem* created) override {
        result=hr;item=created;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT,UINT) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE ResetTimer() override {return S_OK;}
    HRESULT STDMETHODCALLTYPE PauseTimer() override {return S_OK;}
    HRESULT STDMETHODCALLTYPE ResumeTimer() override {return S_OK;}
};
}

CommandTemplates command_templates(const fs::path& root,const Json& options) {
    CommandTemplates result{root/L"templates"};
    if(options.value("new_plus",false)) {
        const auto location=fs::path(env(L"LOCALAPPDATA"))/L"Microsoft"/L"PowerToys"/L"NewPlus";
        const auto preferences=new_plus_settings(location/L"settings.json");
        auto directory=preference(preferences,"TemplateLocation");
        auto path=directory.is_string()?fs::path(wide(directory.get<std::string>())):location/L"Templates";
        if(path.is_absolute())result.directory=std::move(path);
        result.hide_extension=boolean_preference(preferences,"HideFileExtension",true);
        result.hide_digits=boolean_preference(preferences,"HideStartingDigits",true);
        result.variables=boolean_preference(preferences,"ReplaceVariables",false);
    }
    const auto directory=options.value("directory",std::string{});
    if(!directory.empty())result.directory=fs::path(wide(directory));
    result.hide_extension=options.value("hide_extension",result.hide_extension);
    result.hide_digits=options.value("hide_digits",result.hide_digits);
    result.variables=options.value("variables",result.variables);
    return result;
}

std::vector<CommandTemplate> list_command_templates(const CommandTemplates& options) {
    std::error_code error;
    std::vector<CommandTemplate> entries;
    for(fs::directory_iterator item(options.directory,error),end;item!=end&&!error;item.increment(error)) {
        const auto path=item->path();
        const auto filename=path.filename().wstring();
        const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||attributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)||
            _wcsicmp(filename.c_str(),L"desktop.ini")==0)continue;
        const bool folder=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        auto label=!folder&&options.hide_extension?path.stem().wstring():filename;
        if(options.hide_digits&&!label.empty()&&iswdigit(label.front())) {
            auto first=std::find_if_not(label.begin(),label.end(),[](wchar_t ch){return iswdigit(ch)!=0;});
            while(first!=label.end()&&std::wstring_view(L" .-_").find(*first)!=std::wstring_view::npos)++first;
            if(first!=label.end())label.erase(label.begin(),first);
        }
        entries.push_back({path,std::move(label),folder});
    }
    std::sort(entries.begin(),entries.end(),[](const auto& left,const auto& right) {
        if(left.folder!=right.folder)return left.folder;
        return StrCmpLogicalW(left.path.filename().c_str(),right.path.filename().c_str())<0;
    });
    if(entries.size()>100)entries.resize(100);
    return entries;
}

std::wstring template_name(std::wstring name,const fs::path& directory,bool variables) {
    if(!variables)return name;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const auto two=[](unsigned value){return (value<10?L"0":L"")+std::to_wstring(value);};
    const auto parent=directory.filename().empty()?directory.parent_path().filename().wstring():directory.filename().wstring();
    const std::pair<std::wstring_view,std::wstring> values[]{
        {L"$PARENT_FOLDER_NAME",parent},{L"$YYYY",std::to_wstring(time.wYear)},{L"$YY",two(time.wYear%100)},
        {L"$MM",two(time.wMonth)},{L"$DD",two(time.wDay)},{L"$hh",two(time.wHour)},
        {L"$mm",two(time.wMinute)},{L"$ss",two(time.wSecond)}};
    std::wstring expanded;
    for(size_t index=0;index<name.size();) {
        const auto remaining=std::wstring_view(name).substr(index);
        const auto value=std::find_if(std::begin(values),std::end(values),[&](const auto& entry){
            return remaining.starts_with(entry.first);
        });
        if(value==std::end(values))expanded+=name[index++];
        else {expanded+=value->second;index+=value->first.size();}
    }
    return expanded;
}

fs::path shell_create_item(HWND owner,const fs::path& directory,const std::wstring& name,bool folder,
                          const fs::path& source,bool collision_suffix) {
    directory_key(directory.wstring());
    validate_folder_name(name);
    ComPtr<IFileOperation> operation;
    check(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation)),"Open Windows file operation");
    check(operation->SetOwnerWindow(owner),"Set file operation owner");
    const DWORD flags=FOF_ALLOWUNDO|FOFX_ADDUNDORECORD|FOFX_EARLYFAILURE|FOF_NOERRORUI;
    check(operation->SetOperationFlags(flags|(collision_suffix?FOF_RENAMEONCOLLISION:0)),"Configure item creation");
    ComPtr<IShellItem> destination;
    check(SHCreateItemFromParsingName(directory.c_str(),nullptr,IID_PPV_ARGS(&destination)),"Open destination");
    auto destination_name=name;
    if(GetFileAttributesW((directory/name).c_str())!=INVALID_FILE_ATTRIBUTES) {
        if(!collision_suffix)throw Error(ERROR_ALREADY_EXISTS,"An item with that name already exists");
        const auto stem=folder?name:fs::path(name).stem().wstring();
        const auto extension=folder?std::wstring{}:fs::path(name).extension().wstring();
        for(unsigned index=2;;++index) {
            destination_name=stem+L" ("+std::to_wstring(index)+L")"+extension;
            if(GetFileAttributesW((directory/destination_name).c_str())==INVALID_FILE_ATTRIBUTES)break;
        }
    }
    auto sink=Microsoft::WRL::Make<CreatedItem>();
    if(!sink)throw Error(1,"Could not allocate file operation callback");
    if(source.empty()) {
        check(operation->NewItem(destination.Get(),folder?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL,
            destination_name.c_str(),nullptr,sink.Get()),"Queue item creation");
    } else {
        ComPtr<IShellItem> template_item;
        check(SHCreateItemFromParsingName(source.c_str(),nullptr,IID_PPV_ARGS(&template_item)),"Open template");
        sink->source=template_item;
        check(operation->CopyItem(template_item.Get(),destination.Get(),destination_name.c_str(),sink.Get()),"Queue template copy");
    }
    check(operation->PerformOperations(),"Create item");
    check(sink->result,"Complete item creation");
    if(!sink->item)throw Error(5,"Windows did not confirm the created item");
    PWSTR raw=nullptr;
    check(sink->item->GetDisplayName(SIGDN_FILESYSPATH,&raw),"Get created item path");
    const std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(raw,CoTaskMemFree);
    return path.get();
}

bool select_created_item(IShellView& view,const fs::path& path,bool rename) {
    TabLocation location;
    if(FAILED(shell_view_location(view,location))||!location.filesystem_path||
        directory_key(*location.filesystem_path)!=directory_key(path.parent_path().wstring()))return false;
    PIDLIST_ABSOLUTE raw=nullptr;
    if(FAILED(SHParseDisplayName(path.c_str(),nullptr,&raw,0,nullptr))||!raw)return false;
    const std::unique_ptr<ITEMIDLIST,decltype(&CoTaskMemFree)> item(raw,CoTaskMemFree);
    const auto flags=SVSI_SELECT|SVSI_DESELECTOTHERS|SVSI_ENSUREVISIBLE|SVSI_FOCUSED|(rename?SVSI_EDIT:0);
    return view.SelectItem(ILFindLastID(item.get()),flags)==S_OK;
}
}
