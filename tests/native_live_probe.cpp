#include "search.hpp"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>

int wmain(int argc,wchar_t** argv){
    namespace fs=std::filesystem;fs::path data,parent,report;
    for(int i=1;i+1<argc;++i){std::wstring a=argv[i];if(a==L"--data")data=argv[++i];else if(a==L"--fixtures")parent=argv[++i];else if(a==L"--report")report=argv[++i];}
    if(data.empty()||parent.empty()||report.empty())return 2;
    const auto unique=L"deskflow-native-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    const auto base=fs::absolute(parent)/unique;fs::create_directories(base/L"folder");auto primary=base/L"folder"/(unique+L"-primary.txt");nlohmann::json result;
    try{
        desk::SearchStore store(data);
        auto find=[&](const fs::path& p){desk::SearchQuery q;q.text=L"\""+p.filename().wstring()+L"\"";auto rows=store.query(q,100);for(int i=0;i<100&&store.queryPending();++i)rows=store.query(q,100);
            for(const auto& item:rows)if(_wcsicmp(item.path.c_str(),p.c_str())==0)return std::optional<desk::SearchItem>(item);return std::optional<desk::SearchItem>{};};
        auto await=[&](const std::function<bool()>& ready,const char* label){auto began=std::chrono::steady_clock::now();while(!ready()){if(std::chrono::steady_clock::now()-began>std::chrono::seconds(15))throw std::runtime_error(label);std::this_thread::sleep_for(std::chrono::milliseconds(20));}return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();};
        {std::ofstream out(primary);out<<"synthetic";}
        result["create_ms"]=await([&]{auto r=find(primary);return r&&r->sizeKnown&&r->size==9&&r->modified>0;},"native create timeout");auto created=*find(primary);
        {std::ofstream out(primary,std::ios::app);out<<" updated";}
        result["modify_ms"]=await([&]{auto r=find(primary);return r&&r->size==17&&r->modified>created.modified;},"native metadata timeout");
        auto alias=base/L"folder"/(unique+L"-alias.txt");fs::create_hard_link(primary,alias);
        result["hardlink_create_ms"]=await([&]{auto r=find(alias);return r&&r->size==17;},"hardlink create timeout");
        fs::remove(alias);result["hardlink_delete_ms"]=await([&]{return !find(alias);},"hardlink delete timeout");
        auto renamed=base/L"folder"/(unique+L"-renamed.txt");auto beforeRename=*find(primary);fs::rename(primary,renamed);
        result["rename_ms"]=await([&]{auto r=find(renamed);return r&&!find(primary)&&r->id==beforeRename.id;},"rename or stable selection ID timeout");
        fs::rename(base/L"folder",base/L"moved");auto moved=base/L"moved"/renamed.filename();
        result["directory_rename_ms"]=await([&]{return find(moved).has_value()&&!find(renamed);},"directory child path timeout");
        fs::remove(moved);result["delete_ms"]=await([&]{return !find(moved);},"native deletion timeout");
        auto status=store.status();result["indexed_objects"]=status.total;result["complete"]=status.complete;result["limited"]=status.limited;result["ok"]=true;
    }catch(const std::exception& e){result["ok"]=false;result["error"]=e.what();}
    std::ofstream output(report);output<<result.dump(2);output.close();std::cout<<result.dump()<<'\n';
    auto checked=fs::weakly_canonical(base);if(checked.parent_path()==fs::weakly_canonical(parent)&&checked.filename().wstring().starts_with(L"deskflow-native-")){std::error_code ec;fs::remove_all(checked,ec);}
    return result.value("ok",false)?0:1;
}
