#include "search.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <psapi.h>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static std::uint64_t cpu(HANDLE process){
    FILETIME created{},exited{},kernel{},user{};
    require(GetProcessTimes(process,&created,&exited,&kernel,&user)!=FALSE,"cannot sample background CPU");
    return (std::uint64_t(kernel.dwHighDateTime)<<32)+kernel.dwLowDateTime+
           (std::uint64_t(user.dwHighDateTime)<<32)+user.dwLowDateTime;
}
int wmain(int argc,wchar_t** argv){
    if(argc==5&&std::wstring(argv[1])==L"--worker")
        return desk::indexWorkerMain(argv[2],{argv[3]},std::stoul(argv[4]));
    auto base=std::filesystem::temp_directory_path()/(L"DeskIndexBudget-"+std::to_wstring(GetCurrentProcessId()));
    auto data=base/L"data",root=base/L"fixture";PROCESS_INFORMATION child{};int result=1;
    try{
        std::filesystem::create_directories(root);
        for(int i=0;i<12000;++i)std::ofstream(root/(L"entry-"+std::to_wstring(i)+L".txt"));
        desk::SearchStore store(data);wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        auto arguments=L"\""+std::wstring(exe)+L"\" --worker \""+data.wstring()+L"\" \""+root.wstring()+L"\" "+std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{sizeof(startup)};
        require(CreateProcessW(exe,arguments.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE,"cannot launch paced index worker");
        const auto deadline=GetTickCount64()+8000;
        while(store.query(L"entry").empty()&&GetTickCount64()<deadline)Sleep(20);
        require(!store.query(L"entry").empty(),"background work must publish searchable results promptly");
        const auto began=GetTickCount64(),before=cpu(child.hProcess);
        Sleep(8000);
        const double elapsed=(GetTickCount64()-began)/1000.0;
        const auto processors=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        const double percent=(cpu(child.hProcess)-before)/10000000.0/elapsed/processors*100;
        std::cout<<"background cpu_percent="<<percent<<" processors="<<processors<<" seconds="<<elapsed<<'\n';
        require(percent<(processors>=4?1.0:2.0),"sustained synthetic indexing exceeded background CPU budget");
        const auto queryStart=GetTickCount64();require(!store.query(L"entry").empty(),"paced index results disappeared");
        require(GetTickCount64()-queryStart<500,"foreground indexed query slowed down during background work");
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        GetProcessMemoryInfo(child.hProcess,(PROCESS_MEMORY_COUNTERS*)&memory,sizeof(memory));
        const auto stop=GetTickCount64();desk::stopIndexWorker(data);
        require(WaitForSingleObject(child.hProcess,2000)==WAIT_OBJECT_0,"CPU rest must be immediately interruptible on stop");
        std::cout<<"PASS paced indexing cpu_percent="<<percent<<" seconds="<<elapsed
                 <<" private_bytes="<<memory.PrivateUsage<<" stop_ms="<<GetTickCount64()-stop<<'\n';
        result=0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';desk::stopIndexWorker(data);if(child.hProcess)WaitForSingleObject(child.hProcess,5000);}
    if(child.hThread)CloseHandle(child.hThread);if(child.hProcess)CloseHandle(child.hProcess);
    std::error_code error;std::filesystem::remove_all(base,error);return result;
}
