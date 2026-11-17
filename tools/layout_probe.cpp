#include "hook/layout.hpp"
#include "platform/shell_tabs.hpp"
#include <iostream>

// Developer probe. Copy alongside sinan-layout.dll as sinan-agent.exe in an
// isolated folder to satisfy the same client identity validation as the CLI.
int main(int argc,char** argv) {
    using namespace sinan;
    try {
        ShellTabEnumerator enumerator;auto scan=enumerator.scan();
        auto found=std::find_if(scan.tabs.begin(),scan.tabs.end(),[](const auto& tab){return tab.native.has_value();});
        if(found==scan.tabs.end())throw Error(3,"No supported Explorer tab observed");
        auto mode=argc>1?std::string(argv[1]):"inspect";
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
                samples.push_back(send_layout_request(*found->native,action,options));
                if(i!=5)Sleep(300);
            }
            results.push_back({{"requested",current},{"samples",samples}});
        }
        std::cout<<results.dump(2)<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
