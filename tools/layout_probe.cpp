#include "hook/layout.hpp"
#include "platform/shell_tabs.hpp"
#include <iostream>

// Developer probe. Copy alongside sinan-layout.dll as sinan-agent.exe in an
// isolated folder to satisfy the same client identity validation as the CLI.
int main(int argc,char** argv) {
    using namespace sinan;
    try {
        ShellTabEnumerator enumerator;auto scan=enumerator.scan();
        const auto mode=argc>1?std::string(argv[1]):"inspect";
        if(mode=="command-bar") {
            HWND window=argc>2?reinterpret_cast<HWND>(std::stoull(argv[2])):GetForegroundWindow();
            auto target=std::find_if(scan.tabs.begin(),scan.tabs.end(),[&](const auto& tab){
                return tab.native&&tab.native->window==window;
            });
            if(target==scan.tabs.end())throw Error(3,"Target has no supported Explorer tab");
            const auto test=argc>3?std::string_view(argv[3]):"cycle";
            const auto report=[&](std::string_view phase,LayoutAction action) {
                auto result=send_layout_payload(*target->native,action,{},{{"hide_more",test=="hide-more"}});
                result["phase"]=phase;
                std::cout<<result.dump()<<std::endl;
                return result;
            };
            try {
                if(test=="inspect") {
                    report("inspect",LayoutAction::command_bar_inspect);
                    return 0;
                }
                const auto check=[&](const Json& result,bool enabled) {
                    bool left=false,right=false;
                    for(const auto& bar:result.at("bars")) {
                        const bool secondary=bar.at("bar")=="right";
                        (secondary?right:left)=true;
                        const auto expected=enabled?(secondary?2u:1u):0u;
                        if(bar.at("probes")!=expected)throw Error(1,"Unexpected number of probe buttons");
                        if(!secondary&&test=="hide-more"&&((bar.at("more_visibility")==2)!=enabled))
                            throw Error(1,"Unexpected overflow button visibility");
                    }
                    if(!left||!right)throw Error(1,"Both Explorer command bars are required");
                };
                for(int attempt=0;attempt<20;++attempt) {
                    auto result=report("enable",LayoutAction::command_bar_enable);
                    if(!result.value("pending",false)&&!result.at("bars").empty())break;
                    Sleep(250);
                }
                for(int i=0;i<(test=="hold"?60:3);++i) {
                    Sleep(500);
                    check(report("renew",LayoutAction::command_bar_enable),true);
                }
                const bool expire=test=="expire";
                if(expire)Sleep(11500);
                check(report("inspect",LayoutAction::command_bar_inspect),!expire);
                check(report("disable",LayoutAction::command_bar_disable),false);
                return 0;
            } catch(...) {
                try { report("disable",LayoutAction::command_bar_disable); } catch(...) {}
                throw;
            }
        }
        auto found=std::find_if(scan.tabs.begin(),scan.tabs.end(),[](const auto& tab){return tab.native.has_value();});
        if(found==scan.tabs.end())throw Error(3,"No supported Explorer tab observed");
        TabAppearance options;options.width_mode=mode=="inspect"||mode=="restore"||mode=="cycle"?"system":mode;
        if(argc>2)options.width=std::stoi(argv[2]);
        if(argc>3)options.min_width=std::stoi(argv[3]);if(argc>4)options.max_width=std::stoi(argv[4]);
        auto action=mode=="inspect"?LayoutAction::inspect:mode=="restore"?LayoutAction::restore:LayoutAction::apply;
        Json results=Json::array();
        auto modes=mode=="cycle"?std::vector<std::string>{"inspect","fixed","content","restore"}:std::vector<std::string>{mode};
        for(const auto& current:modes) {
            options.width_mode=current=="fixed"?"fixed":current=="content"?"content":"system";
            action=current=="inspect"?LayoutAction::inspect:current=="restore"?LayoutAction::restore:LayoutAction::apply;
            Json samples=Json::array();
            for(int i=0;i<6;++i) {
                samples.push_back(send_layout_request(*found->native,action,{options}));
                if(i!=5)Sleep(300);
            }
            results.push_back({{"requested",current},{"samples",samples}});
        }
        std::cout<<results.dump(2)<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
