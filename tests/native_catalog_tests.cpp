#include "native_catalog.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <chrono>
#include <thread>
#include <atomic>

static void require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
static std::vector<desk::SearchItem> complete(desk::NativeCatalog& reader,const desk::SearchQuery& q,size_t limit,const desk::CatalogPredicate& predicate){
    auto rows=reader.query(q,limit,predicate);auto end=GetTickCount64()+15000;
    while(reader.pending()&&GetTickCount64()<end)rows=reader.query(q,limit,predicate);
    require(!reader.pending(),"native query starved");return rows;
}
int wmain(int argc,wchar_t** argv){
    namespace fs=std::filesystem;
    if(argc==3&&std::wstring(argv[1])==L"--build-lookup"){
        try{const auto began=std::chrono::steady_clock::now();desk::buildCatalogLookup(argv[2],[]{return false;},[](const std::wstring& step){std::wcout<<step<<L'\n';});std::cout<<"lookup_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count()<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    }
    size_t count=argc>1?std::stoull(argv[1]):20000;
    auto base=fs::temp_directory_path()/(L"DeskFlow-native-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    fs::create_directories(base);int result=0;
    try{
        const auto began=std::chrono::steady_clock::now();desk::ntfs::Root root;root.path=L"C:\\";root.rootFile=5;root.serial=7;root.journal=9;root.nextUsn=100;
        desk::CatalogBuilder build(base,{root});
        build.add({100,5,0,0,0,0,true,false,"alpha"});build.add({101,5,0,0,0,0,true,false,"beta"});
        for(size_t i=0;i<count;++i){auto num=std::to_string(i);num=std::string(8-num.size(),'0')+num;
            build.add({1000+i,i%2?100u:101u,0,i%101,i%13?1000000+i:0,0,false,i%17!=0,"Report-"+num+(i%2?".txt":".pdf")});}
        build.add({42,100,0,15,700,0,false,true,"primary.txt"});build.add({42,101,0,15,700,0,false,true,"alias.txt"});
        require(build.finish([]{return false;},[](const auto&) {})==0,"synthetic parents unresolved");
        auto built=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
        desk::NativeCatalog reader(base);require(reader.available()&&reader.total()==count+4,"snapshot count mismatch");
        auto all=[](const desk::CatalogView&){return true;};
        for(auto sort:{desk::SearchSort::Name,desk::SearchSort::Path,desk::SearchSort::Size,desk::SearchSort::Type,desk::SearchSort::Modified,desk::SearchSort::Id})for(bool descending:{false,true}){
            desk::SearchQuery q;q.sort=sort;q.descending=descending;std::set<int64_t> seen;desk::SearchItem previous;bool unknown=false;
            // Page through the full fixture on the default regression workload.
            size_t maximum=count<=20000?count+4:800;
            while(seen.size()<maximum){auto page=complete(reader,q,113,all);if(page.empty())break;
                for(const auto& item:page){require(seen.insert(item.id).second,"duplicate across native pages");require(!item.path.empty(),"missing native path");
                    if(sort==desk::SearchSort::Modified){if(!item.modified)unknown=true;else require(!unknown,"known time followed unknown group");if(previous.id&&previous.modified&&item.modified)require(descending?previous.modified>=item.modified:previous.modified<=item.modified,"global time order changed");}
                    if(sort==desk::SearchSort::Size){bool known=item.sizeKnown&&!item.folder;if(!known)unknown=true;else require(!unknown,"known size followed unknown group");if(previous.id&&previous.sizeKnown&&!previous.folder&&known)require(descending?previous.size>=item.size:previous.size<=item.size,"global size order changed");}
                    previous=item;}
                q.after=page.back();
            }
            require(seen.size()>=maximum,"native cursor omitted rows");
        }
        auto aliases=reader.file(0,42);require(aliases.size()==2,"all hardlink names not represented");
        desk::SearchQuery literal;literal.text=L"primary";
        auto indexed=reader.query(literal,100,[](const auto& v){return v.name=="primary.txt";},{"primary"});
        require(!reader.pending()&&indexed.size()==1,"literal candidate index omitted a matching alias");
        literal.text=L"missing-token";require(reader.query(literal,100,[](const auto&){return false;},{"missing-token"}).empty()&&!reader.pending(),"missing candidate token caused a whole-snapshot scan");
        std::vector<desk::CatalogEntry> update{{42,100,0,21,800,0,false,true,"renamed.txt"}};
        desk::appendCatalogChange(base,reader.generation(),0,42,update);
        desk::SearchQuery query;query.text=L"renamed";
        auto renamed=complete(reader,query,100,[](const desk::CatalogView& v){return v.foldedName.find("renamed")!=std::string_view::npos;});
        require(renamed.size()==1&&renamed[0].size==21&&renamed[0].modified==800,"live rename/metadata failed");
        require(reader.total()==count+3,"deleted alias not removed from count");
        auto directory=reader.file(0,100);directory[0].name="gamma";desk::appendCatalogChange(base,reader.generation(),0,100,directory);
        desk::SearchQuery path;path.sort=desk::SearchSort::Path;
        auto moved=complete(reader,path,100,[](const desk::CatalogView& v){return v.path().find("\\gamma\\renamed.txt")!=std::string::npos;});
        require(moved.size()==1,"directory rename did not update child paths");
        desk::appendCatalogChange(base,reader.generation(),0,42,{});
        require(complete(reader,query,100,[](const desk::CatalogView& v){return v.foldedName.find("renamed")!=std::string_view::npos;}).empty(),"deleted file remains searchable");
        require(reader.total()==count+2,"live deletion count failed");
        // A query's continuation must make progress while update blocks arrive.
        desk::SearchQuery absent;absent.text=L"not-present";size_t visited=0;
        auto slow=[&](const desk::CatalogView&){if(++visited%1000==0)Sleep(10);return false;};
        auto rows=reader.query(absent,100,slow);size_t slices=0;
        while(reader.pending()&&slices++<200){desk::appendCatalogChange(base,reader.generation(),0,90000000,{{90000000,101,0,slices,800+slices,0,false,true,"live.txt"}});rows=reader.query(absent,100,slow);}
        require(!reader.pending()&&rows.empty(),"updates continuously reset the native query");
        {std::ofstream tail(base/L"catalog.delta",std::ios::binary|std::ios::app);tail.write("partial",7);}
        require(complete(reader,path,10,all).size()==10,"partial trailing update invalidated committed data");
        reader.repairIncompleteLog();
        desk::appendCatalogChange(base,reader.generation(),0,99000000,{{99000000,101,0,1,1,0,false,true,"after-crash.txt"}});
        desk::SearchQuery crash;crash.text=L"after-crash";
        require(complete(reader,crash,10,[](const auto& v){return v.name=="after-crash.txt";}).size()==1,"new updates lost after repairing a partial crash tail");
        desk::SearchStatus status;status.roots=1;status.complete=true;status.stage=L"ready";status.total=reader.total();desk::catalogStatus(base,status,GetCurrentProcessId(),{root});
        std::atomic<bool> pollDone=false,pollFailed=false;
        std::thread polling([&]{try{desk::NativeCatalog client(base);for(int i=0;i<4000&&!pollDone;++i){auto s=client.status();if(s.roots!=1)pollFailed=true;}}catch(...){pollFailed=true;}});
        try{for(int i=0;i<500;++i){status.revision=i;desk::catalogStatus(base,status,GetCurrentProcessId(),{root});}}catch(...){pollDone=true;polling.join();throw;}
        pollDone=true;polling.join();require(!pollFailed,"status replacement failed while a reader held the file");
        // A published snapshot and its lookup stay mapped in another client.
        // Rebuilding/compacting must switch generations without replacing them.
        const auto oldSnapshot=reader.snapshotFile();const auto oldGeneration=reader.generation();
        desk::CatalogBuilder replacement(base,{root});reader.each([&](const auto& e){replacement.add(e);});
        replacement.add({99000001,101,0,2,2,0,false,true,"after-publication.txt"});
        replacement.finish([]{return false;},[](const auto&){});
        desk::SearchQuery published;published.text=L"after-publication";
        require(complete(reader,published,10,[](const auto& v){return v.name=="after-publication.txt";}).size()==1,"live mapped reader did not switch snapshot generations");
        require(reader.generation()!=oldGeneration&&fs::exists(oldSnapshot),"publication replaced a mapped snapshot");
        // Simulate log rotation between a reader's reload and its next poll.
        fs::rename(base/L"catalog.delta",base/L"catalog.delta.previous");
        desk::appendCatalogChange(base,reader.generation(),0,99000002,{{99000002,101,0,3,3,0,false,true,"after-rotation.txt"}});
        published.text=L"after-rotation";require(complete(reader,published,10,[](const auto& v){return v.name=="after-rotation.txt";}).size()==1,"rotated shorter log was ignored");
        for(int revision=0;revision<6;++revision)for(uint64_t i=0;i<1000;++i){
            desk::appendCatalogChange(base,reader.generation(),0,100000000+i,{{100000000+i,101,100000000+i,0,1,0,true,false,"temporary-"+std::to_string(i)}},false);
            desk::appendCatalogChange(base,reader.generation(),0,110000000+i,{{110000000+i,100000000+i,110000000+i,(uint64_t)revision,1,0,false,true,"nested.txt"}},false);
        }
        desk::flushCatalogChanges(base);desk::NativeCatalog restarted(base);require(restarted.available(),"restart snapshot unavailable");
        published.text=L"nested";require(complete(restarted,published,1000,[](const auto& v){return v.name=="nested.txt";}).size()==1000,"coalesced directory replay lost child paths");
        require(restarted.file(0,110000000)[0].size==5,"coalesced replay kept an intermediate update");
        std::cout<<"PASS native global sorting, full pagination, metadata groups, hardlinks, rename/delete, directory paths and live continuation\n";
        std::cout<<"rows="<<count+4<<" build_seconds="<<built<<" catalog_bytes="<<fs::file_size(reader.snapshotFile())<<"\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    std::error_code ec;if(base.parent_path()==fs::temp_directory_path()&&base.filename().wstring().starts_with(L"DeskFlow-native-test-"))fs::remove_all(base,ec);return result;
}
