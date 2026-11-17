#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
// GUI-subsystem launcher keeps login startup invisible. No script interpreter,
// elevation, Explorer restart or installer identity is required.
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)return 2;
    std::wstring root;if(count==3&&std::wstring_view(args[1])==L"--data-dir")root=args[2];LocalFree(args);
    if(root.empty()||root.find(L'"')!=root.npos)return 2;
    wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))return 1;
    auto exe=std::filesystem::path(path).parent_path()/L"sinan-agent.exe";
    auto quoted_root=root;size_t trailing=0;for(auto it=root.rbegin();it!=root.rend()&&*it==L'\\';++it)++trailing;quoted_root.append(trailing,L'\\');
    std::wstring command=L"\""+exe.wstring()+L"\" --data-dir \""+quoted_root+L"\"";
    STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&process))return 1;
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return 0;
}
