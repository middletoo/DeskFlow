#include "../src/search.cpp"
#include <fstream>
#include <iostream>

static void require(bool value,const char* message) {
    if(!value) throw std::runtime_error(message);
}
static int64_t scalar(sqlite3* database,const char* sql) {
    desk::Statement query(database,sql);
    require(query.row(),"missing synthetic state");
    return sqlite3_column_int64(query.value,0);
}
int main() {
    namespace fs=std::filesystem;
    const auto base=fs::temp_directory_path()/(L"DeskFlow-coverage-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto root=base/L"files",data=base/L"directory-data",mft=base/L"mft-data";
    fs::create_directories(root);
    int result=0;
    try {
        for(int folder=0;folder<2;++folder) {
            const auto directory=root/(L"group-"+std::to_wstring(folder));
            fs::create_directories(directory);
            for(int file=0;file<1200;++file)
                std::ofstream(directory/(L"sample-"+std::to_wstring(file)+L".txt"))<<"synthetic";
        }
        int64_t job=0,stamp=0,queued=0;
        {
            desk::Indexer index(data);index.initialize({root.wstring()});
            index.scanBatch();
            job=scalar(index.database.value,"SELECT id FROM scan_jobs LIMIT 1");
            stamp=scalar(index.database.value,"SELECT stamp FROM scan_jobs LIMIT 1");
            queued=scalar(index.database.value,"SELECT count(*) FROM scan_queue");
            require(queued>0,"fixture should stop with unfinished queued directories");
        }
        {
            desk::Indexer index(data);index.initialize({root.wstring()});
            require(scalar(index.database.value,"SELECT id FROM scan_jobs LIMIT 1")==job,
                    "restart replaced the unfinished scan job");
            require(scalar(index.database.value,"SELECT stamp FROM scan_jobs LIMIT 1")==stamp,
                    "restart discarded the original reconciliation stamp");
            require(scalar(index.database.value,"SELECT count(*) FROM scan_queue")==queued,
                    "restart cleared or re-enqueued the durable directory queue");
            for(int batch=0;batch<1000 && index.scanBatch();++batch) {}
            require(scalar(index.database.value,"SELECT count(*) FROM scan_jobs")==0,
                    "resumed directory scan did not finish");
            require(scalar(index.database.value,"SELECT count(*) FROM files")==2402,
                    "resumed scan missed files or folders");
        }
        USN_JOURNAL_DATA_V0 journal{};
        journal.UsnJournalID=77;journal.FirstUsn=10;journal.NextUsn=200;
        {
            desk::Indexer index(mft);index.initialize({root.wstring()});
            auto& volume=index.roots.front();volume.rootFrn=5;volume.serial=123;
            index.beginMft(volume,journal);
            require(scalar(index.database.value,"SELECT count(*) FROM scan_jobs")==0,
                    "MFT switch retained a duplicate whole-drive directory scan");
            index.node(volume.id,7,5,L"cloud-metadata",true,volume.buildStamp,
                       FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT);
            index.node(volume.id,8,7,L"unreadable-attributes.txt",false,volume.buildStamp);
            volume.enumCursor=512;index.checkpointMft(volume);
        }
        {
            desk::Indexer index(mft);
            desk::Root volume;volume.id=1;volume.path=root.wstring();volume.rootFrn=5;volume.serial=123;
            require(index.restoreMft(volume,journal)&&volume.phase==desk::NtfsPhase::Enumerate&&volume.enumCursor==512,
                    "interrupted MFT enumeration did not resume its cursor");
            require(scalar(index.database.value,"SELECT count(*) FROM ntfs_nodes")==3,
                    "resuming MFT enumeration discarded committed nodes");
            auto path=index.nodePath(volume,8);
            require(path&&path->find(L"cloud-metadata")!=std::wstring::npos,
                    "physical MFT child was hidden under a reparse directory");
            index.materialize(volume,8,volume.buildStamp);
            require(scalar(index.database.value,"SELECT count(*) FROM files WHERE name='unreadable-attributes.txt' AND size_known=0")==1,
                    "unavailable attributes hid a known MFT filename");
            volume.phase=desk::NtfsPhase::Materialize;volume.materialFirst=false;volume.materialCursor=8;
            index.checkpointMft(volume);
        }
        {
            desk::Indexer index(mft);
            desk::Root volume;volume.id=1;volume.path=root.wstring();volume.rootFrn=5;volume.serial=123;
            require(index.restoreMft(volume,journal)&&volume.phase==desk::NtfsPhase::Materialize&&
                    !volume.materialFirst&&volume.materialCursor==8,
                    "interrupted MFT path materialization did not resume its cursor");
            auto changed=journal;changed.UsnJournalID++;
            require(!index.restoreMft(volume,changed),"replaced USN journal incorrectly reused an old checkpoint");
            changed=journal;changed.FirstUsn=201;changed.NextUsn=300;
            require(!index.restoreMft(volume,changed),"wrapped USN journal incorrectly reused an old build");
            volume.serial++;
            require(!index.restoreMft(volume,journal),"different volume incorrectly reused an old checkpoint");
        }
        {
            desk::Indexer index(base/L"own-data");index.initialize({base.wstring()});
            auto& volume=index.roots.front();volume.rootFrn=5;volume.serial=321;
            index.beginMft(volume,journal);
            index.node(volume.id,9,5,L"own-data",true,volume.buildStamp,FILE_ATTRIBUTE_DIRECTORY);
            index.node(volume.id,10,9,L"files.db",false,volume.buildStamp);
            const auto writes=sqlite3_total_changes64(index.database.value);
            require(!index.journalChange(volume,10,9,201,USN_REASON_DATA_OVERWRITE,FILE_ATTRIBUTE_NORMAL,L"files.db"),
                    "own database journal records caused another durable checkpoint");
            require(sqlite3_total_changes64(index.database.value)==writes,
                    "own database notification changed index data");
            desk::meta(index.database.value,"test_stable_status",L"same");
            const auto before=sqlite3_total_changes64(index.database.value);
            desk::meta(index.database.value,"test_stable_status",L"same");
            require(sqlite3_total_changes64(index.database.value)==before,
                    "unchanged status caused background database writes");
            require(index.journalChange(volume,11,5,202,USN_REASON_FILE_CREATE,FILE_ATTRIBUTE_NORMAL,L"external-file.txt"),
                    "external file changes were suppressed with runtime files");
            index.node(volume.id,12,5,L"new-name.txt",false,volume.buildStamp);
            index.materialize(volume,12,volume.buildStamp);
            desk::upsert(index.database.value,volume.id,(base/L"old-name.txt").wstring(),L"old-name.txt",
                         FILE_ATTRIBUTE_NORMAL,0,volume.buildStamp,12,false);
            index.journalChange(volume,12,5,203,USN_REASON_RENAME_OLD_NAME,FILE_ATTRIBUTE_NORMAL,L"old-name.txt");
            require(scalar(index.database.value,"SELECT count(*) FROM files WHERE name='new-name.txt'")==1&&
                    scalar(index.database.value,"SELECT count(*) FROM files WHERE name='old-name.txt'")==0,
                    "journal replay erased the latest MFT name or retained the old path");
        }
        std::cout<<"PASS durable directory/MFT resume, physical reparse children, metadata-denied names and USN runtime exclusion\n";
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';result=1;}
    const auto checked=fs::weakly_canonical(base);
    if(checked.parent_path()==fs::weakly_canonical(fs::temp_directory_path()) &&
       checked.filename().wstring().starts_with(L"DeskFlow-coverage-")) {
        std::error_code cleanup;fs::remove_all(checked,cleanup);
    } else result=1;
    return result;
}
