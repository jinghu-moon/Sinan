#include "platform/folder_size_query.hpp"
#include "platform/user_process.hpp"
#include <algorithm>

namespace sinan {
using namespace folder_size_ipc;

struct FolderSizeQuery::Worker {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(Batch), nullptr)};
    Handle request{CreateEventW(&inherit, FALSE, FALSE, nullptr)};
    Handle response{CreateEventW(&inherit, FALSE, FALSE, nullptr)};
    Handle journal{CreateEventW(&inherit, TRUE, FALSE, nullptr)};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    std::wstring instance;
    std::unique_ptr<Handle> process;
    Batch* batch{};

    Worker(std::wstring_view instance_name, bool watch_journal) : instance(instance_name) {
        const auto executable = executable_path().parent_path() / L"sinan-folder-sizes.exe";
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!mapping || !request || !response || !journal || !job ||
            !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw Error(5, system_error("创建文件夹大小查询进程的通信通道"));

        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<std::byte> storage(size);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
            throw Error(5, system_error("创建文件夹大小查询进程的启动属性"));
        struct AttributeList {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributeList() { DeleteProcThreadAttributeList(value); }
        } attributes{list};
        HANDLE inherited[]{mapping.get(), request.get(), response.get(), journal.get()};
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherited, sizeof(inherited), nullptr, nullptr))
            throw Error(5, system_error("配置文件夹大小查询进程的句柄"));

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = list;
        auto command = quote_argument(executable.wstring());
        for (auto handle : inherited) command += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(handle));
        command += watch_journal ? L" journal" : L" selection";
        UserProcessLauncher launcher;
        process = std::make_unique<Handle>();
        auto child = launcher.launch_redirected(executable, std::move(command), executable.parent_path(), startup);
        // Duplicate the process handle so the launcher can retain its existing ownership API.
        HANDLE copy{};
        if (!DuplicateHandle(GetCurrentProcess(), child.process.get(), GetCurrentProcess(), &copy,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            TerminateProcess(child.process.get(), 1);
            throw Error(5, system_error("保留文件夹大小查询进程句柄"));
        }
        process.reset(new Handle(copy));
        if (!AssignProcessToJobObject(job.get(), process->get()) ||
            ResumeThread(child.thread.get()) == DWORD(-1)) {
            TerminateProcess(process->get(), 1);
            throw Error(5, system_error("启动文件夹大小查询进程"));
        }
        batch = static_cast<Batch*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Batch)));
        if (!batch) throw Error(5, system_error("映射文件夹大小查询数据"));
        std::copy(instance.begin(), instance.end(), batch->instance);
    }

    ~Worker() {
        if (batch) UnmapViewOfFile(batch);
    }
};

FolderSizeQuery::FolderSizeQuery(bool watch_journal) : watch_journal_(watch_journal) {}
FolderSizeQuery::~FolderSizeQuery() = default;
void FolderSizeQuery::reset() { worker_.reset(); }
bool FolderSizeQuery::journal_pending() const {
    return worker_ && WaitForSingleObject(worker_->journal.get(), 0) == WAIT_OBJECT_0;
}

FolderSizeResult FolderSizeQuery::query(std::wstring_view instance, std::span<const std::wstring> paths,
                                        std::span<const ItemKind> kinds) {
    if (instance.size() > 128 || paths.size() > max_batch || (!kinds.empty() && kinds.size() != paths.size()))
        throw Error(2, "文件夹大小查询请求过大");
    for (const auto& path : paths)
        if (path.empty() || path.size() >= max_path || path.find(L'\0') != path.npos)
            throw Error(2, "文件夹大小查询路径无效");
    if (!worker_ || worker_->instance != instance) worker_ = std::make_unique<Worker>(instance, watch_journal_);
    auto& batch = *worker_->batch;
    batch.count = static_cast<uint32_t>(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        std::copy(paths[i].begin(), paths[i].end(), batch.paths[i]);
        batch.paths[i][paths[i].size()] = L'\0';
        batch.kinds[i] = kinds.empty() ? ItemKind::folder : kinds[i];
    }
    if (!SetEvent(worker_->request.get())) {
        reset();
        throw Error(5, "无法提交文件夹大小查询");
    }
    HANDLE wait[]{worker_->response.get(), worker_->process->get()};
    const auto outcome = WaitForMultipleObjects(2, wait, FALSE, 1000);
    if (outcome != WAIT_OBJECT_0) {
        reset();
        throw Error(5, outcome == WAIT_TIMEOUT ? "Everything 文件夹大小查询超时" : "文件夹大小查询进程已退出");
    }
    FolderSizeResult result{batch.status, batch.error, {batch.bytes, batch.bytes + paths.size()},
                            batch.journal_status, batch.journal_rescan, {}};
    if (batch.journal_count > max_journal_batch) {
        reset();
        throw Error(5, "Everything 索引日志响应无效");
    }
    for (unsigned i = 0; i < batch.journal_count; ++i) {
        const auto length = wcsnlen_s(batch.journal_paths[i], max_path);
        if (length == max_path) {
            reset();
            throw Error(5, "Everything 索引日志路径无效");
        }
        result.journal_paths.emplace_back(batch.journal_paths[i], length);
    }
    return result;
}
}
