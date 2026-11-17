#include "ipc/pipe.hpp"
#include "installer/environment.hpp"
#include <bcrypt.h>
#include <sddl.h>
#include <objbase.h>
#include <array>
#include <cstring>
#include <vector>

namespace tabtail {
namespace {
constexpr uint32_t max_message = 1024 * 1024;
class Operation {
    Handle event_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
public:
    OVERLAPPED overlap{};
    Operation() { if (!event_) throw Error(1, system_error("创建 IPC 事件")); overlap.hEvent = event_.get(); }
    DWORD finish(HANDLE file, DWORD timeout) {
        if (WaitForSingleObject(overlap.hEvent, timeout) != WAIT_OBJECT_0) {
            CancelIoEx(file, &overlap);
            DWORD ignored = 0; GetOverlappedResult(file, &overlap, &ignored, TRUE);
            throw Error(5, "代理通信超时，运行时结果未确认");
        }
        DWORD transferred = 0;
        if (!GetOverlappedResult(file, &overlap, &transferred, FALSE)) throw Error(1, system_error("代理通信失败"));
        return transferred;
    }
};
void transfer(HANDLE pipe, void* data, DWORD size, bool writing, ULONGLONG deadline) {
    auto* bytes = static_cast<BYTE*>(data);
    DWORD offset = 0;
    while (offset < size) {
        auto now = GetTickCount64();
        if (now >= deadline) throw Error(5, "代理通信超时，运行时结果未确认");
        Operation operation;
        DWORD done = 0;
        BOOL ok = writing ? WriteFile(pipe, bytes + offset, size - offset, &done, &operation.overlap)
                          : ReadFile(pipe, bytes + offset, size - offset, &done, &operation.overlap);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) throw Error(1, system_error("读写代理管道"));
            done = operation.finish(pipe, static_cast<DWORD>(deadline - now));
        }
        if (!done) throw Error(1, "代理管道已关闭");
        offset += done;
    }
}
void send_json(HANDLE pipe, const Json& value, ULONGLONG deadline) {
    auto text = value.dump();
    if (text.empty() || text.size() > max_message) throw Error(2, "IPC 报文超出 1 MiB 限制");
    auto size = static_cast<uint32_t>(text.size());
    transfer(pipe, &size, sizeof(size), true, deadline);
    transfer(pipe, text.data(), size, true, deadline);
}
Json receive_json(HANDLE pipe, ULONGLONG deadline) {
    uint32_t size = 0;
    transfer(pipe, &size, sizeof(size), false, deadline);
    if (!size || size > max_message) throw Error(2, "IPC 报文长度无效");
    std::string text(size, '\0');
    transfer(pipe, text.data(), size, false, deadline);
    // Bound parser recursion in addition to the byte-length limit.
    auto result = Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 32) throw Error(2, "IPC JSON 嵌套过深");
        return true;
    });
    if (!result.is_object()) throw Error(2, "IPC 报文必须为对象");
    return result;
}
}
std::wstring agent_endpoint(const fs::path& root) {
    auto key = normalize_path_entry(fs::absolute(root).lexically_normal().wstring());
    std::array<UCHAR, 32> digest{};
    auto result = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(key.data()),
        static_cast<ULONG>(key.size() * sizeof(wchar_t)), digest.data(), static_cast<ULONG>(digest.size()));
    if (result < 0) throw Error(1, "计算代理命名空间失败");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring hash;
    for (auto byte : digest) { hash += hex[byte >> 4]; hash += hex[byte & 15]; }
    return L"\\\\.\\pipe\\TabTail-" + current_sid() + L"-" + std::to_wstring(session_id()) + L"-" + hash;
}
void verify_peer_process(DWORD pid) {
    DWORD session = 0;
    if (!ProcessIdToSessionId(pid, &session) || session != session_id()) throw Error(4, "代理对端不属于当前会话");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) throw Error(4, system_error("查询 IPC 对端进程"));
    std::wstring image(32768, L'\0'); DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &size)) throw Error(4, system_error("查询 IPC 对端映像"));
    image.resize(size);
    std::error_code ec;
    if (!fs::equivalent(fs::path(image), executable_path(), ec) || ec) throw Error(4, "IPC 对端不是同一 TabTail 可执行文件，请使用启动代理的命令路径");
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw)) throw Error(4, system_error("查询 IPC 对端用户"));
    Handle token(raw); DWORD bytes = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes)) throw Error(4, system_error("读取 IPC 对端用户"));
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) throw Error(4, "读取对端 SID 失败");
    std::wstring actual(sid); LocalFree(sid);
    if (actual != current_sid()) throw Error(4, "IPC 对端不属于当前用户");
}
std::optional<Json> agent_request(const fs::path& root, Json request, DWORD timeout) {
    auto command=request.value("command",std::string{});if(command=="workspace.restore"||command=="tabs.batch")timeout=std::max<DWORD>(timeout,45000);
    auto name = agent_endpoint(root);
    HANDLE connected = INVALID_HANDLE_VALUE;
    auto connect_deadline = GetTickCount64() + timeout;
    do {
        connected = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (connected != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(name.c_str(), 100);
    } while (GetTickCount64() < connect_deadline);
    Handle pipe(connected);
    if (!pipe) {
        auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return std::nullopt;
        throw Error(error == ERROR_ACCESS_DENIED ? 4 : 1, system_error("连接后台代理", error));
    }
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &server)) throw Error(4, "无法验证代理服务进程");
    verify_peer_process(server);
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw Error(1, "无法创建请求 ID");
    wchar_t id[40]{}; StringFromGUID2(guid, id, 40);
    request["request_id"] = utf8(id); request["protocol_version"] = 1;
    auto deadline = GetTickCount64() + timeout;
    send_json(pipe.get(), request, deadline);
    auto response = receive_json(pipe.get(), deadline);
    // Server waits for this receipt before disconnecting, so unread response bytes are never discarded.
    BYTE receipt = 1; transfer(pipe.get(), &receipt, sizeof(receipt), true, deadline);
    if (response.value("protocol_version", 0) != 1 || response.value("request_id", "") != request.at("request_id").get<std::string>())
        throw Error(1, "代理响应协议或请求 ID 不匹配");
    if (!response.contains("code") || !response["code"].is_number_integer() || !response.contains("result") || !response["result"].is_object())
        throw Error(1, "代理响应结构无效");
    return response;
}
struct PipeServer::Impl {
    Handle pipe;
    Operation connect;
    bool pending = false;
    explicit Impl(const fs::path& root) : pipe([&]() {
        auto sddl = L"D:P(A;;GA;;;" + current_sid() + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            throw Error(4, system_error("创建管道权限"));
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        auto handle = CreateNamedPipeW(agent_endpoint(root).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 5000, &security);
        auto error = GetLastError(); LocalFree(descriptor);
        if (handle == INVALID_HANDLE_VALUE) throw Error(1, system_error("创建代理管道", error));
        return handle;
    }()) {}
    ~Impl() {
        if (pending) { CancelIoEx(pipe.get(), &connect.overlap); DWORD ignored; GetOverlappedResult(pipe.get(), &connect.overlap, &ignored, TRUE); }
    }
};
PipeServer::PipeServer(const fs::path& root) : impl_(std::make_unique<Impl>(root)) {}
PipeServer::~PipeServer() = default;
bool PipeServer::poll(const std::function<Json(const Json&)>& handler, DWORD wait) {
    auto& state = *impl_;
    if (!state.pending) {
        ResetEvent(state.connect.overlap.hEvent);
        if (!ConnectNamedPipe(state.pipe.get(), &state.connect.overlap)) {
            auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) SetEvent(state.connect.overlap.hEvent);
            else if (error != ERROR_IO_PENDING) throw Error(1, system_error("等待代理客户端", error));
        }
        state.pending = true;
    }
    if (WaitForSingleObject(state.connect.overlap.hEvent, wait) != WAIT_OBJECT_0) return false;
    state.pending = false;
    try {
        ULONG client = 0;
        if (!GetNamedPipeClientProcessId(state.pipe.get(), &client)) throw Error(4, "无法验证客户端身份");
        verify_peer_process(client);
        auto deadline = GetTickCount64() + 5000;
        auto request = receive_json(state.pipe.get(), deadline);
        auto command=request.value("command",std::string{});if(command=="workspace.restore"||command=="tabs.batch")deadline=GetTickCount64()+45000;
        Json response{{"protocol_version", 1}, {"request_id", request.value("request_id", "")}};
        try {
            if (request.value("protocol_version", 0) != 1 || response["request_id"] == "") throw Error(2, "请求协议无效");
            response["result"] = handler(request);
            const auto& result=response["result"];
            bool observing_reload=command=="reload"&&request.value("config_only",false)&&result.value("config_received",false)&&result.value("mode",std::string{})=="observe";
            response["code"] = (!observing_reload&&!result.value("title_applied",true))||result.value("partial",false)||(result.contains("complete")&&!result.at("complete").get<bool>()) ? 5 : 0;
        } catch (const Error& e) { response["code"] = e.code; response["result"] = {{"message", e.what()}}; }
        catch (const PolicyError& e) { response["code"] = 2; response["result"] = {{"message", e.what()}}; }
        catch (const std::exception& e) { response["code"] = 1; response["result"] = {{"message", e.what()}}; }
        send_json(state.pipe.get(), response, deadline);
        BYTE receipt = 0; transfer(state.pipe.get(), &receipt, sizeof(receipt), false, deadline);
    } catch (const std::exception&) {
        // Invalid/broken clients cannot terminate the agent or hold the pipe indefinitely.
    }
    DisconnectNamedPipe(state.pipe.get());
    return true;
}
}
