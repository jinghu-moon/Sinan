#include "agent/layout_tracker.hpp"
#include "app/service.hpp"
#include <iostream>

using namespace tabtail;
namespace {
int checks=0;
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F action){bool rejected=false;try{action();}catch(const Error& e){rejected=e.code==2;}expect(rejected,"invalid appearance accepted");}
}
int main(){
    auto root=fs::temp_directory_path()/(L"TabTail-appearance-"+std::to_wstring(GetCurrentProcessId()));
    try{
        ConfigStore store(root);auto c=store.load();expect(c.tabs.width_mode=="system","default must preserve Explorer layout");
        for(auto mode:{"system","fixed","content"}){
            c=store.update([&](Config& value){value.tabs={mode,360,120,520};});
            expect(store.load().tabs==c.tabs,"TOML appearance round trip");
            expect(config_from_json(config_json(c)).tabs==c.tabs,"IPC appearance round trip");
        }
        auto legacy=config_json(c);legacy.erase("appearance");expect(config_from_json(legacy).tabs==TabAppearance{},"old config defaults to system");
        for(auto key:{"shortcuts","notifications","tab_behavior","tab_style","restore","directories"})legacy.erase(key);legacy["schema_version"]=1;legacy.erase("ui");legacy.erase("rules");expect(config_from_json(legacy).tabs.width_mode=="system","schema1 compatible");
        for(auto mode:{"SYSTEM","auto","invalid"})rejects([&]{appearance_from_json({{"width_mode",mode}});});
        for(auto number:{Json(79),Json(1001),Json(320.5),Json("320"),Json(true),Json(nullptr),Json(UINT64_MAX)})
            for(auto key:{"width","min_width","max_width"})rejects([&]{appearance_from_json({{key,number}});});
        rejects([&]{appearance_from_json({{"min_width",500},{"max_width",200}});});
        rejects([&]{appearance_from_json({{"unknown",2}});});
        for(auto text:{"0","79","1001","320x","3.2","-120",""," 320"})rejects([&]{parse_tab_width(text);});
        expect(parse_tab_width("80")==80&&parse_tab_width("1000")==1000,"width endpoints");
        auto previous=read_file(store.path());rejects([&]{store.update([](Config& v){v.tabs.min_width=900;});});
        expect(read_file(store.path())==previous,"invalid width edit preserves configuration");
        auto revision=c.revision;
        auto result=application_action(root,{{"action","appearance"},{"revision",revision},{"tabs",{{"width_mode","fixed"},{"width",420}}}});
        expect(result.at("result").at("saved")==true&&store.load().tabs.width==420,"application saves appearance atomically");
        rejects([&]{application_action(root,{{"action","appearance"},{"revision",revision},{"tabs",appearance_json({})}});});
        expect(store.load().depth==c.depth&&store.load().rules==c.rules,"width preserves title policy");
        {
            auto before=store.load();auto text=application_action(root,{{"action","config.show"}}).at("result").at("text").get<std::string>();
            expect(text==config_toml(before),"TUI config report uses the saved effective TOML");
            auto style_path=root/L"style-test.json";auto desired_style=before.features.at("tab_style");desired_style["default"]={{"text",{{"bold",true}}}};
            atomic_write(style_path,desired_style.dump());
            auto checked=application_action(root,{{"action","style.read"},{"file",utf8(style_path.wstring())},{"expected",before.features.at("tab_style")}});
            expect(checked.at("result").at("style")==desired_style&&store.load().features==before.features,"validating imported style captures content without modifying config");
            atomic_write(style_path,"{invalid");bool invalid_style=false;try{application_action(root,{{"action","style.read"},{"file",utf8(style_path.wstring())},{"expected",before.features.at("tab_style")}});}catch(...){invalid_style=true;}
            expect(invalid_style&&store.load().features==before.features,"invalid style file cannot alter saved settings");
            atomic_write(style_path,std::string(200001,'x'));rejects([&]{application_action(root,{{"action","style.read"},{"file",utf8(style_path.wstring())},{"expected",before.features.at("tab_style")}});});
            for(auto action:{"install","uninstall","path.add","path.remove"})rejects([&]{application_action(root,{{"action",action}});});
            expect(store.load().features==before.features,"custom-root installation requests are rejected before mutations");
            auto altered=store.update([](Config& value){value.depth=6;value.separator=L" / ";});
            application_action(root,{{"action","display.reset"},{"revision",altered.revision}});
            auto reset=store.load();expect(reset.depth==2&&reset.separator==L"\\"&&reset.tabs==altered.tabs&&reset.rules==altered.rules&&reset.features==altered.features&&reset.enabled==altered.enabled,"display reset changes only depth and separator");
        }
        auto baseline=store.load();auto desired=baseline.tabs;desired.width=700;
        store.update([](Config& value){value.language="en";value.depth=5;value.enabled=true;});
        result=application_action(root,{{"action","appearance"},{"revision",baseline.revision},
            {"expected_tabs",appearance_json(baseline.tabs)},{"tabs",appearance_json(desired)}});
        expect(result.at("result").at("saved")==true&&store.load().tabs.width==700,"unrelated changes do not invalidate a width draft");
        expect(store.load().language=="en"&&store.load().depth==5&&store.load().enabled,"width save preserves concurrent unrelated changes");
        auto concurrent=store.update([](Config& value){value.tabs.width=500;});
        previous=read_file(store.path());
        result=application_action(root,{{"action","appearance"},{"revision",baseline.revision},
            {"expected_tabs",appearance_json(baseline.tabs)},{"tabs",appearance_json(desired)}});
        expect(result.at("code")==2&&result.at("result").at("conflict")=="appearance","concurrent width edit returns a structured conflict");
        expect(read_file(store.path())==previous,"width conflict preserves latest file");
        result=application_action(root,{{"action","appearance"},{"revision",baseline.revision},
            {"expected_tabs",appearance_json(baseline.tabs)},{"tabs",appearance_json(concurrent.tabs)}});
        expect(result.at("result").at("saved")==true,"same desired width is accepted idempotently despite old revision");
        result=application_action(root,{{"action","appearance"},{"revision",concurrent.revision},
            {"expected_tabs",appearance_json(concurrent.tabs)},{"tabs",appearance_json(desired)}});
        expect(result.at("result").at("saved")==true&&store.load().tabs.width==700,"reloaded width baseline allows retry");
        LayoutTracker tracker;
        NativeTab a;a.window=reinterpret_cast<HWND>(1);a.process_id=10;a.process_started=100;
        NativeTab duplicate=a;duplicate.browser_window=reinterpret_cast<HWND>(99);
        NativeTab b=a;b.window=reinterpret_cast<HWND>(2);
        int calls=0,applies=0,restores=0;bool pending=false,fail=false;
        auto transport=[&](const NativeTab&,LayoutAction action,const TabAppearance& value)->Json{
            ++calls;if(fail)throw Error(3,"simulated layout failure");
            expect(value.width_mode=="system"||value.width_mode=="fixed"||value.width_mode=="content","valid transport policy");
            if(action==LayoutAction::apply)++applies;else ++restores;
            return {{"layout_applied",!pending},{"pending",pending}};
        };
        auto thread=[](HWND hwnd){return static_cast<DWORD>(reinterpret_cast<uintptr_t>(hwnd));};
        bool ended=false;auto probe=[&](DWORD,uint64_t){return ended?ProcessLifetime::ended:ProcessLifetime::running;};
        auto sync=[&](std::vector<NativeTab> tabs,bool enabled,TabAppearance appearance){return tracker.synchronize(tabs,enabled,appearance,transport,thread,probe);};
        TabAppearance fixed{"fixed",320,120,480};
        expect(sync({a},false,fixed).at("complete")==true&&calls==0,"observe mode never injects width");
        sync({a},true,{});expect(calls==0,"system default never injects width");
        pending=true;auto state=sync({a,duplicate,b},true,fixed);
        expect(calls==2&&state.at("pending_threads")==2&&!state.at("layout_applied").get<bool>(),"pending ACK never claims layout applied; deduplicate UI threads");
        pending=false;state=sync({a,b},true,fixed);
        expect(state.at("layout_applied")==true&&applies==4,"measurement confirms both UI threads");
        calls=0;state=sync({},true,fixed);expect(calls==0&&!state.at("layout_applied").get<bool>(),"missing observations never renew leases or claim measured");
        state=sync({},false,fixed);expect(state.at("pending_threads")==2,"unobserved restoration remains pending");
        fail=true;state=sync({a,b},false,{});expect(state.at("failed_threads")==2,"restoration failures remain retryable");
        fail=false;state=sync({a,b},false,{});expect(state.at("complete")==true&&restores==2,"disable restores all controlled threads");
        calls=0;sync({a,b},false,{});expect(calls==0,"restored default does not reinject");
        sync({a},true,fixed);ended=true;calls=0;sync({},false,{});expect(calls==0,"ended lifetime not sent restoration");
        ended=false;state=sync({a},true,TabAppearance{"content",320,140,420});expect(state.at("layout_applied")==true,"content mode transported");
        state=sync({a},true,{});expect(state.at("complete")==true&&!state.at("layout_applied").get<bool>(),"switch to system restores width independently of titles");
        fs::remove_all(root);std::cout<<checks<<" appearance checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
