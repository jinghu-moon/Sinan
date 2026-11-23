#pragma once
#include <cstdint>

namespace sinan::folder_size_ipc {
constexpr unsigned max_batch = 16;
constexpr unsigned max_path = 32761;
constexpr unsigned max_journal_batch = 32;
constexpr uint64_t unknown = UINT64_MAX;

enum class IndexStatus : uint32_t { disconnected, loading, ready, incompatible };
enum class JournalStatus : uint32_t { unavailable, listening };
enum class ItemKind : uint8_t { folder, file };

struct Batch {
    wchar_t instance[129]{};
    uint32_t count{};
    IndexStatus status{};
    uint32_t error{};
    wchar_t paths[max_batch][max_path]{};
    uint64_t bytes[max_batch]{};
    JournalStatus journal_status{};
    bool journal_rescan{};
    uint32_t journal_count{};
    wchar_t journal_paths[max_journal_batch][max_path]{};
    ItemKind kinds[max_batch]{};
};
}
