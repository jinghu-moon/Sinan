#pragma once
#include "config/config.hpp"
#include "hook/native.hpp"

namespace sinan {
inline constexpr wchar_t style_message[]=L"Sinan.Style.1";
inline std::wstring style_mapping(DWORD pid,uint64_t nonce){return L"Local\\Sinan-Style-"+std::to_wstring(pid)+L"-"+std::to_wstring(nonce);}
enum class StyleAction:uint32_t { inspect=1, apply=2, restore=3, locate=4, open=5, order=6 };
struct StylePacket {
    uint32_t magic=0x54545354,version=1,size=sizeof(StylePacket);
    StyleAction action=StyleAction::inspect;
    uint64_t nonce=0,sender_started=0,target_started=0,window=0,browser_window=0;
    DWORD sender_pid=0;
    GUID guid{};
    bool require_active=false;
    char payload[220*1024]{};
    LONG completed=0;
    HRESULT result=E_PENDING;
    char report[256*1024]{};
};
Json send_style_request(const NativeTab& tab,StyleAction action,const Json& payload=Json::object(),bool require_active=false);
}
