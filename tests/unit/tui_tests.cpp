#include "platform/windows.hpp"
#include <iostream>
namespace sinan {int verify_tui_components(const fs::path& artifacts);}
int main(int argc,char** argv) {
    try {return sinan::verify_tui_components(argc>1?sinan::fs::path(sinan::wide(argv[1])):sinan::fs::path(L"tui-artifacts"));}
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
