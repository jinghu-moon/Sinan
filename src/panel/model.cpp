#include "panel/model.hpp"
#include <algorithm>
#include <cwctype>
#include <sstream>

namespace sinan::panel {
namespace {
std::wstring lower(std::wstring value){for(auto& c:value)c=static_cast<wchar_t>(std::towlower(c));return value;}
struct Description {const char* id;const wchar_t* zh;const wchar_t* en;const char* icon;int priority;};
constexpr Description descriptions[]={
    {"tabs.pin",L"让重要标签留在手边，并保护它免于意外关闭。",L"Keep an important tab close and protect it from accidental closing.","pin",0},
    {"tabs.lock",L"将当前标签锁定在原目录，后续导航在新标签中打开。",L"Keep a tab at its directory and send navigation to a new tab.","lock",4},
    {"tabs.protect",L"为标签开启独立关闭保护，不改变目录导航方式。",L"Protect a tab from closing while allowing normal navigation.","shield",5},
    {"tabs.list",L"按完整路径查找已打开的标签，不受标题层级影响。",L"Find open tabs by their full paths, regardless of title depth.","tabs",6},
    {"preview",L"先看看不同路径层级会怎样显示，再决定是否应用。",L"See how a path title looks at different depths before applying it.","text",7},
    {"test",L"检查路径命中的目录规则、优先级和标题预览。",L"Inspect the matching directory rule, priority and title preview.","search",8},
    {"doctor",L"检查当前系统、Explorer 和逐项能力的兼容状态。",L"Inspect system, Explorer and per-feature compatibility.","shield",9},
    {"symbols.status",L"读取本地符号缓存和能力状态，不联网。",L"Inspect local symbols and capability status without network access.","settings",10},
    {"symbols.prepare",L"在独立工作进程中准备微软符号和适配契约。",L"Prepare Microsoft symbols in an isolated worker process.","settings",11},
    {"tabs.locate",L"定位一个明确的目标标签，找回当前工作位置。",L"Locate a specific tab and return to your working context.","search",12},
    {"appearance.status",L"查看实测标签宽度和原生布局适配状态。",L"Inspect observed tab widths and native layout status.","settings",13},
    {"agent.status",L"查看独立代理是否运行及其当前工作模式。",L"Inspect the independent agent and its current mode.","settings",14},
    {"start",L"启用已配置的标题、标签保护和外观定制。",L"Enable configured titles, tab protection and appearance.","play",15},
    {"stop",L"停用定制并请求恢复 Explorer 的原始显示。",L"Stop customization and request original Explorer visuals.","play",16}
};
}
std::vector<Item> make_items(const Json& actions,const Json& entries,bool demo){
    std::vector<Item> result;
    for(auto& action:actions){
        Item item;item.id=action.at("id");item.zh=wide(action.at("name_zh").get<std::string>());item.en=wide(action.at("name_en").get<std::string>());
        item.keywords=wide(action.at("keywords").get<std::string>());item.requires_tab=action.at("requires_tab");
        for(auto& d:descriptions)if(item.id==d.id){item.description_zh=d.zh;item.description_en=d.en;item.icon=d.icon;item.priority=d.priority;break;}
        result.push_back(std::move(item));
    }
    for(auto& entry:entries)if(entry.value("enabled",true)){
        Item item;item.id="directory:"+entry.at("id").get<std::string>();item.category=Category::directories;
        item.zh=item.en=wide(entry.at("name").get<std::string>());item.path=wide(entry.at("path").get<std::string>());
        item.description_zh=L"通过目录入口返回工作位置，优先复用已打开的标签。";item.description_en=L"Return to a directory entry, preferring an existing tab.";
        item.keywords=L"folder directory mulu lujing";item.icon="folder";item.priority=3;result.push_back(std::move(item));
    }
    if(demo){
        auto add=[&](const char* id,Category category,const wchar_t* zh,const wchar_t* en,const wchar_t* path,const char* icon,int priority){
            Item item;item.id=id;item.category=category;item.zh=zh;item.en=en;item.path=path;item.icon=icon;item.demo=true;item.priority=priority;
            item.keywords=category==Category::tabs?L"tab biaoqian":L"folder directory mulu";
            item.description_zh=category==Category::tabs?L"按完整路径查找标签，即使标签标题只显示一级也能找到。":L"为常用工作位置设置别名，减少在目录之间反复跳转。";
            item.description_en=category==Category::tabs?L"Find a tab by its full path, even when its title shows one level.":L"Give a working directory a memorable name and reach it quickly.";result.push_back(std::move(item));
        };
        add("demo:work",Category::directories,L"工作项目",L"Projects",L"D:\\Projects\\Sinan","folder",1);
        add("demo:tab",Category::tabs,L"Sinan · 源代码",L"Sinan · Source",L"D:\\Projects\\Sinan\\src","tabs",2);
        add("demo:downloads",Category::directories,L"下载收件箱",L"Downloads",L"D:\\Inbox\\Downloads","folder",17);
        add("demo:design",Category::directories,L"设计资源",L"Design assets",L"D:\\Design\\Resources","folder",18);
        add("demo:docs",Category::tabs,L"设计文档",L"Design documents",L"D:\\Projects\\Sinan\\docs","tabs",19);
        add("demo:assets",Category::tabs,L"图标素材",L"Icon assets",L"D:\\Design\\Resources\\Icons","tabs",20);
    }
    return result;
}
std::vector<size_t> search(const std::vector<Item>& items,std::wstring query,Category category){
    std::wistringstream words(lower(std::move(query)));std::vector<std::wstring> tokens;std::wstring token;while(words>>token)tokens.push_back(token);
    std::vector<size_t> result;
    for(size_t i=0;i<items.size();++i){const auto& item=items[i];if(category!=Category::all&&item.category!=category)continue;
        auto haystack=lower(item.zh+L" "+item.en+L" "+item.path+L" "+item.keywords+L" "+wide(item.id));
        if(std::all_of(tokens.begin(),tokens.end(),[&](auto& t){return haystack.find(t)!=haystack.npos;}))result.push_back(i);
    }
    std::stable_sort(result.begin(),result.end(),[&](size_t a,size_t b){return items[a].priority<items[b].priority;});return result;
}
size_t next_selection(size_t selected,int delta,size_t count){if(!count)return 0;auto n=static_cast<int64_t>(count);return static_cast<size_t>(((static_cast<int64_t>(selected)+delta)%n+n)%n);}
std::wstring preview_item(const Item& item,bool chinese){
    auto name=chinese?item.zh:item.en;
    return chinese?L"已预览「"+name+L"」\n交互原型未更改 Explorer，也未启动代理。":L"Previewed “"+name+L"”\nThis prototype did not change Explorer or start the agent.";
}
}
