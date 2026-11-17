#include "compat/symbols.hpp"

namespace sinan::compat {
Json prepare(const fs::path& cache,bool offline,bool force){
    fs::create_directories(cache);
    auto result_path=fs::absolute(cache)/(L"result-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".json");
    struct Cleanup{fs::path file;~Cleanup(){std::error_code ec;fs::remove(file,ec);}} cleanup{result_path};
    auto helper=executable_path().parent_path()/L"sinan-symbols.exe";
    if(!fs::is_regular_file(helper))throw Error(1,"分发目录缺少 sinan-symbols.exe；请保留配套文件");
    std::wstring command=quote_argument(helper.wstring())+L" --cache-dir "+quote_argument(fs::absolute(cache).wstring())+L" --result "+quote_argument(result_path.wstring());
    if(offline)command+=L" --offline";if(force)command+=L" --force";
    Handle job(CreateJobObjectW(nullptr,nullptr));if(!job)throw Error(1,system_error("Create symbol worker job"));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))throw Error(1,system_error("Limit symbol worker"));
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
    if(!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,helper.parent_path().c_str(),&si,&pi))throw Error(1,system_error("Start symbol worker"));
    Handle process(pi.hProcess),thread(pi.hThread);
    if(!AssignProcessToJobObject(job.get(),process.get())||ResumeThread(thread.get())==DWORD(-1)){
        TerminateProcess(process.get(),1);throw Error(1,"Cannot isolate symbol worker");
    }
    auto wait=WaitForSingleObject(process.get(),90000);
    if(wait!=WAIT_OBJECT_0){TerminateJobObject(job.get(),5);WaitForSingleObject(process.get(),2000);refresh_profile();auto result=compat::status(cache);result["prepared"]=false;result["timed_out"]=true;result["message"]="符号准备超过 90 秒；已终止独立工作进程，已完成的模块缓存保留，可稍后重试或使用离线 PDB。";return result;}
    if(!fs::is_regular_file(result_path)||fs::file_size(result_path)>128*1024)throw Error(5,"Symbol worker did not return a valid report");
    auto result=Json::parse(read_file(result_path));refresh_profile();return result;
}
}
