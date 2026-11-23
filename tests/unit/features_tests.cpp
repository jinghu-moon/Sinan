#include "config/config.hpp"
#include "app/service.hpp"
#include "agent/workspace.hpp"
#include "agent/features.hpp"
#include <atomic>
#include <iostream>

using namespace sinan;
namespace {
int checks{};
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(...){rejected=true;}expect(rejected,"Invalid feature accepted");}
}
class TestIdentity final:public IUnknown {
    std::atomic<ULONG> refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs_;if(!n)delete this;return n;}
};
int main() {
    auto root=fs::temp_directory_path()/(L"Sinan-features-"+std::to_wstring(GetCurrentProcessId()));
    try {
        TabFlags flags{true,true,true};flags.pinned=false;expect(flags.close_protected()&&flags.locked,"unpin must preserve explicit protection and lock");
        flags.protected_tab=false;expect(!flags.close_protected()&&flags.locked,"lock is independent from close protection");
        auto f=default_features();validate_features(f);
        f["tab_style"]["default"]={{"text",{{"bold",true},{"font_color","#123456"}}}};
        f["tab_style"]["pinned"]["text"]={{"font_color","#FF0000"}};
        f["tab_style"]["locked"]["text"]={{"bold",false}};
        f["tab_style"]["rules"].push_back({{"id","workspace"},{"patterns",{"D:\\Projects\\**"}},{"style",{{"text",{{"font_family","system"}}},{"light",{{"background",{{"selected","#FFFF00"}}}}}}}});
        flags={true,true,true};auto style=resolve_style(f,flags,L"D:\\Projects\\test");
        expect(style.at("text").at("bold")==false,"false overrides true");
        expect(style.at("text").at("font_color")=="#FF0000","pin overrides global text color");
        expect(style.at("icon").at("source")=="builtin:lock-rounded","lock icon wins");
        auto override=resolve_style(f,flags,L"D:\\Projects\\test",{{"text",{{"font_color","system"},{"bold","inherit"}}},{"icon",{{"source","system"}}}});
        expect(override.at("text").at("font_color")=="system","system must stop inheritance");
        expect(override.at("text").at("bold")==false,"inherit must retain effective lower field");
        expect(override.at("icon").at("source")=="system","temporary explicit system icon overrides states");
        expect(style.at("rule_id")=="workspace","independent appearance rule matched");
        expect(style_for_theme(style,false,true,true,false).at("background")=="#FFFF00","selected wins over hover");
        expect(!style_for_theme(style,false,true,false,true).contains("text"),"high contrast restores system text");
        expect(contrast_ratio("#000000","#FFFFFF")>20.99,"WCAG contrast");
        auto chord=parse_shortcut("Ctrl+Alt+P");expect(chord.key=='P'&&(chord.modifiers&MOD_NOREPEAT),"shortcut no repeat");
        expect(parse_shortcut("").key==0,"empty chord unbound");
        for(auto bad:{"P","Ctrl+W","Win+E","Ctrl+Ctrl+P","Alt+F25","Ctrl+P+L","Ctrl+P+","Ctrl+Shift+W","Alt+D"})rejects([&]{parse_shortcut(bad);});
        rejects([&]{auto bad=f;bad["shortcuts"]["toggle_pin"]="Ctrl+Alt+P";bad["shortcuts"]["toggle_protection"]="ctrl+alt+p";validate_features(bad);});
        rejects([&]{validate_style({{"text",{{"bold","false"}}}});});
        rejects([&]{validate_style({{"text",{{"font_color","#xyz123"}}}});});
        rejects([&]{validate_style({{"icon",{{"source","missing.svg"}}}});});
        rejects([&]{validate_style({{"text",{{"font_family","NoSuchSinanFont9827"}}}});});
        rejects([&]{auto bad=f;bad["tab_style"]["rules"][0]["patterns"]={"D:\\[!abc]\\**"};validate_features(bad);});
        auto color_layers=default_features();color_layers["tab_style"]["pinned"]["light"]["text"]["selected"]="#112233";
        color_layers["tab_style"]["locked"]["text"]["font_color"]="system";
        expect(style_for_theme(resolve_style(color_layers,{true,false,true},L"D:\\test"),false,true,false,false).at("text").at("font_color")=="system","higher generic system color overrides lower themed color");
        rejects([&]{auto bad=f;bad["tab_style"]["rules"][0]["patterns"]="D:\\Projects\\**";validate_features(bad);});
        rejects([&]{auto bad=f;bad["notifications"].erase("mode");validate_features(bad);});
        ConfigStore store(root);
        atomic_write(store.path(),"schema_version=2\nrevision=3\nenabled=false\nautostart=false\n[display]\ndepth=3\nseparator='\\'\n");
        expect(store.load().features==default_features(),"legacy defaults");
        auto old=read_file(store.path());expect(!fs::exists(root/L"sinan.pre-schema3.toml"),"read does not migrate");
        auto config=store.update([&](Config& c){c.features=f;});
        expect(read_file(root/L"sinan.pre-schema3.toml")==old,"upgrade backup exact");
        expect(store.load().features==f&&config_json(config).at("schema_version")==4,"schema3 TOML roundtrip");
        auto no_pin_override=f;no_pin_override["tab_style"]["pinned"]={{"enabled",true}};
        store.update([&](Config& c){c.features=no_pin_override;});expect(!store.load().features.at("tab_style").at("pinned").contains("icon"),"removed state icon override stays inherited after TOML reload");
        store.update([&](Config& c){c.features=f;});
        auto baseline=config.features.at("notifications");store.update([](Config& c){c.depth=5;});
        auto result=application_action(root,{{"action","features.save"},{"group","notifications"},{"expected",baseline},{"value",{{"mode","errors"}}}});
        expect(result.at("result").at("saved")==true,"unrelated config change does not invalidate feature draft");
        result=application_action(root,{{"action","features.save"},{"group","notifications"},{"expected",baseline},{"value",{{"mode","off"}}}});
        expect(result.at("code")==2&&!result.at("result").at("saved").get<bool>(),"real feature conflict preserves current settings");
        expect(store.load().features.at("notifications").at("mode")=="errors","conflict must not overwrite saved setting");
        result=application_action(root,{{"action","features.save"},{"group","notifications"},{"expected",baseline},{"value",{{"mode","errors"}}}});
        expect(result.at("code")==0,"same desired value idempotent");
        Json tabs=Json::array();for(int i=0;i<3;++i)tabs.push_back({{"tab_id","runtime."+std::to_string(i)},{"window",i==2?22:11},{"path","D:\\Projects\\test"},{"behavior",{{"pinned",true},{"locked",i==1}}}});
        auto saved=save_workspace(root,tabs,20);expect(saved.at("count")==2,"workspace dedupes within window and retains distinct window groups");
        auto entries=load_workspace(root,20);expect(entries[0].at("locked")==true,"snapshot merges important states for same directory");
        expect(entries.size()==2&&!entries[0].contains("tab_id")&&!entries[0].contains("window"),"workspace persists directories not runtime identity");
        expect(load_workspace(root,1).size()==1,"workspace limit");
        // Exercise production state ownership with deterministic native transports.
        // Identical paths must still have independent flags and temporary styles.
        ShellTabTracker tracker("features-test",Config{},[](DWORD,uint64_t){return ProcessLifetime::running;});
        ShellTabScan observed;for(int i=0;i<2;++i){ShellTabObservation t;t.com_identity.Attach(new TestIdentity);t.window=reinterpret_cast<HWND>(100);t.browser_window=reinterpret_cast<HWND>(uintptr_t(101+i));t.process_id=42;t.process_started=1000;t.location={L"D:\\same",L"same"};t.location_resolved=true;NativeTab n;n.window=t.window;n.browser_window=t.browser_window;n.process_id=42;n.process_started=1000;n.guid.Data1=1+i;t.native=n;observed.tabs.push_back(t);}tracker.reconcile(observed);
        auto ids=tracker.list();auto a=ids[0].at("tab_id").get<std::string>(),b=ids[1].at("tab_id").get<std::string>();
        bool fail_native=false,fail_style=false,fail_icon=false,restore_style=false,selected=true;int native_restores=0,style_restores=0,pending_style=0;std::map<unsigned,Json> actual;
        unsigned located_guid=0;
        TabFeatures controller([&](const NativeTab& t,FeatureAction action,const Json& payload,bool){
            if(action==FeatureAction::locate){located_guid=t.guid.Data1;return Json{{"active",selected},{"active_known",true}};}
            if(action==FeatureAction::restore){++native_restores;actual.erase(t.guid.Data1);return Json{{"icon_restored",true}};}
            if(fail_native)throw Error(3,"native rejected");actual[t.guid.Data1]=payload;return Json{{"protection_confirmed",true},{"native_window_icon_applied",true},{"icon_applied",false}};
        },[&](const NativeTab&,StyleAction action,const Json&,bool){if(action==StyleAction::restore){++style_restores;return Json{{"restoration_confirmed",restore_style}};}if(fail_style)throw Error(5,"style unavailable");if(pending_style>0){--pending_style;return Json{{"style_applied",false},{"pending",true},{"initializing",true},{"error",""}};}return Json{{"style_applied",true},{"icon_applied",!fail_icon}};},[](DWORD,uint64_t){return ProcessLifetime::running;});
        auto change=[&](const std::string& id,const char* action,bool enabled){return controller.change(tracker,Config{},{{"command",std::string("tabs.")+action},{"tab_id",id},{"enabled",enabled}});};
        auto located=change(b,"locate",true);
        expect(located_guid==2&&located.at("selection_confirmed")==true&&located.at("tab_id")==b,"locate selects the exact same-path tab identity");
        expect(located.at("foreground_activated")==false&&located.at("partial")==true,"missing foreground window cannot confirm full success");
        selected=false;located=change(a,"locate",true);
        expect(located_guid==1&&located.at("selection_confirmed")==false&&located.at("partial")==true,"selection without readback is not confirmed");
        fail_native=true;rejects([&]{change(a,"pin",true);});expect(!controller.list(tracker,Config{})[0]["behavior"]["pinned"].get<bool>(),"failed guard cannot commit pin");fail_native=false;
        pending_style=2;auto ready=change(a,"pin",true);expect(!ready.at("partial").get<bool>()&&ready.at("icon_applied")==true&&pending_style==0,"pending XAML initialization retries before reporting confirmed appearance");
        fail_icon=true;auto missing_icon=change(a,"pin",true);expect(missing_icon.at("partial")==true&&missing_icon.at("icon_applied")==false&&missing_icon.at("native_window_icon_applied")==true,"native HWND icon cannot confirm rendered tab icon");fail_icon=false;
        change(a,"pin",true);change(a,"protect",true);change(a,"pin",false);
        auto controlled=controller.list(tracker,Config{});expect(!controlled[0]["behavior"]["pinned"].get<bool>()&&controlled[0]["behavior"]["effective_close_protection"].get<bool>(),"unpin retains independent native guard");
        expect(!controlled[1]["behavior"]["effective_close_protection"].get<bool>(),"same-path peer remains independent");
        fail_style=true;auto partial=change(a,"lock",true);expect(partial.at("partial")==true&&controller.list(tracker,Config{})[0]["behavior"]["locked"].get<bool>(),"style failure retains committed native lock");
        auto stopped=controller.stop();expect(!stopped.at("complete").get<bool>()&&native_restores==1&&style_restores==1,"stop independently attempts behavior and appearance restore");
        expect(!controller.list(tracker,Config{})[0]["behavior"]["locked"].get<bool>(),"successful native restore clears flags even when appearance restore fails");
        restore_style=true;expect(controller.stop().at("complete")==true&&native_restores==2&&style_restores==2,"restoration retries idempotently");
        expect(workspace_directory_available(root.wstring()),"bounded directory probe accepts live directory");
        expect(!workspace_directory_available((root/L"missing").wstring()),"bounded directory probe rejects missing directory");
        rejects([&]{auto bad=f;bad["restore"]["max_tabs"]=2.5;validate_features(bad);});
        rejects([&]{auto bad=f;bad["restore"]["max_tabs"]=0;validate_features(bad);});
        rejects([&]{auto bad=f;bad["tab_style"]["default"]["unknown"]=true;validate_features(bad);});
        fs::remove_all(root);std::cout<<checks<<" feature checks passed\n";return 0;
    }catch(const std::exception& ex){std::cerr<<"FAIL after "<<checks<<": "<<ex.what()<<'\n';return 1;}
}
