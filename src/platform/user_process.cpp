#include "platform/user_process.hpp"
#include "platform/explorer.hpp"
#include <sddl.h>
#include <userenv.h>
#include <vector>

namespace tabtail {
namespace {
bool is_elevated(HANDLE token) {
    TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes))
        throw Error(4, system_error("读取令牌提权状态"));
    return elevation.TokenIsElevated != 0;
}
void validate_user_token(HANDLE token, bool preserve_desktop = false) {
    DWORD session = 0, bytes = 0;
    if (!GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &bytes) || session != session_id())
        throw Error(4, "用户令牌不属于当前会话");
    if (!preserve_desktop && is_elevated(token)) throw Error(4, "目标令牌仍具有管理员权限");
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> user(bytes);
    if (!GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes)) throw Error(4, system_error("读取令牌用户"));
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) throw Error(4, system_error("读取令牌 SID"));
    std::wstring value(sid); LocalFree(sid);
    if (value != current_sid()) throw Error(4, "用户令牌不属于当前用户");
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
    std::vector<BYTE> integrity(bytes);
    if (!GetTokenInformation(token, TokenIntegrityLevel, integrity.data(), bytes, &bytes)) throw Error(4, system_error("读取令牌完整性级别"));
    auto label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
    auto level = *GetSidSubAuthority(label, *GetSidSubAuthorityCount(label) - 1);
    if (level < SECURITY_MANDATORY_MEDIUM_RID ||
        (preserve_desktop ? level > SECURITY_MANDATORY_HIGH_RID : level >= SECURITY_MANDATORY_HIGH_RID))
        throw Error(4, "目标令牌不是可用的桌面完整性级别");
}
HANDLE duplicate_user_token(HANDLE source, bool preserve_desktop = false) {
    validate_user_token(source, preserve_desktop);
    HANDLE copy = nullptr;
    if (!DuplicateTokenEx(source, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY,
        nullptr, SecurityImpersonation, TokenPrimary, &copy)) throw Error(4, system_error("保留桌面用户主令牌"));
    return copy;
}
HANDLE prepare_token() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw Error(4, system_error("读取当前进程令牌"));
    Handle own(raw);
    if (!is_elevated(own.get())) return nullptr; // Existing ordinary-user launch path.
    PRIVILEGE_SET required{}; required.PrivilegeCount = 1; required.Control = PRIVILEGE_SET_ALL_NECESSARY;
    if (!LookupPrivilegeValueW(nullptr, SE_IMPERSONATE_NAME, &required.Privilege[0].Luid))
        throw Error(4, system_error("检查普通用户启动权限，尚未结束 Explorer"));
    required.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL permitted = FALSE;
    if (!PrivilegeCheck(own.get(), &required, &permitted) || !permitted)
        throw Error(4, "当前管理员令牌缺少已启用的 SeImpersonatePrivilege，尚未结束 Explorer。");

    // Prefer the actual desktop user's token. The candidate must also be a
    // verified system Explorer belonging to this SID and session.
    DWORD shell_pid = 0;
    std::string reason;
    HANDLE desktop_raw = nullptr;
    GetWindowThreadProcessId(GetShellWindow(), &shell_pid);
    if (shell_pid) {
        for (auto candidate : explorer_processes()) if (candidate.pid == shell_pid) {
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shell_pid));
            if (process) OpenProcessToken(process.get(), TOKEN_QUERY | TOKEN_DUPLICATE, &desktop_raw);
            break;
        }
    }
    Handle desktop_token(desktop_raw);
    if (desktop_token) {
        try { return duplicate_user_token(desktop_token.get()); } catch (const Error& error) { reason = error.what(); }
    }
    // The desktop may already be absent. UAC's linked filtered token provides
    // the same user without administrator elevation; never use a foreign SID.
    TOKEN_LINKED_TOKEN linked{}; DWORD bytes = 0;
    if (GetTokenInformation(own.get(), TokenLinkedToken, &linked, sizeof(linked), &bytes)) {
        Handle filtered(linked.LinkedToken);
        try { return duplicate_user_token(filtered.get()); } catch (const Error& error) { reason += std::string("；UAC: ") + error.what(); }
    }
    // UAC-disabled / unsplit desktops may already run elevated. Preserve that
    // verified desktop token rather than rejecting administrator terminals.
    if (desktop_token) return duplicate_user_token(desktop_token.get(), true);
    throw Error(4, "无法取得当前用户当前会话的桌面令牌，尚未结束 Explorer；" + reason);
}
}
UserProcessLauncher::UserProcessLauncher() : token_(prepare_token()) {
    if (token_) elevated_context_ = is_elevated(token_.get());
    if (token_ && !CreateEnvironmentBlock(&environment_, token_.get(), FALSE))
        throw Error(4, system_error("准备桌面用户环境，尚未结束 Explorer"));
}
UserProcessLauncher::~UserProcessLauncher() { if (environment_) DestroyEnvironmentBlock(environment_); }
UserProcess UserProcessLauncher::launch(const fs::path& executable, std::wstring command, DWORD flags) const {
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    BOOL created = FALSE;
    if (uses_user_token()) {
        wchar_t desktop[] = L"winsta0\\default"; startup.lpDesktop = desktop;
        created = CreateProcessWithTokenW(token_.get(), 0, executable.c_str(), command.data(),
            flags | CREATE_UNICODE_ENVIRONMENT, environment_, executable.parent_path().c_str(), &startup, &process);
    } else {
        created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, flags,
            nullptr, executable.parent_path().c_str(), &startup, &process);
    }
    if (!created) throw Error(4, system_error("以桌面用户令牌启动进程"));
    return UserProcess(process.hProcess, process.hThread);
}
}
