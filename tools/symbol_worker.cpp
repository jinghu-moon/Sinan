#include "compat/symbols.hpp"
#include <iostream>

int wmain(int argc,wchar_t** argv){
    using namespace sinan;using namespace sinan::compat;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    fs::path cache=cache_directory(),result_file,pdb;bool offline=false,force=false,single=false;Module module=Module::frame;Json report;
    try{
        for(int i=1;i<argc;++i){std::wstring_view arg=argv[i];
            if(arg==L"--offline")offline=true;else if(arg==L"--force")force=true;
            else if(arg==L"--module"&&i+1<argc){single=true;std::wstring_view value=argv[++i];if(value==L"frame")module=Module::frame;else if(value==L"view")module=Module::view;else if(value==L"ui")module=Module::ui;else throw Error(2,"Module must be frame, view or ui");}
            else if((arg==L"--cache-dir"||arg==L"--result"||arg==L"--pdb")&&i+1<argc){auto value=fs::absolute(argv[++i]);if(arg==L"--cache-dir")cache=value;else if(arg==L"--result")result_file=value;else pdb=value;}
            else throw Error(2,"Usage: sinan-symbols [--cache-dir PATH] [--offline] [--force] [--module frame|view|ui] [--pdb FILE]");
        }
        report=single||!pdb.empty()?resolve_module(cache,offline,force,module,pdb):resolve_all(cache,offline,force);
    }catch(const std::exception& ex){report={{"prepared",false},{"error",ex.what()},{"message",ex.what()}};}
    try{if(result_file.empty())std::cout<<report.dump(2)<<'\n';else atomic_write(result_file,report.dump(2)+"\n");}catch(...){return 5;}
    return report.value("prepared",false)?0:5;
}
