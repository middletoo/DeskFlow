#include "native_catalog.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(){
    namespace fs=std::filesystem;
    auto base=fs::temp_directory_path()/(L"DeskFlow-native-filters-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));fs::create_directories(base);int result=0;
    try{
        desk::ntfs::Root root;root.path=L"C:\\";root.rootFile=5;
        desk::CatalogBuilder builder(base,{root});builder.add({10,5,0,0,100,0,true,false,"项目"});
        builder.add({20,10,0,15,200,0,false,true,"AI 金融 [final].pdf"});builder.add({21,10,0,20,300,0,false,true,"photo.PNG"});builder.add({22,5,0,10,400,0,false,true,"README.md"});
        builder.finish([]{return false;},[](const auto&){});desk::SearchStore store(base);
        auto check=[&](std::wstring text,size_t expected,bool matchCase=false){desk::SearchQuery q;q.text=text;q.matchCase=matchCase;auto rows=store.query(q,100);for(int i=0;i<100&&store.queryPending();++i)rows=store.query(q,100);if(store.queryPending()||rows.size()!=expected)throw std::runtime_error("native filter mismatch");};
        check(L"AI 金融",1);check(L"ext:pdf",1);check(L"type:folder",1);check(L"!ext:pdf",3);check(L"name:README",1);check(L"path:项目",3);check(L"in:C:\\项目",3);check(L"AI | photo",2);check(L"*.pdf",1);check(L"*[final].pdf",1);check(L"ai",0,true);check(L"AI",1,true);check(L"regex:^photo",1);
        std::cout<<"PASS native Unicode, literal candidates, wildcard brackets, case, OR, exclusion, type, extension, scope, path and regex filters\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    std::error_code ec;if(base.parent_path()==fs::temp_directory_path()&&base.filename().wstring().starts_with(L"DeskFlow-native-filters-"))fs::remove_all(base,ec);return result;
}
