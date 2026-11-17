#include "client/agent.hpp"
namespace sinan {
namespace {
Json required(const fs::path& root, Json request) {
    auto response = agent_request(root, std::move(request));
    if (!response) throw Error(3, "标签观察代理未运行；请先执行 sinan agent start。");
    return *response;
}
void check(const Json& response) {
    auto code = response.at("code").get<int>();
    if (code) throw Error(code, response.at("result").value("message", "代理请求失败"));
}
}
Json observer_status(const fs::path& root) {
    auto response = agent_request(root, {{"command", "status"}});
    if (!response) return {{"running", false}, {"mode", "observe"}, {"hook_available", false}};
    check(*response);
    return response->at("result");
}
Json start_observer_agent(const fs::path& root) {
    auto prior = observer_status(root);
    if (prior.value("running", false)) { prior["message"] = "标签代理已运行。"; return prior; }
    // Validate configuration before spawning a background process.
    ConfigStore(root).load();
    auto exe = executable_path().parent_path()/L"sinan-agent.exe";
    auto command = quote_argument(exe.wstring()) + L" --data-dir " + quote_argument(root.wstring());
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION raw{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &startup, &raw))
        throw Error(1, system_error("启动标签观察代理"));
    Handle process(raw.hProcess), thread(raw.hThread);
    auto deadline = GetTickCount64() + 12000;
    while (GetTickCount64() < deadline) {
        try {
            auto result = observer_status(root);
            if (result.value("running", false)) {
                result["message"] = "标签观察代理已启动；执行 sinan start 启用真实标题定制。";
                return result;
            }
        } catch (const Error& e) { if (e.code == 4) throw; }
        if (WaitForSingleObject(process.get(), 100) == WAIT_OBJECT_0) throw Error(1, "标签观察代理启动失败；可直接运行 sinan-agent.exe 查看错误。");
    }
    TerminateProcess(process.get(), 1); WaitForSingleObject(process.get(), 2000);
    throw Error(5, "标签观察代理启动超时，已终止本次启动的进程。");
}
Json stop_observer_agent(const fs::path& root) {
    auto before = agent_request(root, {{"command", "status"}});
    if (!before) return {{"stopped", true}, {"message", "标签观察代理未运行。"}};
    check(*before);
    auto pid = before->at("result").at("pid").get<DWORD>();
    Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) throw Error(4, system_error("等待代理退出"));
    verify_peer_process(pid);
    auto response = required(root, {{"command", "stop"}}); check(response);
    if (response.at("result").value("runtime_id", "") != before->at("result").value("runtime_id", ""))
        throw Error(5, "代理在停止期间重启，请检查状态");
    if (WaitForSingleObject(process.get(), 6000) != WAIT_OBJECT_0) throw Error(5, "代理退出超时；未强制结束进程");
    auto feature_restore=response.at("result").value("feature_restoration",Json::object());
    return {{"stopped",true},{"feature_restoration",feature_restore},{"restoration",response.at("result").at("restoration")},{"complete",feature_restore.value("complete",false)},{"message",feature_restore.value("complete",false)?"代理已停止，保护与外观已恢复。":"代理已停止；部分恢复未确认，失效租约将继续解除限制。"}};
}
Json require_agent_result(const fs::path& root, Json request, int& result_code) {
    auto response = required(root, std::move(request));
    result_code = response.at("code").get<int>();
    return response.at("result");
}
}
