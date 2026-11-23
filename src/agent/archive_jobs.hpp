#pragma once

#include "platform/archive.hpp"
#include <future>
#include <list>

namespace sinan {
class ArchiveJobs {
public:
    struct Job {
        std::string id, state="queued", error, location;
        ArchiveSource source;
        Json settings;
        HWND frame{};
        DWORD process_id{};
        uint64_t process_started{};
        std::stop_source stop;
        std::atomic<int> progress{-1};
        std::optional<ArchiveResult> result;
    };
    ~ArchiveJobs();
    const Job& enqueue(ArchiveSource source, Json settings, HWND frame);
    Job& get(std::string_view id);
    void cancel(Job& job);
    Job* poll();
    bool pending() const;
    Json list() const;
    static Json describe(const Job& job);
private:
    std::list<Job> jobs_;
    Job* active_{};
    std::future<ArchiveResult> work_;
};
}
