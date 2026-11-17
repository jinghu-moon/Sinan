#include "ipc/pipe.hpp"
#include <future>
#include <iostream>

using namespace sinan;
namespace {
int checks = 0;
void expect(bool value, const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
void invalid_client(const fs::path& root, bool oversized) {
    auto endpoint = agent_endpoint(root);
    HANDLE opened=INVALID_HANDLE_VALUE;auto deadline=GetTickCount64()+2000;
    do {opened=CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(opened!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_PIPE_BUSY)break;WaitNamedPipeW(endpoint.c_str(),100);}while(GetTickCount64()<deadline);
    Handle pipe(opened);
    if (!pipe) throw Error(1, system_error("测试客户端连接失败"));
    if (oversized) {
        uint32_t size = 1024 * 1024 + 1; DWORD written = 0;
        if (!WriteFile(pipe.get(), &size, sizeof(size), &written, nullptr)) throw Error(1, "测试写入失败");
    }
    // Missing input / oversized input must be disconnected without killing the server.
    BYTE byte; DWORD read = 0;
    expect(!ReadFile(pipe.get(), &byte, 1, &read, nullptr), "malformed client disconnected");
}
void invalid_protocol(const fs::path& root,const std::string& product,int version){
    auto endpoint=agent_endpoint(root);HANDLE opened=INVALID_HANDLE_VALUE;auto deadline=GetTickCount64()+2000;
    do{opened=CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(opened!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_PIPE_BUSY)break;WaitNamedPipeW(endpoint.c_str(),100);}while(GetTickCount64()<deadline);
    Handle pipe(opened);if(!pipe)throw Error(1,"Protocol test connection failed");
    Json request{{"command","must-not-execute"},{"request_id","test"},{"protocol_version",version}};
    if(!product.empty())request["product"]=product;
    auto text=request.dump();uint32_t size=static_cast<uint32_t>(text.size());DWORD done{};
    if(!WriteFile(pipe.get(),&size,4,&done,nullptr)||!WriteFile(pipe.get(),text.data(),size,&done,nullptr))throw Error(1,"Protocol test write failed");
    if(!ReadFile(pipe.get(),&size,4,&done,nullptr)||size>1024*1024)throw Error(1,"Protocol test response failed");
    std::string bytes(size,'\0');DWORD offset=0;
    while(offset<size){if(!ReadFile(pipe.get(),bytes.data()+offset,size-offset,&done,nullptr)||!done)throw Error(1,"Protocol response incomplete");offset+=done;}
    BYTE receipt=1;WriteFile(pipe.get(),&receipt,1,&done,nullptr);
    auto result=Json::parse(bytes);expect(result.at("code")==2,"wrong or missing protocol rejected before dispatch");
}

}
int main() {
    try {
        auto root = fs::temp_directory_path() / (L"Sinan-ipc-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        expect(agent_endpoint(root) == agent_endpoint(root / L"."), "equivalent roots share endpoint");
        expect(agent_endpoint(root) != agent_endpoint(root / L"other"), "different config roots isolated");
        std::promise<void> ready;
        auto server = std::async(std::launch::async, [&] {
            PipeServer pipe(root);
            ready.set_value();
            int handled = 0;
            auto deadline = GetTickCount64() + 20000;
            while (handled < 7 && GetTickCount64() < deadline) {
                if (pipe.poll([](const Json& r) { if(r.at("command")!="echo")throw Error(1,"Invalid protocol reached handler");return Json{{"echo", "ok"}}; }, 100)) ++handled;
            }
            if (handled != 7) throw std::runtime_error("server didn't recover from invalid clients");
        });
        ready.get_future().get();
        invalid_protocol(root,"",1);
        invalid_protocol(root,"TabTail",1);
        invalid_protocol(root,"Sinan",99);
        invalid_client(root, true);
        auto first = agent_request(root, {{"command", "echo"}});
        expect(first && first->at("code").get<int>() == 0 && first->at("result").at("echo").get<std::string>() == "ok", "valid request succeeds after oversized frame");
        invalid_client(root, false); // Tests the five-second frame read timeout.
        auto second = agent_request(root, {{"command", "echo"}});
        expect(second && second->at("code").get<int>() == 0, "server survives idle client timeout");
        server.get();
        expect(!agent_request(root, {{"command", "echo"}}), "stopped endpoint is absent");
        std::cout << checks << " IPC checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
