#include "agent/runtime.hpp"
#include <shellapi.h>
#include <objbase.h>
#include <iostream>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Independent host: no CLI parser, terminal UI, or application command dispatch.
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)return 2;
    auto root=sinan::data_directory();
    const bool from_login=count==4&&std::wstring_view(args[3])==L"--login";
    const bool has_root=(count==3||from_login)&&std::wstring_view(args[1])==L"--data-dir";
    const bool valid=count==1||has_root;
    if(has_root)root=sinan::fs::absolute(args[2]);
    LocalFree(args);if(!valid)return 2;
    auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com))return 1;
    int code=0;
    try {code=sinan::run_observer_agent(root,from_login);}
    catch(const sinan::Error& e){std::cerr<<"Sinan Agent: "<<e.what()<<'\n';code=e.code;}
    catch(const std::exception& e){std::cerr<<"Sinan Agent: "<<e.what()<<'\n';code=1;}
    CoUninitialize();return code;
}
