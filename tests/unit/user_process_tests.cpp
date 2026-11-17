#include "platform/user_process.hpp"
#include <sddl.h>
#include <iostream>
#include <vector>

using namespace sinan;
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--child") return 0;
    try {
        auto expect = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        HANDLE own_raw = nullptr;
        expect(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own_raw), "open caller token");
        Handle own(own_raw); TOKEN_ELEVATION caller{}; DWORD bytes = 0;
        expect(GetTokenInformation(own.get(), TokenElevation, &caller, sizeof(caller), &bytes), "caller elevation");
        UserProcessLauncher launcher;
        expect(!caller.TokenIsElevated || launcher.uses_user_token() || launcher.launches_elevated(), "elevated caller selects verified desktop context");
        auto exe = executable_path();
        auto child = launcher.launch(exe, quote_argument(exe.wstring()) + L" --child", CREATE_SUSPENDED | CREATE_NO_WINDOW);
        // Always release a suspended child, including assertion failures.
        struct ChildCleanup {
            HANDLE process, thread;
            ~ChildCleanup() { ResumeThread(thread); if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) TerminateProcess(process, 1); }
        } cleanup{child.process.get(), child.thread.get()};
        FILETIME birth{},exit{},kernel{},user_time{};
        expect(GetProcessTimes(child.process.get(),&birth,&exit,&kernel,&user_time),"read child lifetime");
        auto started=(uint64_t(birth.dwHighDateTime)<<32)|birth.dwLowDateTime;
        auto child_pid=GetProcessId(child.process.get());
        expect(process_lifetime(child_pid,started)==ProcessLifetime::running,"running process lifetime recognized");
        expect(process_lifetime(child_pid,started+1)==ProcessLifetime::ended,"different birth time rejects reused PID");
        HANDLE raw = nullptr;
        expect(OpenProcessToken(child.process.get(), TOKEN_QUERY, &raw), "open child token");
        Handle token(raw); TOKEN_ELEVATION elevation{};
        expect(GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &bytes), "child elevation");
        expect((elevation.TokenIsElevated != 0) == launcher.launches_elevated(), "child has selected desktop elevation");
        DWORD session = 0;
        expect(GetTokenInformation(token.get(), TokenSessionId, &session, sizeof(session), &bytes), "child session");
        expect(session == session_id(), "same interactive session");
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes); std::vector<BYTE> user(bytes);
        expect(GetTokenInformation(token.get(), TokenUser, user.data(), bytes, &bytes), "child user");
        LPWSTR sid = nullptr;
        expect(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid), "child SID");
        std::wstring actual(sid); LocalFree(sid);
        expect(actual == current_sid(), "same user");
        GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &bytes); std::vector<BYTE> integrity(bytes);
        expect(GetTokenInformation(token.get(), TokenIntegrityLevel, integrity.data(), bytes, &bytes), "child integrity");
        auto label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
        auto level = *GetSidSubAuthority(label, *GetSidSubAuthorityCount(label) - 1);
        expect(level >= SECURITY_MANDATORY_MEDIUM_RID &&
            (launcher.launches_elevated() ? level == SECURITY_MANDATORY_HIGH_RID : level < SECURITY_MANDATORY_HIGH_RID), "selected desktop integrity");
        expect(ResumeThread(child.thread.get()) != static_cast<DWORD>(-1), "resume child");
        expect(WaitForSingleObject(child.process.get(), 5000) == WAIT_OBJECT_0, "child exits");
        expect(process_lifetime(child_pid,started)==ProcessLifetime::ended,"exited process recognized with open process handle");
        expect(process_lifetime(MAXDWORD,started)==ProcessLifetime::ended,"nonexistent process recognized");
        DWORD code = 1; expect(GetExitCodeProcess(child.process.get(), &code) && code == 0, "child succeeds");
        if (caller.TokenIsElevated && !launcher.launches_elevated()) {
            auto ordinary = launcher.launch(exe, quote_argument(exe.wstring()) + L" --ordinary-parent", CREATE_NO_WINDOW);
            expect(WaitForSingleObject(ordinary.process.get(), 10000) == WAIT_OBJECT_0, "ordinary caller test exits");
            expect(GetExitCodeProcess(ordinary.process.get(), &code) && code == 0, "ordinary caller launch path also succeeds");
        }
        std::cout << "User launch verified: caller_elevated=" << caller.TokenIsElevated << ", child_elevated=" << elevation.TokenIsElevated
            << ", same SID/session, integrity=" << level << ". No Explorer restarted.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
