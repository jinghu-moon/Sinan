#include "config/config.hpp"
#include "policy/entries.hpp"
#include "formatter/formatter.hpp"
#include "agent/interaction.hpp"
#include "ui/shortcut_recorder.hpp"
#include "hook/controller.hpp"
#include <iostream>
using namespace sinan;
namespace {
int checks{};
void expect(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(...){rejected=true;}expect(rejected,"invalid entry accepted");}
}
int main(){try{
    DirectoryEntry entry{"work",L"工作",L"D:\\Projects\\Sinan","Ctrl+Alt+1",true,true,"alias_depth"};
    DisplayOptions display{2,L"\\",{entry}};
    expect(format_title(L"d:/Projects/Sinan/./",display).text==L"工作","exact alias and slash/case normalization");
    expect(format_title(L"D:\\Projects\\Sinan\\src\\agent\\config",display).text==L"工作 · agent\\config","relative suffix respects depth");
    display.depth=3;expect(format_title(L"D:\\Projects\\Sinan\\src\\agent\\config",display).text==L"工作 · src\\agent\\config","preview reacts to depth");
    display.depth=std::nullopt;expect(format_title(L"D:\\Projects\\Sinan\\a\\b\\c\\d",display).text==L"工作 · a\\b\\c\\d","all relative components");
    expect(!alias_title(display.entries,L"D:\\Projects\\Sinan-old\\src",display),"directory boundary");
    display.entries[0].descendants=false;expect(!alias_title(display.entries,L"D:\\Projects\\Sinan\\src",display),"exact default excludes descendants");
    display.entries[0].descendants=true;auto child=entry;child.id="child";child.path+=L"\\src";child.name=L"源代码";child.title="alias";child.shortcut="";display.entries.push_back(child);
    expect(format_title(L"D:\\Projects\\Sinan\\src\\agent",display).text==L"源代码","most specific entry");
    display.entries[1].title="off";expect(!alias_title(display.entries,L"D:\\Projects\\Sinan\\src\\agent",display),"specific opt out does not inherit parent alias");
    display.entries[1].enabled=false;expect(alias_title(display.entries,L"D:\\Projects\\Sinan\\src\\agent",display).has_value(),"disabled entry does not shadow");
    expect(directory_key(L"\\\\server\\share\\a\\..\\b\\")==L"\\\\SERVER\\SHARE\\B","UNC normalization");
    expect(directory_key(L"D:\\Ärger")==directory_key(L"d:\\ärger"),"Windows Unicode case mapping");
    expect(directory_key(L"\\\\?\\D:\\Projects\\Sinan")==directory_key(entry.path),"extended drive path");
    expect(directory_parent(L"d:/Projects/Sinan/.")==L"d:\\Projects","parent normalizes the path");
    expect(directory_parent(L"D:\\Projects")==L"D:\\","a first-level directory opens the drive root");
    expect(!directory_parent(L"D:\\"),"the drive root has no filesystem parent");
    expect(directory_parent(L"\\\\server\\share\\folder")==L"\\\\server\\share"&&
        !directory_parent(L"\\\\server\\share\\"),"the UNC share root bounds parent navigation");
    rejects([&]{auto e=entry;e.path=L"D:relative";validate_entries({e});});
    rejects([&]{auto e=entry;e.path=L"shell:Downloads";validate_entries({e});});
    rejects([&]{auto e=entry;e.name=L"bad\nname";validate_entries({e});});
    rejects([&]{auto e=entry;e.id="same";validate_entries({e,e});});
    rejects([&]{auto e=entry;e.id="other";e.path=L"d:/projects/sinan/";validate_entries({entry,e});});
    Config config;config.features["directories"]["entries"]=Json::array({entry_json(entry)});validate_config(config);
    auto copy=config_from_json(config_json(config));expect(copy.display_options()==config.display_options(),"config JSON roundtrip");
    RuntimePolicy policy{copy.display_options(),std::make_shared<RuleSet>(),{}};
    expect(runtime_policy_from_json(runtime_policy_json(policy)).defaults==policy.defaults,"title hook receives aliases");
    rejects([&]{auto f=config.features;f["shortcuts"]["toggle_pin"]="Alt+Ctrl+1";validate_features(f);});
    rejects([&]{auto f=config.features;f["tab_behavior"]["pinned_position"]="middle";validate_features(f);});
    auto legacy=config_json(Config{});legacy.erase("directories");legacy.erase("scripts");legacy["tab_behavior"].erase("pinned_position");legacy["schema_version"]=3;
    legacy.erase("navigation");
    legacy["shortcuts"].erase("open_parent_tab");
    expect(!config_from_json(legacy).double_click_parent&&
        config_from_json(legacy).features.at("shortcuts").at("open_parent_tab")=="","older configurations load new controls disabled");
    expect(config_from_json(legacy).features.at("directories").at("entries").empty(),"schema3 loads with safe defaults");
    std::vector<std::string> tabs{"a","p","b","q","c"},pins{"p","q"};
    expect(dock_order(tabs,pins,"left")==std::vector<std::string>({"p","q","a","b","c"}),"left stable partition");
    expect(dock_order(tabs,pins,"right")==std::vector<std::string>({"a","b","c","p","q"}),"right stable partition");
    expect(dock_order(tabs,pins,"keep")==tabs,"keep no move");
    expect(dock_order({"p","q","a","b"},{"q"},"left",{"p"})==std::vector<std::string>({"q","p","a","b"}),"unpin joins left ordinary boundary");
    expect(dock_order({"a","b","p","q"},{"p"},"right",{"q"})==std::vector<std::string>({"a","b","q","p"}),"unpin joins right ordinary boundary");
    expect(dock_order({"q","p","a","b"},pins,"left")==std::vector<std::string>({"q","p","a","b"}),"dragged group order retained");
    rejects([&]{dock_order({"a","a"},pins,"left");});
    expect(shortcut_chord(MOD_CONTROL,'Q')=="Ctrl+Q","record Ctrl+Q");
    expect(shortcut_chord(MOD_CONTROL|MOD_ALT,'1')=="Ctrl+Alt+1","record Ctrl+Alt+1");
    expect(shortcut_chord(MOD_CONTROL|MOD_SHIFT,VK_F12)=="Ctrl+Shift+F12","record modified function key");
    expect(shortcut_chord(0,'A').empty()&&shortcut_chord(MOD_CONTROL,VK_TAB).empty(),"plain text and Tab retain input navigation");
    {ShortcutRecorder recorder(GetForegroundWindow(),[]{});expect(recorder.error().empty()&&recorder.take().empty()&&!recorder.hook_active(),"recorder stays unhooked with no field armed");}
    {auto dll=executable_path().parent_path()/L"sinan-hook.dll";auto module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);expect(module!=nullptr,"load title DLL for read-only preflight");
        auto same=conflicting_title_modules(GetCurrentProcessId(),dll);auto other=conflicting_title_modules(GetCurrentProcessId(),dll.parent_path()/L"another-copy"/L"sinan-hook.dll");FreeLibrary(module);
        expect(same.empty(),"same title module path accepted");expect(other.size()==1&&other.front()==dll,"foreign title module path reported");}
    // Hold a real OS registration; no keystrokes or Explorer changes.
    auto chord=parse_shortcut("Ctrl+Alt+Shift+F23");
    bool reserved=RegisterHotKey(nullptr,0x77a0,chord.modifiers,chord.key)!=FALSE;
    if(reserved){DirectoryShortcuts shortcuts;auto e=entry;e.shortcut="Ctrl+Alt+Shift+F23";shortcuts.update(Json::array({entry_json(e)}));
        expect(shortcuts.status().at("bound")==0&&!shortcuts.status().at("errors").empty(),"OS collision reported");
        UnregisterHotKey(nullptr,0x77a0);shortcuts.update(Json::array());shortcuts.update(Json::array({entry_json(e)}));
        expect(shortcuts.status().at("bound")==1&&shortcuts.status().at("errors").empty(),"shortcut rebound after collision release");
        auto host=FindWindowExW(HWND_MESSAGE,nullptr,L"Sinan.DirectoryShortcuts.1",nullptr);
        expect(host&&PostMessageW(host,WM_HOTKEY,0x6801,MAKELPARAM(chord.modifiers&~MOD_NOREPEAT,chord.key)),"queue old directory shortcut");
        auto replacement=e;replacement.id="replacement";shortcuts.update(Json::array({entry_json(replacement)}));
        MSG message{};while(PeekMessageW(&message,host,WM_HOTKEY,WM_HOTKEY,PM_REMOVE))DispatchMessageW(&message);
        expect(shortcuts.take().empty(),"queued hotkey cannot open the replacement binding");
        replacement.enabled=false;shortcuts.update(Json::array({entry_json(replacement)}));
        expect(shortcuts.status().at("bound")==0,"disabled entry releases its global shortcut");
        shortcuts.update(Json::array());expect(RegisterHotKey(nullptr,0x77a0,chord.modifiers,chord.key)!=FALSE,"removed shortcut released");UnregisterHotKey(nullptr,0x77a0);
    }else throw std::runtime_error("Test chord unexpectedly occupied");
    std::cout<<checks<<" entry/docking checks passed\n";return 0;
}catch(const std::exception& ex){std::cerr<<"after "<<checks<<" checks: "<<ex.what()<<'\n';return 1;}}
