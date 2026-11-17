#include "platform/windows.hpp"
#include <iostream>
namespace tabtail {int verify_tui_components(const fs::path& artifacts);}
int main(int argc,char** argv) {
    try {return tabtail::verify_tui_components(argc>1?tabtail::fs::path(tabtail::wide(argv[1])):tabtail::fs::path(L"tui-artifacts"));}
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
