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
    {"appearance.status","布局状态","Layout status","width kuandu",false,false},
    {"agent.status","代理状态","Agent status","daili",false,false},
    {"start","启用定制","Start customization","qiyong",false,true},
    {"stop","停止定制","Stop customization","tingzhi",false,true}
};
std::string folded(std::string text){for(auto& c:text)if(c>='A'&&c<='Z')c+=32;return text;}
}
Json action_catalog(std::string query){
    auto rows=Json::array();query=folded(std::move(query));
    for(const auto& a:actions){
        auto searchable=folded(std::string(a.id)+" "+a.zh+" "+a.en+" "+a.keywords);
        if(!query.empty()&&searchable.find(query)==std::string::npos)continue;
        rows.push_back({{"id",a.id},{"name_zh",a.zh},{"name_en",a.en},{"keywords",a.keywords},{"requires_tab",a.target},{"mutates",a.mutates}});
    }
    auto count=rows.size();return {{"actions",std::move(rows)},{"count",count}};
}
Json execute_action(const fs::path& root,const Json& request){
    auto id=request.at("id").get<std::string>();
    auto found=std::find_if(std::begin(actions),std::end(actions),[&](auto& a){return id==a.id;});
    if(found==std::end(actions))throw Error(2,"Unknown action ID");
    auto parameters=request.value("parameters",Json::object());
    if(!parameters.is_object())throw Error(2,"Action parameters must be an object");
    if(found->target&&(!parameters.contains("tab_id")||!parameters.at("tab_id").is_string()))throw Error(2,"This action requires a target tab_id");
    if((id=="preview"||id=="test")&&!parameters.contains("path"))throw Error(2,"This action requires a path");
    if(id=="tabs.pin"||id=="tabs.protect"||id=="tabs.lock"){
        if(!parameters.contains("enabled")||!parameters.at("enabled").is_boolean())throw Error(2,"This action requires enabled: true or false");
    }
    parameters["action"]=id;return application_action(root,parameters);
}
}
