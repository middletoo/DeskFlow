#include "../src/search.cpp"
#include <iostream>
#include <fstream>

// A synthetic initial-MFT workload, including every production FTS trigger and
// sort index. It never enumerates the user's volumes or changes their database.
int wmain(int argc,wchar_t** argv) {
    if(argc==3&&std::wstring(argv[1])==L"--resume-copy"){
        const std::filesystem::path data=argv[2];
        if(!std::filesystem::exists(data/L".deskflow-index-benchmark")){std::cerr<<"Requires an isolated marked benchmark copy\n";return 1;}
        try{
            desk::Indexer index(data);
            desk::Statement roots(index.database.value,"SELECT id,path,build_stamp,material_cursor FROM search_roots WHERE build_phase='materialize'");
            while(roots.row()){
                desk::Root root;root.id=sqlite3_column_int64(roots.value,0);root.path=desk::wide(sqlite3_column_text(roots.value,1));root.buildStamp=sqlite3_column_int64(roots.value,2);root.materialCursor=sqlite3_column_int64(roots.value,3);root.materialFirst=false;root.phase=desk::NtfsPhase::Materialize;
                desk::Statement self(index.database.value,"SELECT frn FROM ntfs_nodes WHERE root_id=?1 AND frn=parent_frn AND name='' LIMIT 1");self.number(1,root.id);if(!self.row())throw std::runtime_error("Missing root identity");root.rootFrn=sqlite3_column_int64(self.value,0);index.roots.push_back(std::move(root));
            }
            index.buildResources(true);desk::Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));
            desk::WriterPacing pacing(index.database.value,stop.value,nullptr,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));pacing.initialBuild=true;
            const auto start=desk::Clock::now();const auto cpu=desk::WriterPacing::threadCpu();
            IO_COUNTERS before{},after{};GetProcessIoCounters(GetCurrentProcess(),&before);size_t rows=0;
            for(int i=0;i<66;++i){index.ntfsBatch();rows=0;for(const auto& root:index.roots)rows+=root.materialDone;}
            GetProcessIoCounters(GetCurrentProcess(),&after);double elapsed=std::chrono::duration<double>(desk::Clock::now()-start).count();
            std::cout<<"rows="<<rows<<" seconds="<<elapsed<<" rows_per_second="<<rows/elapsed<<" cpu_seconds="<<(desk::WriterPacing::threadCpu()-cpu)/1e7<<" write_bytes="<<after.WriteTransferCount-before.WriteTransferCount<<"\n";return 0;
        }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
    }
    const size_t count=argc>1?std::stoull(argv[1]):20000;
    const bool paced=argc>2&&std::wstring(argv[2])==L"--paced";
    auto base=std::filesystem::temp_directory_path()/(L"DeskFlow-build-probe-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try {
        {
            desk::Indexer index(base/L"index");
            std::filesystem::create_directories(base/L"files");index.initialize({(base/L"files").wstring()});
            auto& root=index.roots.front();root.rootFrn=5;root.serial=123;
            USN_JOURNAL_DATA_V0 journal{};journal.UsnJournalID=77;journal.FirstUsn=10;journal.NextUsn=200;
            index.beginMft(root,journal);
            {desk::Transaction tx(index.database.value);
                for(size_t i=0;i<400;++i) index.node(root.id,100+i,5,L"synthetic-中文-project-directory-"+std::to_wstring(i),true,root.buildStamp);
                for(size_t i=0;i<count;++i) index.node(root.id,10000+i,100+((i*7919)%400),L"finance-中文-report-"+std::to_wstring(i)+L".pdf",false,root.buildStamp);
                tx.commit();}
            root.phase=desk::NtfsPhase::Materialize;
            index.buildResources(true);
            while(desk::search_detail::prepareSortIndexes(index.database.value,1024)){}
            desk::Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));
            std::unique_ptr<desk::WriterPacing> pacing;
            if(paced)pacing=std::make_unique<desk::WriterPacing>(index.database.value,stop.value,nullptr,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            if(pacing)pacing->initialBuild=true;
            const auto began=desk::Clock::now();const auto cpu=desk::WriterPacing::threadCpu();
            while(root.phase==desk::NtfsPhase::Materialize)index.ntfsBatch();
            const auto elapsed=std::chrono::duration<double>(desk::Clock::now()-began).count();
            std::cout<<"rows="<<count+400<<" seconds="<<elapsed<<" rows_per_second="<<(count+400)/elapsed<<" cpu_seconds="<<(desk::WriterPacing::threadCpu()-cpu)/1e7<<" processors="<<GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)<<" paced="<<paced<<"\n";
            desk::Statement verify(index.database.value,"SELECT count(*) FROM files");verify.row();
            if(sqlite3_column_int64(verify.value,0)!=count+400)throw std::runtime_error("materialization lost objects");
        }
        std::filesystem::remove_all(base);return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<"\n";std::filesystem::remove_all(base);return 1;}
}
