#pragma once
#include "platform/folder_size_protocol.hpp"
#include <windows.h>
#include <string>

namespace sinan::folder_size_ipc {
constexpr unsigned cache_capacity = 512;
constexpr unsigned cache_path_capacity = max_path;
constexpr uint64_t lease_duration_ms = 6500;

struct CachedSize {
    uint64_t bytes;
    uint64_t requested;
    uint64_t queried;
    uint64_t successful_at;
    bool dirty;
    wchar_t path[cache_path_capacity];
};

struct Cache {
    alignas(8) volatile LONG64 expires;
    volatile LONG busy;
    uint32_t count;
    unsigned next;
    IndexStatus index;
    CachedSize entries[cache_capacity];
};

inline std::wstring cache_name(DWORD owner, uint64_t started, DWORD explorer) {
    return L"Local\\Sinan.FolderSizes.2-" + std::to_wstring(owner) + L"-" +
        std::to_wstring(started) + L"-" + std::to_wstring(explorer);
}

struct CacheLock {
    Cache* acquired;
    explicit CacheLock(Cache* cache)
        : acquired(InterlockedCompareExchange(&cache->busy, 1, 0) == 0 ? cache : nullptr) {}
    ~CacheLock() { if (acquired) InterlockedExchange(&acquired->busy, 0); }
    CacheLock(const CacheLock&) = delete;
    CacheLock& operator=(const CacheLock&) = delete;
    explicit operator bool() const { return acquired != nullptr; }
};
}
