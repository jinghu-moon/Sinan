#include "agent/archive_jobs.hpp"
#include <iostream>

int wmain(int argc,wchar_t** argv) {
    using namespace sinan;
    try {
        if(argc!=2)return 2;
        SetConsoleOutputCP(CP_UTF8);
        const auto input=Json::parse(read_file(argv[1]));
        std::vector<std::wstring> selected;
        for(const auto& path:input.at("selected"))selected.push_back(wide(path.get<std::string>()));
        auto job=ArchiveJobs::Job{};
        job.source=archive_source(selected);
        job.settings=input.at("settings");
        auto features=default_features();
        features["archives"]=job.settings;
        validate_features(features,false);
        if(input.value("cancel",false))job.stop.request_stop();
        job.result=extract_archive(job.source,job.settings,job.stop.get_token());
        job.state="completed";
        std::cout<<ArchiveJobs::describe(job).dump()<<'\n';
        return 0;
    } catch(const Error& error) {
        std::cout<<Json{{"error",error.what()},{"code",error.code}}.dump()<<'\n';
        return error.code;
    } catch(const std::exception& error) {
        std::cout<<Json{{"error",error.what()},{"code",1}}.dump(-1,' ',false,Json::error_handler_t::replace)<<'\n';
        return 1;
    }
}
