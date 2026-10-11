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
            auto keywordRoot=base/L"keyword-fixture";fs::create_directories(keywordRoot/L"late-parent");
            std::ofstream(keywordRoot/L"late-parent"/L"DeskFlow.exe")<<"synthetic executable fixture";
            auto keywordData=base/L"keyword-data";
            desk::Indexer index(keywordData);index.initialize({keywordRoot.wstring()});
            auto& volume=index.roots.front();volume.rootFrn=5;volume.serial=123;index.beginMft(volume,journal);
            index.node(volume.id,100,300,L"DeskFlow.exe",false,volume.buildStamp);
            require(!index.nodePath(volume,100),"a child with a missing parent cannot be assigned a fabricated path");
            index.node(volume.id,300,5,L"late-parent",true,volume.buildStamp);
            index.node(volume.id,200,300,L"KnownDenied.exe",false,volume.buildStamp);
            volume.phase=desk::NtfsPhase::Materialize;
            for(int batch=0;batch<8&&volume.phase==desk::NtfsPhase::Materialize;++batch)index.ntfsBatch();
            index.publish();
            desk::SearchStore reader(keywordData);
            auto hits=reader.query(L"deskflow");
            require(hits.size()==1&&hits.front().path==(keywordRoot/L"late-parent"/L"DeskFlow.exe").wstring(),
                    "complete MFT materialization missed a filename whose parent arrived later");
            require(!reader.status().complete,"MFT paths alone must not claim directory/hardlink coverage is complete");
            require(!hits.front().sizeKnown,"names-first indexing should disclose pending metadata");
            for(int batch=0;batch<20&&index.scanBatch();++batch){}
            index.publish();require(!reader.status().complete,"journal changes must catch up before declaring complete coverage");
            volume.journalCaughtUp=true;index.publish();hits=reader.query(L"deskflow");
            require(reader.status().complete&&hits.size()==1&&hits.front().sizeKnown&&hits.front().size>0&&hits.front().modified>0,
                    "directory supplement must finish coverage and fill file metadata");
            require(reader.query(L"KnownDenied").size()==1,"directory supplement removed a primary name confirmed by the native snapshot");
            index.journalChange(volume,200,300,201,USN_REASON_FILE_DELETE,FILE_ATTRIBUTE_NORMAL,L"KnownDenied.exe");
            require(reader.query(L"KnownDenied").empty(),"journal deletion must remove a retained native primary name");
            volume.clearCache();index.materialize(volume,100,volume.buildStamp,false);
            hits=reader.query(L"deskflow");
            require(hits.front().sizeKnown&&hits.front().size>0&&hits.front().modified>0,
                    "resumed names-first work must retain already known metadata");
        }
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
            // Use the physical volume spelling: redirected Temp/AppData may
            // be reached through a different drive, unlike a real MFT root.
            const auto liveRoot=fs::path(desk::finalDirectory(base.wstring()))/L"live-build";fs::create_directories(liveRoot);
            std::ofstream(liveRoot/L"newest.txt")<<"synthetic new file";
            desk::Indexer index(base/L"live-build-data");index.initialize({liveRoot.wstring()});
            auto& volume=index.roots.front();volume.rootFrn=5;volume.serial=123;index.beginMft(volume,journal);
            const auto snapshot=volume.buildStamp;
            index.node(volume.id,100,5,L"deleted.txt",false,snapshot);
            index.materialize(volume,100,snapshot,false);
            volume.phase=desk::NtfsPhase::Materialize;volume.materialFirst=false;volume.materialCursor=1000;
            // The new ID is behind the durable build cursor; only live journal
            // processing can make it visible before initial construction ends.
            index.journalChange(volume,20,5,201,USN_REASON_FILE_CREATE,FILE_ATTRIBUTE_NORMAL,L"newest.txt");
            desk::SearchStore reader(base/L"live-build-data");
            desk::SearchQuery recent;recent.sort=desk::SearchSort::Modified;recent.descending=true;
            while(desk::search_detail::prepareSortIndexes(index.database.value,512)){}
            auto hits=reader.query(recent);
            require(!hits.empty()&&hits.front().name==L"newest.txt"&&hits.front().modified>0,
                    "new file did not become searchable with current modification time during the initial build");
            index.node(volume.id,20,5,L"stale-enumerated-name.txt",false,snapshot);
            volume.clearCache();
            require(reader.query(L"newest").size()==1&&index.nodePath(volume,20)==(liveRoot/L"newest.txt").wstring(),
                    "older enumeration overwrote a live journal name");
            const auto oldRevision=reader.status().revision;
            index.journalChange(volume,100,5,202,USN_REASON_FILE_DELETE,FILE_ATTRIBUTE_NORMAL,L"deleted.txt");
            require(reader.query(L"deleted").empty()&&reader.status().revision>oldRevision,
                    "live deletion was not committed or did not notify readers");
            index.materialize(volume,100,snapshot,false);
            require(reader.query(L"deleted").empty(),"materialization resurrected a deleted node");
            volume.nextUsn=250;index.checkpointMft(volume);
            auto resumedJournal=journal;resumedJournal.FirstUsn=210;resumedJournal.NextUsn=300;
            desk::Root restored;restored.id=volume.id;restored.path=liveRoot.wstring();restored.rootFrn=5;restored.serial=123;
            require(index.restoreMft(restored,resumedJournal)&&restored.nextUsn==250&&restored.materialCursor==1000,
                    "concurrent journal checkpoint did not preserve resumable build after old journal pages expired");
            fs::create_directories(liveRoot/L"late-parent");
            std::ofstream(liveRoot/L"late-parent"/L"arrived.txt")<<"synthetic late-parent event";
            volume.phase=desk::NtfsPhase::Enumerate;index.checkpointMft(volume);
            index.journalChange(volume,21,300,251,USN_REASON_FILE_CREATE,FILE_ATTRIBUTE_NORMAL,L"arrived.txt");
            require(!index.refreshBatch()&&scalar(index.database.value,"SELECT count(*) FROM ntfs_refresh")>0,
                    "a change with an unenumerated parent lost its durable retry");
            index.node(volume.id,300,5,L"late-parent",true,snapshot);
            volume.phase=desk::NtfsPhase::Materialize;index.checkpointMft(volume);index.refreshBatch();
            require(reader.query(L"arrived").size()==1,
                    "a new file behind the build cursor was missed when its parent arrived later");
        }
        {
            const auto physical=desk::finalDirectory(base.wstring());require(!physical.empty(),"missing physical fixture root");
            const auto linkRoot=fs::path(physical)/L"hardlink-events";fs::create_directories(linkRoot);
            std::ofstream(linkRoot/L"primary.txt")<<"synthetic hardlink fixture";
            auto identity=[](const fs::path& path){
                desk::Handle h(CreateFileW(desk::extended(path.wstring()).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
                BY_HANDLE_FILE_INFORMATION info{};require(h&&GetFileInformationByHandle(h.value,&info),"cannot read synthetic file identity");
                return (uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
            };
            desk::Indexer index(base/L"hardlink-data");index.initialize({linkRoot.wstring()});
            auto& volume=index.roots.front();volume.rootFrn=identity(linkRoot);volume.serial=123;index.beginMft(volume,journal);volume.phase=desk::NtfsPhase::Journal;
            const auto file=identity(linkRoot/L"primary.txt");
            require(CreateHardLinkW((linkRoot/L"alias.txt").c_str(),(linkRoot/L"primary.txt").c_str(),nullptr)!=FALSE,"cannot create synthetic hardlink");
            index.journalChange(volume,file,volume.rootFrn,201,USN_REASON_HARD_LINK_CHANGE,FILE_ATTRIBUTE_NORMAL,L"primary.txt");
            for(int batch=0;batch<20&&index.scanBatch();++batch){}
            desk::SearchStore reader(base/L"hardlink-data");
            require(reader.query(L"alias").size()==1,"live hardlink creation was not indexed by enumeration or fallback");
            fs::remove(linkRoot/L"alias.txt");
            index.journalChange(volume,file,volume.rootFrn,202,USN_REASON_HARD_LINK_CHANGE,FILE_ATTRIBUTE_NORMAL,L"primary.txt");
            require(reader.query(L"alias").empty()&&reader.query(L"primary").size()==1,"removed alias remained indexed or removed a surviving hardlink");
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
