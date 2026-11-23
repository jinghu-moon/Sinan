#pragma once
#include "config/config.hpp"
#include "hook/native.hpp"

namespace sinan {
inline constexpr wchar_t feature_message[]=L"Sinan.Features.1";
inline std::wstring feature_mapping(DWORD pid,uint64_t nonce){return L"Local\\Sinan-Features-"+std::to_wstring(pid)+L"-"+std::to_wstring(nonce);}
enum class FeatureAction:uint32_t { inspect=1, apply=2, restore=3, locate=4, open=5, reorder=6, close_duplicate=7, external_tabs=8, external_activation=9 };
struct FeaturePacket {
    uint32_t magic=0x54544654,version=1,size=sizeof(FeaturePacket);
    FeatureAction action=FeatureAction::inspect;
    uint64_t nonce=0,sender_started=0,target_started=0,window=0,browser_window=0;
    DWORD sender_pid=0;
    GUID guid{};
    bool require_active=false;
    char payload[220*1024]{};
    LONG completed=0;
    HRESULT result=E_PENDING;
    char report[64*1024]{};
};
Json send_feature_request(const NativeTab& tab,FeatureAction action,const Json& payload=Json::object(),bool require_active=false);
Json send_external_activation_request(bool enabled,uint64_t cookie);
bool is_shell_windows_broker(HANDLE process);
}
