#pragma once
#include "platform/windows.hpp"
#include "platform/folder_size_protocol.hpp"
#include <memory>
#include <span>
#include <vector>

namespace sinan {
struct FolderSizeResult {
    folder_size_ipc::IndexStatus status;
    uint32_t error;
    std::vector<uint64_t> bytes;
    folder_size_ipc::JournalStatus journal_status;
    bool journal_rescan;
    std::vector<std::wstring> journal_paths;
};

class FolderSizeQuery {
    struct Worker;
    std::unique_ptr<Worker> worker_;
    bool watch_journal_;
public:
    explicit FolderSizeQuery(bool watch_journal = true);
    ~FolderSizeQuery();
    FolderSizeResult query(std::wstring_view instance, std::span<const std::wstring> paths,
                           std::span<const folder_size_ipc::ItemKind> kinds = {});
    bool journal_pending() const;
    void reset();
};
}
