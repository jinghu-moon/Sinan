#include <windows.h>
#include <Everything3.h>
#include "folder_size_protocol.hpp"
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sinan::folder_size_ipc;

namespace {
using Client = std::unique_ptr<EVERYTHING3_CLIENT, decltype(&Everything3_DestroyClient)>;

Client connect(const std::wstring& instance) {
    Client client(Everything3_ConnectW(instance.empty() ? nullptr : instance.c_str()), Everything3_DestroyClient);
    if (!client && instance.empty()) client.reset(Everything3_ConnectW(L"1.5a"));
    return client;
}

std::wstring extended_path(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\")) return std::wstring(path);
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + std::wstring(path.substr(2));
    return L"\\\\?\\" + std::wstring(path);
}

class Journal {
    HANDLE event_;
    std::mutex mutex_;
    std::vector<std::wstring> paths_;
    JournalStatus status_{};
    bool rescan_{};
    uint64_t next_{};

    void enqueue(std::wstring_view parent, std::wstring_view name) {
        if (rescan_ || parent.empty() || name.empty()) return;
        std::wstring path(parent);
        if (path.back() != L'\\') path += L'\\';
        path += name;
        if (std::find(paths_.begin(), paths_.end(), path) != paths_.end()) return;
        if (paths_.size() == max_journal_batch || path.size() >= max_path) {
            rescan_ = true;
            paths_.clear();
        } else {
            paths_.push_back(std::move(path));
        }
    }

    void set_status(JournalStatus status, bool rescan = false) {
        std::lock_guard lock(mutex_);
        if (status_ != status || rescan) SetEvent(event_);
        status_ = status;
        rescan_ = rescan_ || rescan;
    }

    static EVERYTHING3_BOOL EVERYTHING3_API record(void* context, const EVERYTHING3_JOURNAL_CHANGEW* change) {
        auto& journal = *static_cast<Journal*>(context);
        std::lock_guard lock(journal.mutex_);
        if (change->change_id != journal.next_) journal.rescan_ = true;
        journal.next_ = change->change_id + 1;
        try {
            const std::wstring_view parent(change->old_path, change->old_path_len);
            journal.enqueue(parent, {change->old_name, change->old_name_len});
            if (change->new_name_len) {
                const auto destination = change->new_path_len
                    ? std::wstring_view(change->new_path, change->new_path_len) : parent;
                journal.enqueue(destination, {change->new_name, change->new_name_len});
            }
        } catch (...) {
            journal.rescan_ = true;
        }
        if (journal.rescan_ || !journal.paths_.empty()) SetEvent(journal.event_);
        return TRUE;
    }

public:
    explicit Journal(HANDLE event) : event_(event) {}

    void read(const std::wstring& instance) {
        uint64_t id{};
        for (;;) {
            auto client = connect(instance);
            EVERYTHING3_JOURNAL_INFO info{};
            if (client && Everything3_GetJournalInfo(client.get(), &info) && info.max_size) {
                const bool gap = !id || id != info.journal_id || next_ < info.first_item_index || next_ > info.next_item_index;
                id = info.journal_id;
                if (gap) next_ = info.next_item_index;
                set_status(JournalStatus::listening, gap);
                constexpr auto flags = EVERYTHING3_READ_JOURNAL_FLAG_CHANGE_ID |
                    EVERYTHING3_READ_JOURNAL_FLAG_OLD_PATH | EVERYTHING3_READ_JOURNAL_FLAG_OLD_NAME |
                    EVERYTHING3_READ_JOURNAL_FLAG_NEW_PATH | EVERYTHING3_READ_JOURNAL_FLAG_NEW_NAME;
                Everything3_ReadJournalW(client.get(), id, next_, flags, this, record);
            }
            set_status(JournalStatus::unavailable);
            Sleep(2000);
        }
    }

    void drain(Batch& batch) {
        std::lock_guard lock(mutex_);
        batch.journal_status = status_;
        batch.journal_rescan = rescan_;
        batch.journal_count = rescan_ ? 0 : static_cast<uint32_t>(paths_.size());
        for (unsigned i = 0; i < batch.journal_count; ++i) {
            std::copy(paths_[i].begin(), paths_[i].end(), batch.journal_paths[i]);
            batch.journal_paths[i][paths_[i].size()] = L'\0';
        }
        paths_.clear();
        rescan_ = false;
        ResetEvent(event_);
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 6) return 2;
    const std::wstring_view mode(argv[5]);
    const bool watch_journal = mode == L"journal";
    if (!watch_journal && mode != L"selection") return 2;
    const auto parse_handle = [](const wchar_t* argument) -> HANDLE {
        wchar_t* end{};
        const auto value = std::wcstoull(argument, &end, 10);
        return value && !*end ? reinterpret_cast<HANDLE>(value) : nullptr;
    };
    const auto mapping = parse_handle(argv[1]);
    const auto request = parse_handle(argv[2]);
    const auto response = parse_handle(argv[3]);
    const auto journal_event = parse_handle(argv[4]);
    if (!mapping || !request || !response || !journal_event) return 2;
    auto* batch = static_cast<Batch*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Batch)));
    if (!batch) return 2;

    Client client(nullptr, Everything3_DestroyClient);
    auto journal = std::make_shared<Journal>(journal_event);
    std::wstring instance;
    bool started = false;
    // The instance is fixed for the lifetime of this worker.
    while (WaitForSingleObject(request, INFINITE) == WAIT_OBJECT_0) {
        if (batch->count > max_batch || batch->instance[128]) return 2;
        if (!started) {
            started = true;
            instance = batch->instance;
            // The journal connection blocks independently for the lifetime of this job-owned process.
            if (watch_journal) std::thread([journal, instance] { journal->read(instance); }).detach();
        }
        batch->error = 0;
        batch->status = IndexStatus::disconnected;
        std::fill_n(batch->bytes, batch->count, unknown);
        bool needs_index = watch_journal;
        bool indexed_folder[max_batch]{};
        for (uint32_t i = 0; i < batch->count; ++i) {
            if (batch->paths[i][max_path - 1]) return 2;
            if (watch_journal) {
                indexed_folder[i] = true;
                continue;
            }
            const auto kind = batch->kinds[i];
            const std::wstring_view path(batch->paths[i]);
            if (kind == ItemKind::folder && (path.size() < 3 || path[1] != L':' || path[2] != L'\\')) continue;
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(extended_path(path).c_str(), GetFileExInfoStandard, &data)) continue;
            if (kind == ItemKind::file && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                batch->bytes[i] = (uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
            if (kind == ItemKind::folder &&
                (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == FILE_ATTRIBUTE_DIRECTORY) {
                needs_index = indexed_folder[i] = true;
            }
        }
        if (!needs_index) {
            if (!SetEvent(response)) break;
            continue;
        }
        if (!client) client = connect(instance);
        if (!client) {
            batch->error = Everything3_GetLastError();
        } else if (!Everything3_IsDBLoaded(client.get())) {
            batch->error = Everything3_GetLastError();
            batch->status = batch->error ? IndexStatus::disconnected : IndexStatus::loading;
            if (batch->error) client.reset();
        } else {
            batch->status = IndexStatus::ready;
            for (uint32_t i = 0; i < batch->count; ++i) {
                if (!indexed_folder[i]) continue;
                batch->bytes[i] = Everything3_GetFolderSizeFromFilenameW(client.get(), batch->paths[i]);
                if (batch->bytes[i] != unknown) continue;
                batch->error = Everything3_GetLastError();
                if (batch->error == EVERYTHING3_ERROR_INVALID_COMMAND) {
                    batch->status = IndexStatus::incompatible;
                    break;
                }
                if (batch->error == EVERYTHING3_ERROR_DISCONNECTED ||
                    batch->error == EVERYTHING3_ERROR_SHUTDOWN) {
                    batch->status = IndexStatus::disconnected;
                    client.reset();
                    break;
                }
            }
        }
        journal->drain(*batch);
        if (!SetEvent(response)) break;
    }
    UnmapViewOfFile(batch);
    return 0;
}
