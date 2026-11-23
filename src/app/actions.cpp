#include "app/actions.hpp"
#include "app/service.hpp"
#include <algorithm>

namespace sinan {
namespace {
struct Action {const char* id;const char* zh;const char* en;const char* keywords;bool target;bool mutates;};
constexpr Action actions[]={
    {"preview","标题预览","Preview title","path depth lujing cengji",false,false},
    {"test","规则测试","Test rules","directory guize",false,false},
    {"doctor","系统诊断","System diagnostics","compatibility zhenduan",false,false},
    {"symbols.status","符号状态","Symbol status","compatibility fuhao",false,false},
    {"symbols.prepare","准备符号","Prepare symbols","download fuhao",false,true},
    {"tabs.list","标签列表","List tabs","search biaoqian sousuo",false,false},
    {"tabs.pin","固定标签","Pin tab","guding",true,true},
    {"tabs.protect","保护标签","Protect tab","baohu close",true,true},
    {"tabs.lock","锁定目录","Lock directory","suoding path",true,true},
    {"tabs.locate","定位标签","Locate tab","dingwei",true,false},
    {"directories.open","打开目录入口","Open directory entry","alias path rukou",false,false},
    {"explorer.open_terminal","在此打开终端","Open terminal here","terminal zhongduan",false,false},
    {"explorer.copy_directory","复制当前目录路径","Copy current directory path","clipboard path fuzhi",false,false},
    {"explorer.copy_selection","复制选中项路径","Copy selected item paths","clipboard file fuzhi",false,false},
    {"explorer.copy_names","复制选中项名称","Copy selected item names","clipboard file fuzhi",false,false},
    {"explorer.copy_path","复制路径","Copy path","clipboard path fuzhi",false,false},
    {"explorer.open_terminal_smart","在选中文件夹或当前目录打开终端","Open terminal for selected folder or current directory","terminal zhongduan",false,false},
    {"explorer.locate_clipboard","从剪贴板定位","Locate path from clipboard","file directory dingwei",false,false},
    {"explorer.new_folder","新建文件夹","New folder","create mkdir xinjian",false,true},
    {"explorer.new_file","新建文件","New file","create file xinjian",false,true},
    {"explorer.transfer_selection","发送到标签","Send to tab","copy move transfer fuzhi yidong",false,true},
    {"explorer.extract_archive","解压并定位","Extract and locate","archive zip rar jieya",false,true},
    {"explorer.search_everything","在当前目录用 Everything 搜索","Search this directory in Everything","search sousuo",false,false},
    {"explorer.reopen_closed_tab","重开刚关闭的标签","Reopen closed tab","restore history chongkai",false,true},
    {"explorer.merge_duplicate_tabs","合并重复标签","Merge duplicate tabs","deduplicate hebing",false,true},
    {"explorer.open_parent_tab","在新标签打开父目录","Open parent in new tab","up parent fuji",false,true},
    {"explorer.toggle_file_extensions","切换文件扩展名显示","Toggle file extensions","extensions kuozhanming",false,true},
    {"appearance.status","布局状态","Layout status","width kuandu",false,false},
    {"agent.status","代理状态","Agent status","daili",false,false},
    {"start","启用定制","Start customization","qiyong",false,true},
    {"stop","停止定制","Stop customization","tingzhi",false,true}
};
std::string folded(std::string text){for(auto& c:text)if(c>='A'&&c<='Z')c+=32;return text;}
}
Json action_catalog(const Json& scripts,std::string query){
    auto rows=Json::array();query=folded(std::move(query));
    for(const auto& a:actions){
        auto searchable=folded(std::string(a.id)+" "+a.zh+" "+a.en+" "+a.keywords);
        if(!query.empty()&&searchable.find(query)==std::string::npos)continue;
        rows.push_back({{"id",a.id},{"name_zh",a.zh},{"name_en",a.en},{"keywords",a.keywords},{"requires_tab",a.target},{"mutates",a.mutates}});
        const std::string_view id=a.id;
        if(id=="directories.open"||id=="explorer.locate_clipboard")rows.back()["shortcut_scopes"]={"global","explorer"};
        else if(id.starts_with("explorer.")&&id!="explorer.search_everything")rows.back()["shortcut_scopes"]={"explorer"};
    }
    for(const auto& script:scripts) {
        const auto id="script."+script.at("id").get<std::string>();
        const auto name=script.at("name").get<std::string>();
        if(folded(id+" "+name+" powershell 脚本").find(query)==std::string::npos)continue;
        rows.push_back({{"id",id},{"name_zh",name},{"name_en",name},{"keywords","powershell 脚本"},
            {"requires_tab",false},{"requires_explorer_context",script.at("scope")=="explorer"},{"mutates",true},
            {"shortcut",script.at("shortcut")},{"shortcut_scopes",Json::array({script.at("scope")})}});
    }
    auto count=rows.size();return {{"actions",std::move(rows)},{"count",count}};
}
Json execute_action(const fs::path& root,const Json& request){
    auto id=request.at("id").get<std::string>();
    auto found=std::find_if(std::begin(actions),std::end(actions),[&](auto& a){return id==a.id;});
    auto parameters=request.value("parameters",Json::object());
    if(!parameters.is_object())throw Error(2,"Action parameters must be an object");
    if(id.starts_with("script.")){
        parameters["action"]="scripts.run";
        parameters["id"]=id.substr(7);
        return application_action(root,parameters);
    }
    if(found==std::end(actions))throw Error(2,"Unknown action ID");
    if(found->target&&(!parameters.contains("tab_id")||!parameters.at("tab_id").is_string()))throw Error(2,"This action requires a target tab_id");
    if((id=="preview"||id=="test")&&!parameters.contains("path"))throw Error(2,"This action requires a path");
    if(id=="tabs.pin"||id=="tabs.protect"||id=="tabs.lock"){
        if(!parameters.contains("enabled")||!parameters.at("enabled").is_boolean())throw Error(2,"This action requires enabled: true or false");
    }
    parameters["action"]=id;return application_action(root,parameters);
}
}
