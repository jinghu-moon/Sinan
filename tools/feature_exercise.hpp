#pragma once
#include "hook/features.hpp"
namespace sinan {
inline constexpr wchar_t exercise_message[]=L"Sinan.AcceptanceExercise.1";
inline std::wstring exercise_mapping(DWORD pid,uint64_t nonce){return L"Local\\Sinan-Acceptance-"+std::to_wstring(pid)+L"-"+std::to_wstring(nonce);}
// Test-only DLL: never installed or packaged. Restricts destructive calls to
// an identity-checked directory named Sinan-acceptance-* in the probe folder.
struct ExercisePacket {
    uint32_t magic=0x54544558,size=sizeof(ExercisePacket),operation=0;
    DWORD sender_pid{};uint64_t sender_started{},nonce{};
    NativeTab tab;
    wchar_t target[32768]{};
    LONG completed{};HRESULT result=E_PENDING;
};
}
