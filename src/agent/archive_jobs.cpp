#include "agent/archive_jobs.hpp"
#include <objbase.h>
#include <algorithm>

namespace sinan {
ArchiveJobs::~ArchiveJobs() {
    if(active_)active_->stop.request_stop();
}

const ArchiveJobs::Job& ArchiveJobs::enqueue(ArchiveSource source,Json settings,HWND frame) {
    for(const auto& job:jobs_)
        if(job.source.key==source.key&&(job.state=="queued"||job.state=="running"||job.state=="canceling"))return job;
    if(jobs_.size()>=32) {
        const auto oldest=std::find_if(jobs_.rbegin(),jobs_.rend(),[](const auto& job){
            return job.state=="completed"||job.state=="failed"||job.state=="canceled";
        });
        if(oldest==jobs_.rend())throw Error(2,"Archive queue is full");
        jobs_.erase(std::prev(oldest.base()));
    }
    GUID identifier{};
    if(FAILED(CoCreateGuid(&identifier)))throw Error(1,"Could not identify archive job");
    wchar_t id[40]{};
    StringFromGUID2(identifier,id,40);
    auto& job=jobs_.emplace_front();
    job.id=utf8(id);
    job.source=std::move(source);
    job.settings=std::move(settings);
    job.frame=frame;
    GetWindowThreadProcessId(frame,&job.process_id);
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,job.process_id));
    FILETIME created{},exited{},kernel{},user{};
    if(process&&GetProcessTimes(process.get(),&created,&exited,&kernel,&user))
        job.process_started=(static_cast<uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    return job;
}

ArchiveJobs::Job& ArchiveJobs::get(std::string_view id) {
    for(auto& job:jobs_)if(job.id==id)return job;
    throw Error(3,"Archive job was not found");
}

void ArchiveJobs::cancel(Job& job) {
    if(job.state=="queued")job.state="canceled";
    else if(job.state=="running") {
        job.stop.request_stop();
        job.state="canceling";
    }
}

ArchiveJobs::Job* ArchiveJobs::poll() {
    if(active_) {
        if(work_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return nullptr;
        auto* done=std::exchange(active_,nullptr);
        try {
            done->result=work_.get();
            done->state="completed";
        } catch(const Error& error) {
            done->state=error.code==7?"canceled":"failed";
            done->error=error.what();
        } catch(const std::exception& error) {
            done->state="failed";
            done->error=error.what();
        }
        return done;
    }
    for(auto it=jobs_.rbegin();it!=jobs_.rend();++it)if(it->state=="queued") {
        it->state="running";
        auto* job=&*it;
        try {
            work_=std::async(std::launch::async,[job] {
                return extract_archive(job->source,job->settings,job->stop.get_token(),&job->progress);
            });
            active_=job;
        } catch(const std::exception& error) {
            it->state="failed";
            it->error=error.what();
            return job;
        }
        break;
    }
    return nullptr;
}

bool ArchiveJobs::pending() const {
    return active_ || std::any_of(jobs_.begin(), jobs_.end(), [](const Job& job) {
        return job.state == "queued";
    });
}

Json ArchiveJobs::describe(const Job& job) {
    Json value={{"id",job.id},{"source",utf8(job.source.selected.wstring())},{"state",job.state},
        {"error",job.error},{"location",job.location}};
    const auto percent=job.progress.load(std::memory_order_relaxed);
    if(percent>=0&&(job.state=="running"||job.state=="canceling"))value["progress_percent"]=percent;
    if(job.result) {
        const auto& result=*job.result;
        auto paths=[](const auto& items) {
            auto values=Json::array();
            for(const auto& item:items)values.push_back(utf8(item.wstring()));
            return values;
        };
        value["output"]=utf8(result.output.wstring());
        value["directory"]=utf8(result.directory.wstring());
        value["file"]=utf8(result.file.wstring());
        value["recycle"]=result.recycle;
        value["recycled"]=paths(result.recycled);
        value["retained"]=paths(result.retained);
    }
    return value;
}

Json ArchiveJobs::list() const {
    auto values=Json::array();
    for(const auto& job:jobs_)values.push_back(describe(job));
    return {{"jobs",std::move(values)}};
}
}
