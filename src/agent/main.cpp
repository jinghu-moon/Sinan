#include "agent/runtime.hpp"
#include <shellapi.h>
#include <objbase.h>
#include <iostream>

// Independent host: no CLI parser, terminal UI, or application command dispatch.
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)return 2;
    auto root=sinan::data_directory();
    bool valid=count==1||(count==3&&std::wstring_view(args[1])==L"--data-dir");
    if(count==3&&valid)root=sinan::fs::absolute(args[2]);
    LocalFree(args);if(!valid)return 2;
    auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com))return 1;
    int code=0;
    try {code=sinan::run_observer_agent(root);}
    catch(const sinan::Error& e){std::cerr<<"Sinan Agent: "<<e.what()<<'\n';code=e.code;}
    catch(const std::exception& e){std::cerr<<"Sinan Agent: "<<e.what()<<'\n';code=1;}
    CoUninitialize();return code;
}
