#include "ntfs_catalog.hpp"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>

int wmain(int argc,wchar_t** argv){
    std::wstring root;std::filesystem::path report,fixture;bool scan=false;
    for(int i=1;i<argc;++i){std::wstring a=argv[i];if(a==L"--root"&&i+1<argc)root=argv[++i];else if(a==L"--report"&&i+1<argc)report=argv[++i];else if(a==L"--fixture"&&i+1<argc)fixture=argv[++i];else if(a==L"--scan")scan=true;}
    nlohmann::json result;
    try{
        const auto began=std::chrono::steady_clock::now();desk::ntfs::Volume volume(root);
        uint64_t visited=0,names=0,metadata=0,extensions=0;
        if(!fixture.empty()){
            HANDLE h=CreateFileW(fixture.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);BY_HANDLE_FILE_INFORMATION info{};
            if(h==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(h,&info))throw std::runtime_error("Synthetic fixture identity failed");CloseHandle(h);
            auto r=volume.file((uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow);
            result["fixture_record_present"]=r.has_value();if(r){result["fixture_names"]=r->names.size();result["fixture_size"]=r->size;result["fixture_size_known"]=r->sizeKnown;result["fixture_modified"]=r->modified;result["fixture_link_count"]=r->links;}
            if(!r||r->names.size()<2||!r->sizeKnown||r->size!=15||!r->modified)throw std::runtime_error("Raw MFT hardlink/size/time validation failed");
            for(const auto& n:r->names)if(n.text!=L"primary.txt"&&n.text!=L"alias.txt"&&!(n.text.starts_with(L"alias-")&&n.text.ends_with(L".txt")))throw std::runtime_error("Synthetic hardlink name content mismatch");
            result["synthetic_hardlink_names"]=r->names.size();result["synthetic_metadata_verified"]=true;
        }
        try{volume.enumerate([&](const auto& r){++visited;names+=r.names.size();metadata+=r.sizeKnown&&r.modified>0;extensions+=r.base!=0;},[&]{return !scan&&visited>=2048;},[](auto){});}
        catch(const std::exception& e){if(scan||std::string(e.what())!="Native indexing stopped")throw;}
        result.update({{"valid_records",visited},{"names",names},{"metadata_records",metadata},{"extension_records",extensions},{"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count()},{"allocated_record_slots",volume.info.records},{"reader",volume.info.raw?"raw_extents":"file_record_api"}});
        if(!fixture.empty()){
            HANDLE h=CreateFileW(fixture.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);BY_HANDLE_FILE_INFORMATION info{};
            if(h==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(h,&info))throw std::runtime_error("Synthetic fixture identity failed");CloseHandle(h);
            uint64_t ref=(uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
            auto r=volume.file(ref);if(!r||r->names.size()<2||!r->sizeKnown||r->size!=15||!r->modified)throw std::runtime_error("Raw MFT hardlink/size/time validation failed");
            result["synthetic_hardlink_names"]=r->names.size();result["synthetic_metadata_verified"]=true;
        }
        result["ok"]=true;
    }catch(const std::exception& e){
        result["ok"]=false;
        const char* raw=e.what();int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,raw,-1,nullptr,0);
        UINT code=n?CP_UTF8:CP_ACP;if(!n)n=MultiByteToWideChar(code,0,raw,-1,nullptr,0);
        std::wstring w(n,L'\0');MultiByteToWideChar(code,0,raw,-1,w.data(),n);
        int m=WideCharToMultiByte(CP_UTF8,0,w.data(),n,nullptr,0,nullptr,nullptr);std::string utf(m,'\0');WideCharToMultiByte(CP_UTF8,0,w.data(),n,utf.data(),m,nullptr,nullptr);if(!utf.empty())utf.pop_back();result["error"]=utf;
    }
    if(!report.empty()){std::ofstream out(report);out<<result.dump(2);}
    std::cout<<result.dump()<<'\n';return result.value("ok",false)?0:1;
}
