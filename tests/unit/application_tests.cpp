#include "app/service.hpp"
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
 auto listed=call({{"action","actions.list"},{"query","固定"}}).at("result");
 expect(listed.at("count").get<int>()>0,"Chinese action discovery");
 expect(!fs::exists(root),"Action discovery does not create state");
 auto all=call({{"action","actions.list"}}).at("result").at("actions");std::set<std::string> ids;
 for(auto& a:all){expect(ids.insert(a.at("id").get<std::string>()).second,"Unique catalog ID");expect(!a.at("name_zh").get<std::string>().empty()&&!a.at("name_en").get<std::string>().empty(),"Bilingual catalog labels");}
 expect(call({{"action","actions.list"},{"query","GUDING"}}).at("result").at("count")==1,"ASCII keyword search is case insensitive");
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
 call({{"action","shortcuts.set"},{"id","toggle_pin"},{"chord","Ctrl+Alt+P"}});
 expect(call({{"action","features.get"},{"group","shortcuts"}}).at("result").at("toggle_pin")=="Ctrl+Alt+P","Shared shortcut setting");
 rejects([&]{call({{"action","shortcuts.set"},{"id","missing"},{"chord","Ctrl+Alt+P"}});});
 call({{"action","appearance.patch"},{"mode","fixed"},{"width",360}});
 call({{"action","appearance.patch"},{"mode","content"},{"min_width",140},{"max_width",420}});
 auto width=call({{"action","appearance.get"}}).at("result");expect(width.at("width")==360&&width.at("min_width")==140,"Partial width edit preserves unrelated fields");
 rejects([&]{call({{"action","tabs.set"},{"tab_id","bad"},{"depth","1"}});});
 call({{"action","pinned.position"},{"position","right"}});
 expect(call({{"action","features.get"},{"group","tab_behavior"}}).at("result").at("pinned_position")=="right","Shared pin position");

 auto style_path=root/L"oversized-style.json";atomic_write(style_path,std::string(200001,' '));auto saved=read_file(root/L"sinan.toml");
 rejects([&]{call({{"action","style.load"},{"file",utf8(style_path.wstring())}});});
 expect(saved==read_file(root/L"sinan.toml"),"Oversized style rejected without mutation");
 auto via_action=call({{"action","actions.execute"},{"id","preview"},{"parameters",{{"path","D:\\A\\B"},{"depth","1"}}}});
 expect(via_action.at("result").at("title")=="B","Catalog executes shared application operation");
 std::cout<<checks<<" application checks passed\n";
 std::error_code ec;fs::remove_all(root,ec);return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';std::error_code ec;fs::remove_all(root,ec);return 1;}}
