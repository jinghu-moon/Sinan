#include "compat/symbols.hpp"
#include "feature_exercise.hpp"
#include "hook/identity.hpp"
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace {
HMODULE self{};
void execute(sinan::ExercisePacket& p) {
    using namespace sinan;verify_layout_sender(self,p.sender_pid,p.sender_started);
    if(p.magic!=0x54544558||p.size!=sizeof(p))throw Error(2,"Invalid exercise packet");
    if(!compat::current_profile()->baseline)throw Error(3,"Exercise guard/navigation ABI requires the exact baseline image");
    auto tabs=read_native_tabs(p.tab.window,p.tab.process_id,p.tab.process_started);
    auto found=std::find_if(tabs.begin(),tabs.end(),[&](auto& t){return t.guid==p.tab.guid&&t.host==p.tab.host;});
    if(found==tabs.end())throw Error(3,"Exercise tab identity changed");
    using Get=HRESULT(WINAPI*)(void*,REFIID,void**);Microsoft::WRL::ComPtr<IShellItem> item;
    auto hr=reinterpret_cast<Get>(p.tab.module_base+native_rva("location"))(reinterpret_cast<void*>(p.tab.host+0x10),IID_PPV_ARGS(&item));
    if(FAILED(hr))throw Error(3,"Exercise location unavailable");
    PWSTR raw{};hr=item->GetDisplayName(SIGDN_FILESYSPATH,&raw);if(FAILED(hr)||!raw)throw Error(3,"Exercise requires filesystem directory");
    fs::path path(raw);CoTaskMemFree(raw);wchar_t dll[32768]{};GetModuleFileNameW(self,dll,32768);
    auto probe_root=fs::path(dll).parent_path();
    if(path.parent_path()!=probe_root||!path.filename().wstring().starts_with(L"Sinan-acceptance-"))throw Error(4,"Exercise refuses user work directories");
    if(p.operation==1){using Remove=HRESULT(WINAPI*)(void*,GUID);p.result=reinterpret_cast<Remove>(p.tab.module_base+0x1a6be0)(reinterpret_cast<void*>(p.tab.frame+0xe8),p.tab.guid);}
    else if(p.operation==2){using Close=HRESULT(WINAPI*)(void*,bool);p.result=reinterpret_cast<Close>(p.tab.module_base+0x125da4)(reinterpret_cast<void*>(p.tab.host),false);}
    else if(p.operation==3) {
        fs::path target(p.target);if(target.parent_path()!=probe_root||!target.filename().wstring().starts_with(L"Sinan-acceptance-"))throw Error(4,"Invalid exercise navigation target");
        PIDLIST_ABSOLUTE pidl{};hr=SHParseDisplayName(target.c_str(),nullptr,&pidl,0,nullptr);if(FAILED(hr))throw Error(3,"Exercise target unavailable");
        uintptr_t browser{};SIZE_T read{};ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p.tab.host+0xa0),&browser,sizeof(browser),&read);
        using Navigate=HRESULT(WINAPI*)(void*,PCIDLIST_ABSOLUTE,DWORD,DWORD);p.result=reinterpret_cast<Navigate>(p.tab.module_base+0x137b08)(reinterpret_cast<void*>(browser),pidl,0,0);CoTaskMemFree(pidl);
    }else throw Error(2,"Invalid exercise operation");
}
void process(sinan::ExercisePacket* p){try{execute(*p);}catch(...){p->result=E_FAIL;}InterlockedExchange(&p->completed,1);}
}
extern "C" __declspec(dllexport) LRESULT CALLBACK SinanExerciseHook(int code,WPARAM w,LPARAM l) {
    using namespace sinan;
    if(code>=0){auto m=reinterpret_cast<CWPSTRUCT*>(l);if(m&&m->message==RegisterWindowMessageW(exercise_message)) {
        auto sender=static_cast<DWORD>(m->wParam);auto nonce=static_cast<uint64_t>(m->lParam);
        Handle h(OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,exercise_mapping(sender,nonce).c_str()));
        if(h){auto p=static_cast<ExercisePacket*>(MapViewOfFile(h.get(),FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(ExercisePacket)));
            if(p){if(p->sender_pid==sender&&p->nonce==nonce&&p->tab.window==m->hwnd)process(p);UnmapViewOfFile(p);}}
    }}return CallNextHookEx(nullptr,code,w,l);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH)self=module;return TRUE;}
