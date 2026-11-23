#include "app/service.hpp"
#include "platform/shell_actions.hpp"
#include <iostream>
#include <functional>
#include <set>
using namespace sinan;
namespace {
int checks=0;
void expect(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
void rejects(const std::function<void()>& action,int expected=2){bool ok=false;try{action();}catch(const Error& e){ok=e.code==expected;}expect(ok,"Expected application error");}
}
int main(){auto root=fs::temp_directory_path()/(L"sinan-app-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
try{
 auto call=[&](Json r){return application_action(root,r);};
 expect(everything_search_argument(L"*.cpp | *.h")==L"\"<*.cpp | *.h>\"","OR stays inside the directory scope");
 expect(everything_search_argument(L"\"带 空格\"")==L"\"<\"\"\"带 空格\"\"\">\"","Everything preserves quoted Unicode queries");
 expect(everything_search_argument(L"size:>1mb")==L"\"<size:>1mb>\"","Size comparison is preserved");
 rejects([]{everything_search_argument(L"> | *.txt");});
 rejects([]{everything_search_argument(L" \\0\n");});
 const Json everything_query{{"action","everything-search.query"},{"directory","D:\\A"},{"query","*.txt"}};
 auto invalid_query=everything_query;
 invalid_query["limit"]=1001;
 rejects([&]{call(invalid_query);});
 invalid_query=everything_query;
 invalid_query["directory"]="relative";
 rejects([&]{call(invalid_query);});
 invalid_query=everything_query;
 invalid_query["query"]="\"missing closing quote";
 rejects([&]{call(invalid_query);});
 auto legacy=config_json(Config{});
 legacy.at("navigation").erase("everything_search");
 legacy.at("shortcuts").erase("new_file");
 expect(!config_from_json(legacy).everything_search,"existing config leaves Everything gesture disabled");
 expect(config_from_json(legacy).features.at("shortcuts").at("new_file")=="","existing config leaves the new file shortcut unbound");
 const Json selection_rule={{"count","multiple"},{"kind","files"},{"extensions",Json::array({".PNG",".jpg"})},{"hide",true}};
 const auto command_bar=command_bar_from_json({{"conditions",{{"explorer.copy_names",selection_rule}}}});
 expect(command_bar.conditions.at("explorer.copy_names").at("extensions")==Json::array({".png",".jpg"}),
     "Command bar matches extensions case insensitively");
 expect(command_bar_from_json(command_bar_json(command_bar))==command_bar,"Command bar selection rule round trip");
 rejects([]{command_bar_from_json({{"conditions",{{"explorer.copy_names",{{"count","unknown"}}}}}});});
 auto listed=call({{"action","actions.list"},{"query","固定"}}).at("result");
 expect(listed.at("count").get<int>()>0,"Chinese action discovery");
 expect(!fs::exists(root),"Action discovery does not create state");
 auto all=call({{"action","actions.list"}}).at("result").at("actions");std::set<std::string> ids;
 for(auto& a:all){expect(ids.insert(a.at("id").get<std::string>()).second,"Unique catalog ID");expect(!a.at("name_zh").get<std::string>().empty()&&!a.at("name_en").get<std::string>().empty(),"Bilingual catalog labels");}
 expect(call({{"action","actions.list"},{"query","GUDING"}}).at("result").at("count")==1,"ASCII keyword search is case insensitive");
 const Json first{{"tab_id","runtime.1"},{"path","D:\\A\\src"},{"original_title","src"},{"preview_title","Ärger 源码"},{"applied_title","开发"}};
 const Json second{{"tab_id","runtime.2"},{"path","D:\\B\\src"},{"original_title","src"}};
 const Json virtual_tab{{"tab_id","runtime.3"},{"path",nullptr},{"original_title","此电脑"}};
 expect(TabSearch("SRC").matches(first)&&TabSearch("SRC").matches(second),"same-name results are both retained");
 expect(TabSearch("d:/a/SRC").matches(first)&&!TabSearch("d:/a/SRC").matches(second),"full path distinguishes same-name tabs");
 expect(TabSearch("d:/a ärger\u3000开发").matches(first),"terms can span path, preview and applied title");
 expect(TabSearch("此电脑 RUNTIME.3").matches(virtual_tab),"virtual titles and tab IDs are searchable");
 expect(TabSearch(" \t").matches(virtual_tab)&&!TabSearch("missing").matches(first),"empty query and no match");
 rejects([&]{call({{"action","unregistered"}});});
 rejects([&]{call({{"action","preview"}});});
 expect(!fs::exists(root),"Malformed and unknown requests do not create state");
 auto preview=call({{"action","preview"},{"path","D:\\项目\\源码"},{"depth","1"}}).at("result");
 expect(preview.at("title")=="源码","Shared preview applies depth");
 expect(!fs::exists(root),"Preview does not create state");
 rejects([&]{call({{"action","actions.execute"},{"id","missing"}});});
 rejects([&]{call({{"action","actions.execute"},{"id","tabs.pin"}});});
 rejects([&]{call({{"action","preview"},{"path","D:\\A"},{"depth","0"}});});
 call({{"action","config.set"},{"key","depth"},{"value","3"}});
 expect(call({{"action","config.get"}}).at("result").at("display").at("depth")==3,"Shared config mutation");
 expect(fs::exists(root/L"sinan.toml")&&!fs::exists(root/L"tabtail.toml"),"Renamed config contract");
 auto before=read_file(root/L"sinan.toml");rejects([&]{call({{"action","config.set"},{"key","depth"},{"value","0"}});});
 expect(read_file(root/L"sinan.toml")==before,"Invalid shared mutation is atomic");
 call({{"action","entries.put"},{"id","work"},{"path","D:\\Work"},{"name","工作"}});
 rejects([&]{call({{"action","entries.put"},{"id","work"},{"path","D:\\Other"},{"name","另一"}});});
 expect(call({{"action","features.get"},{"group","directories"}}).at("result").at("entries").size()==1,"Entry duplicate does not write");
 ConfigStore(root).update([](Config& config){
     config.features["directories"]["entries"].push_back({{"id","legacy"},{"path","D:\\Legacy"},{"name","Legacy"}});
 });
 const auto entry_settings=call({{"action","entries.settings.get"}}).at("result");
 const auto legacy_entry=entry_settings.at("entries").back();
 expect(legacy_entry.at("enabled")==true&&legacy_entry.at("title")=="alias_depth",
     "Directory settings fill optional fields for existing entries");
 const Json disable_entry{{"action","entries.put"},{"id","legacy"},{"path","D:\\Legacy"},{"name","Legacy"},
     {"enabled",false},{"replace",true},{"expected_entry",legacy_entry}};
 expect(call(disable_entry).at("result").at("entries").back().at("enabled")==false,
     "Directory settings can update a legacy entry and return the saved state");
 expect(call(disable_entry).at("result").at("conflict")=="directories","Stale entry cannot overwrite a newer edit");
 call({{"action","shortcuts.set"},{"id","toggle_pin"},{"chord","Ctrl+Alt+P"}});
 expect(call({{"action","features.get"},{"group","shortcuts"}}).at("result").at("toggle_pin")=="Ctrl+Alt+P","Shared shortcut setting");
 rejects([&]{call({{"action","shortcuts.set"},{"id","missing"},{"chord","Ctrl+Alt+P"}});});
 call({{"action","appearance.patch"},{"mode","fixed"},{"width",360}});
 call({{"action","appearance.patch"},{"mode","content"},{"min_width",140},{"max_width",420}});
 auto width=call({{"action","appearance.get"}}).at("result");expect(width.at("width")==360&&width.at("min_width")==140,"Partial width edit preserves unrelated fields");
 const auto appearance=call({{"action","appearance.settings.get"}}).at("result");
 auto desired=appearance;
 desired["depth"]="all";
 desired["separator"]=" / ";
 desired["tabs"]["max_width"]=450;
 Json save_appearance{{"action","appearance.settings.save"},{"expected",appearance},{"settings",desired}};
 call({{"action","language"},{"language","zh-CN"}});
 expect(call(save_appearance).at("result").at("saved")==true,"Appearance save accepts unrelated config edits");
 expect(call({{"action","appearance.settings.get"}}).at("result")==desired&&ConfigStore(root).load().language=="zh-CN",
     "Combined appearance save persists both sections and preserves other settings");
 save_appearance["settings"]["depth"]="4";
 expect(call(save_appearance).at("result").at("conflict")=="appearance","Stale appearance draft cannot overwrite current settings");
 save_appearance["expected"]=desired;
 save_appearance["settings"]["tabs"]["min_width"]=600;
 rejects([&]{call(save_appearance);});
 expect(call({{"action","appearance.settings.get"}}).at("result")==desired,"Invalid width leaves the title settings intact");
 rejects([&]{call({{"action","tabs.set"},{"tab_id","bad"},{"depth","1"}});});
 call({{"action","pinned.position"},{"position","right"}});
 expect(call({{"action","features.get"},{"group","tab_behavior"}}).at("result").at("pinned_position")=="right","Shared pin position");
 expect(call({{"action","pinned.position"},{"position","left"},{"expected_position","keep"}})
     .at("result").at("conflict")=="tab_behavior","Stale pinned position is rejected");
 expect(call({{"action","pinned.position"},{"position","left"},{"expected_position","right"}})
     .at("result").at("pinned_position")=="left","Pinned position response reflects the saved position");

 auto style_path=root/L"oversized-style.json";atomic_write(style_path,std::string(200001,' '));auto saved=read_file(root/L"sinan.toml");
 rejects([&]{call({{"action","style.load"},{"file",utf8(style_path.wstring())}});});
 expect(saved==read_file(root/L"sinan.toml"),"Oversized style rejected without mutation");
 auto via_action=call({{"action","actions.execute"},{"id","preview"},{"parameters",{{"path","D:\\A\\B"},{"depth","1"}}}});
 expect(via_action.at("result").at("title")=="B","Catalog executes shared application operation");
 for(auto id:{"directories.open","explorer.open_terminal","explorer.open_terminal_smart","explorer.copy_directory",
     "explorer.copy_selection","explorer.copy_names","explorer.copy_path","explorer.locate_clipboard","explorer.new_folder",
     "explorer.new_file","explorer.reopen_closed_tab","explorer.merge_duplicate_tabs","explorer.open_parent_tab",
     "explorer.toggle_file_extensions","explorer.search_everything"}) {
     const auto matches=call({{"action","actions.list"},{"query",id}}).at("result").at("actions");
     expect(std::any_of(matches.begin(),matches.end(),[&](const auto& row){return row.at("id")==id;}),
         "Explorer action is discoverable by ID");
 }
 auto create=[&](const char* name){return call({{"action","actions.execute"},{"id","explorer.new_folder"},
     {"parameters",{{"parent",utf8(root.wstring())},{"name",name}}}});};
 auto folder=create("新建 项目");
 expect(folder.at("result").at("created")==true&&fs::is_directory(root/L"新建 项目"),"Named action creates a real Unicode folder without the agent");
 rejects([&]{create("新建 项目");});
 rejects([&]{create("..");});
 rejects([&]{create("child/escape");});
 rejects([&]{create("COM1.txt");});
 auto shortcuts=call({{"action","shortcuts.set"},{"id","new_folder"},{"chord","Ctrl+Alt+Shift+F20"},{"name","assets"}});
 expect(shortcuts.at("code")==0&&ConfigStore(root).load().features["shortcuts"]["new_folder_name"]=="assets","Preset survives TOML reload");
 rejects([&]{call({{"action","shortcuts.set"},{"id","new_file"},{"chord","Ctrl+Shift+Alt+F20"}});});
 call({{"action","shortcuts.set"},{"id","new_file"},{"chord","Ctrl+Alt+Shift+F21"}});
 auto bindings=ConfigStore(root).load().features.at("shortcuts");
 expect(bindings.at("new_file")=="Ctrl+Alt+Shift+F21"&&bindings.at("new_folder_name")=="assets","New file shortcut persists without changing the folder preset");
 call({{"action","shortcuts.set"},{"id","new_file"},{"chord",""}});
 expect(ConfigStore(root).load().features.at("shortcuts").at("new_file")=="","New file shortcut can be cleared");
 const auto shortcut_settings=call({{"action","shortcuts.settings.get"}}).at("result");
 expect(shortcut_settings.at("shortcut_settings").at("new_folder_name")=="assets"&&
     shortcut_settings.at("shortcut_listener").at("running")==false,"Shortcut settings report saved values with no agent");
 Json save_shortcut{{"action","shortcuts.set"},{"id","new_folder"},{"chord","Ctrl+Alt+Shift+F22"},
     {"expected_chord","Ctrl+Alt+Shift+F20"},{"name","images"},{"expected_name","old preset"}};
 expect(call(save_shortcut).at("result").at("conflict")=="shortcuts","A stale folder preset cannot overwrite the current setting");
 save_shortcut["expected_name"]="assets";
 const auto saved_shortcut=call(save_shortcut).at("result").at("shortcut_settings");
 expect(saved_shortcut.at("new_folder")=="Ctrl+Alt+Shift+F22"&&saved_shortcut.at("new_folder_name")=="images",
     "Shortcut settings save returns both values persisted together");
 call({{"action","shortcuts.set"},{"id","new_folder"},{"chord","Ctrl+Alt+Shift+F20"}});
 rejects([&]{call({{"action","shortcuts.set"},{"id","locate_clipboard_global"},{"chord","Alt+Ctrl+Shift+F20"}});});
 std::cout<<checks<<" application checks passed\n";
 std::error_code ec;fs::remove_all(root,ec);return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';std::error_code ec;fs::remove_all(root,ec);return 1;}}
