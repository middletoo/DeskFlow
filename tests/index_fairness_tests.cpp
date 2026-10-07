#include "search.hpp"
#include "sqlite3.h"
#include <fstream>
#include <iostream>
#include <thread>
int wmain(int argc,wchar_t** argv){
    if(argc==6&&std::wstring(argv[1])==L"--worker")return desk::indexWorkerMain(argv[2],{argv[3],argv[4]},std::stoul(argv[5]));
    auto base=std::filesystem::temp_directory_path()/(L"DeskIndexFairness-"+std::to_wstring(GetCurrentProcessId()));
    auto data=base/L"data",large=base/L"first",small=base/L"second";std::filesystem::create_directories(large);std::filesystem::create_directories(small);PROCESS_INFORMATION process{};int outcome=0;
    try{
        for(int i=0;i<5000;++i)std::ofstream(large/(L"synthetic-"+std::to_wstring(i)+L".txt"));
        std::ofstream(small/L"AI 金融.txt")<<"public synthetic fixture";
        desk::SearchStore store(data);wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        auto args=L"\""+std::wstring(exe)+L"\" --worker \""+data.wstring()+L"\" \""+large.wstring()+L"\" \""+small.wstring()+L"\" "+std::to_wstring(GetCurrentProcessId());STARTUPINFOW startup{sizeof(startup)};
        if(!CreateProcessW(exe,args.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))throw std::runtime_error("launch fairness worker");
        desk::SearchQuery query;query.text=L"AI 金融";auto end=GetTickCount64()+8000;bool found=false;
        while(GetTickCount64()<end&&!found){found=!store.query(query).empty();if(!found)Sleep(10);}
        if(!found)throw std::runtime_error("later root failed to publish its existing filename promptly");
        sqlite3* db{};sqlite3_open_v2((data/L"files.db").string().c_str(),&db,SQLITE_OPEN_READONLY,nullptr);sqlite3_stmt* state{};sqlite3_prepare_v2(db,"SELECT complete FROM search_roots ORDER BY id LIMIT 1",-1,&state,nullptr);bool firstComplete=sqlite3_step(state)==SQLITE_ROW&&sqlite3_column_int(state,0)!=0;sqlite3_finalize(state);sqlite3_close(db);
        if(firstComplete)throw std::runtime_error("later root was postponed until the large first root completed");
        desk::stopIndexWorker(data);if(WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0)throw std::runtime_error("fairness worker failed to stop");
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';outcome=1;desk::stopIndexWorker(data);if(process.hProcess)WaitForSingleObject(process.hProcess,5000);}
    if(process.hThread)CloseHandle(process.hThread);if(process.hProcess)CloseHandle(process.hProcess);std::error_code error;std::filesystem::remove_all(base,error);
    if(!outcome)std::cout<<"PASS later disk root indexed before the large first root completes\n";return outcome;
}
