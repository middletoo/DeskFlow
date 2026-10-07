#include "search.hpp"
#include "sqlite3.h"
#include <winioctl.h>
#include <functional>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <algorithm>

namespace fs = std::filesystem;
namespace desk::search_detail {
using UsnVisitor = std::function<void(uint64_t,uint64_t,int64_t,DWORD,DWORD,const std::wstring&)>;
bool visitUsnRecords(const void*,size_t,const UsnVisitor&);
std::string shortQueryToken(const std::wstring&);
void registerShortGrams(sqlite3*);
bool backfillShortGrams(sqlite3*,size_t);
bool prepareSortIndexes(sqlite3*,size_t);
}

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void put(const fs::path& path, const char* contents = "fixture") {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << contents;
}

static bool junction(const fs::path& link,const fs::path& target) {
    if(!CreateDirectoryW(link.c_str(),nullptr)) return false;
    HANDLE directory=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(directory==INVALID_HANDLE_VALUE) {RemoveDirectoryW(link.c_str());return false;}
    struct MountPoint {
        DWORD tag;WORD length,reserved,substituteOffset,substituteLength,printOffset,printLength;
        wchar_t names[1];
    };
    auto substitute=L"\\??\\"+target.wstring();auto print=target.wstring();
    auto nameBytes=(substitute.size()+print.size()+2)*sizeof(wchar_t);
    std::vector<unsigned char> storage(offsetof(MountPoint,names)+nameBytes);
    auto* point=reinterpret_cast<MountPoint*>(storage.data());
    point->tag=IO_REPARSE_TAG_MOUNT_POINT;point->length=static_cast<WORD>(storage.size()-8);
    point->substituteLength=static_cast<WORD>(substitute.size()*sizeof(wchar_t));
    point->printOffset=static_cast<WORD>((substitute.size()+1)*sizeof(wchar_t));point->printLength=static_cast<WORD>(print.size()*sizeof(wchar_t));
    memcpy(point->names,substitute.c_str(),(substitute.size()+1)*sizeof(wchar_t));
    memcpy(reinterpret_cast<unsigned char*>(point->names)+point->printOffset,print.c_str(),(print.size()+1)*sizeof(wchar_t));
    DWORD returned=0;bool okay=DeviceIoControl(directory,FSCTL_SET_REPARSE_POINT,storage.data(),static_cast<DWORD>(storage.size()),nullptr,0,&returned,nullptr)!=FALSE;
    CloseHandle(directory);if(!okay) RemoveDirectoryW(link.c_str());return okay;
}

static std::wstring executable() {
    std::wstring path(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
}

struct Child {
    PROCESS_INFORMATION process{};
    Child(const std::wstring& arguments) {
        auto command = L"\"" + executable() + L"\" " + arguments;
        STARTUPINFOW startup{sizeof(startup)};
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
                "cannot launch index test worker");
        CloseHandle(process.hThread);
        process.hThread = nullptr;
    }
    ~Child() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 0);
                WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hProcess);
        }
    }
    void stopAbruptly() {
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
    }
};

template<class Check> static bool eventually(Check check, int seconds = 15) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    do {
        if (check()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    } while (std::chrono::steady_clock::now() < end);
    return check();
}

static int benchmark(size_t count,const fs::path& existing={}) {
    wchar_t temporary[MAX_PATH]{};GetTempPathW(MAX_PATH,temporary);
    auto data=existing.empty() ? fs::path(temporary)/(L"desk-search-benchmark-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())) : existing;
    std::cout<<"benchmark_fixture="<<data.string()<<"\n"<<std::flush;
    desk::SearchStore store(data);
    sqlite3* db=nullptr;
    require(sqlite3_open((data/L"files.db").string().c_str(),&db)==SQLITE_OK,"benchmark database open failed");
    desk::search_detail::registerShortGrams(db);
    require(sqlite3_exec(db,"PRAGMA cache_size=-4096; PRAGMA mmap_size=0; PRAGMA temp_store=FILE; INSERT OR IGNORE INTO search_roots(id,path,path_key,complete) VALUES(1,'c:\\fixture','c:\\fixture',1); BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"benchmark setup failed");
    sqlite3_stmt* insert=nullptr;
    require(sqlite3_prepare_v2(db,"INSERT INTO files(root_id,path,name,path_fold,name_fold,folder,size,seen) VALUES(1,?1,?2,?1,?2,0,123,1)",-1,&insert,nullptr)==SQLITE_OK,"benchmark insert prepare failed");
    auto started=std::chrono::steady_clock::now();
    for(size_t i=0;existing.empty() && i<count;++i) {
        auto name=(i%10000==0 ? std::string("needle-finance-") : std::string("report-"))+std::to_string(10000000+i)+".txt";
        if(i+1==count) name="末尾中文报告-"+std::to_string(i)+".txt";
        auto path="c:\\fixture\\folder\\"+name;
        sqlite3_bind_text(insert,1,path.c_str(),static_cast<int>(path.size()),SQLITE_TRANSIENT);
        sqlite3_bind_text(insert,2,name.c_str(),static_cast<int>(name.size()),SQLITE_TRANSIENT);
        require(sqlite3_step(insert)==SQLITE_DONE,"benchmark real SQL insertion failed");
        sqlite3_reset(insert);sqlite3_clear_bindings(insert);
        if((i+1)%5000==0) require(sqlite3_exec(db,"COMMIT; BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"benchmark batch commit failed");
        if((i+1)%100000==0) std::cout<<"seeded "<<(i+1)<<" / "<<count<<"\n"<<std::flush;
    }
    sqlite3_finalize(insert);sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr);
    std::cout<<"seed_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<<"\n";
    auto gramsStart=std::chrono::steady_clock::now();
    while(desk::search_detail::backfillShortGrams(db,1024)) {}
    std::cout<<"short_grams_backfill_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-gramsStart).count()<<"\n";
    sqlite3_stmt* plan=nullptr;
    sqlite3_prepare_v2(db,"EXPLAIN QUERY PLAN SELECT f.id FROM files f JOIN files_fts ON files_fts.rowid=f.id WHERE files_fts MATCH 'report' ORDER BY files_fts.rowid DESC LIMIT 100",-1,&plan,nullptr);
    while(sqlite3_step(plan)==SQLITE_ROW) std::cout<<"plan="<<sqlite3_column_text(plan,3)<<"\n";
    sqlite3_finalize(plan);sqlite3_close(db);
    for(const auto& query:{std::wstring(L"report"),std::wstring(L"needle-finance"),std::wstring(L"中文报告")}) {
        std::vector<double> times;
        for(int sample=0;sample<40;++sample) {
            auto start=std::chrono::steady_clock::now();auto result=store.query(query);
            double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();times.push_back(elapsed);
            require(result.size()==(query==L"中文报告" ? 1 : std::min<size_t>(100,query==L"needle-finance" ? (count+9999)/10000 : count-((count+9999)/10000)-1)),"large fixture query did not return all requested first-page results");
        }
        std::sort(times.begin(),times.end());
        std::cout<<"query_unicode_chars="<<query.size()<<" p50_ms="<<times[20]<<" p95_ms="<<times[38]<<" max_ms="<<times.back()<<"\n";
    }
    for(const auto& query:{std::wstring(L"中"),std::wstring(L"中文"),std::wstring(L"r")}) {
        std::vector<double> times;
        for(int sample=0;sample<40;++sample) {
            auto start=std::chrono::steady_clock::now();auto hits=store.query(query);
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            require(hits.size()==(query==L"r" ? 100 : 1) && !store.queryPending(),"short filename gram query was incomplete");
        }
        std::sort(times.begin(),times.end());
        std::cout<<"short_gram_unicode_chars="<<query.size()<<" p50_ms="<<times[20]<<" p95_ms="<<times[38]<<" max_ms="<<times.back()<<"\n";
    }
    auto deepStart=std::chrono::steady_clock::now();auto deepPage=store.query(L"report",100,1000);
    require(deepPage.size()==100 && deepPage.front().id<1000 && deepPage.back().id<deepPage.front().id,"million-row deep cursor page failed");
    std::cout<<"deep_cursor_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-deepStart).count()<<"\n";
    auto shortStart=std::chrono::steady_clock::now();
    auto shortHits=store.query(L"中文");int batches=1;
    std::cout<<"short_chinese_first_hits="<<shortHits.size()<<" first_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-shortStart).count()<<"\n";
    while(store.queryPending() && std::chrono::steady_clock::now()-shortStart<std::chrono::seconds(5)) {shortHits=store.query(L"中文");++batches;}
    require(shortHits.size()==1 && !store.queryPending(),"bounded short Chinese scan did not eventually find the last millionth row");
    std::cout<<"short_chinese_hits="<<shortHits.size()<<" batches="<<batches<<" completion_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-shortStart).count()<<"\n";
    auto regexStart=std::chrono::steady_clock::now();auto regexHits=store.query(L"regex:^末尾中文报告.*txt$");int regexBatches=1;
    while(store.queryPending() && std::chrono::steady_clock::now()-regexStart<std::chrono::seconds(15)) {regexHits=store.query(L"regex:^末尾中文报告.*txt$");++regexBatches;}
    require(regexHits.size()==1 && !store.queryPending(),"regular expression continuation did not find the last millionth row");
    std::cout<<"regex_hits="<<regexHits.size()<<" batches="<<regexBatches<<" completion_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-regexStart).count()<<"\n";
    std::cout<<"fixture_rows="<<count<<" db_bytes="<<fs::file_size(data/L"files.db")<<"\n";
    std::cout<<"benchmark_fixture="<<data.string()<<"\n";
    return 0;
}

static std::string fixtureUtf8(const std::wstring& text) {
    int length=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(length,'\0');WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),length,nullptr,nullptr);return result;
}
static std::wstring lowerAscii(std::wstring text) {for(auto& ch:text) if(ch>=L'A' && ch<=L'Z') ch+=L'a'-L'A';return text;}
static std::wstring fixtureType(const desk::SearchItem& item) {
    if(item.folder) return L"0folder";
    auto name=lowerAscii(item.name);auto dot=name.rfind(L'.');return L"1:"+(dot==std::wstring::npos || dot==0 ? L"" : name.substr(dot+1));
}
static std::vector<desk::SearchItem> completeSorted(desk::SearchStore& store,const desk::SearchQuery& spec,size_t limit=100) {
    auto result=store.query(spec,limit);
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(store.queryPending() && std::chrono::steady_clock::now()<end) result=store.query(spec,limit);
    require(!store.queryPending(),"sorted query did not complete within bounded continuation");return result;
}
static void sortedSearchTests(const fs::path& data) {
    desk::SearchStore store(data);sqlite3* db=nullptr;
    require(sqlite3_open((data/L"files.db").string().c_str(),&db)==SQLITE_OK,"cannot seed global sort fixture");
    desk::search_detail::registerShortGrams(db);
    require(sqlite3_exec(db,"INSERT INTO search_roots(id,path,path_key,complete) VALUES(1,'c:\\fixture','c:\\fixture',1); BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"sort fixture setup failed");
    sqlite3_stmt* statement=nullptr;
    require(sqlite3_prepare_v2(db,"INSERT INTO files(root_id,path,path_fold,name,name_fold,folder,size,seen) VALUES(1,?1,?2,?3,?4,0,?5,1)",-1,&statement,nullptr)==SQLITE_OK,"sort fixture statement failed");
    std::vector<desk::SearchItem> rows;
    constexpr int total=2307;
    for(int i=0;i<total;++i) {
        auto number=std::to_wstring((i*73)%total);number=std::wstring(4-number.size(),L'0')+number;
        auto name=std::wstring(i%2 ? L"report-" : L"Report-")+number+(i%3==0 ? L".txt" : i%3==1 ? L".png" : L".mp3");
        desk::SearchItem item;item.id=i+1;item.name=name;item.path=L"c:\\fixture\\g"+std::to_wstring(i%11)+L"\\"+name;item.size=(i*13)%997;
        auto bind=[&](int index,const std::wstring& value) {auto bytes=fixtureUtf8(value);sqlite3_bind_text(statement,index,bytes.data(),static_cast<int>(bytes.size()),SQLITE_TRANSIENT);};
        bind(1,item.path);bind(2,lowerAscii(item.path));bind(3,name);bind(4,lowerAscii(name));sqlite3_bind_int64(statement,5,item.size);
        require(sqlite3_step(statement)==SQLITE_DONE,"global sort fixture insertion failed");sqlite3_reset(statement);sqlite3_clear_bindings(statement);rows.push_back(std::move(item));
    }
    sqlite3_finalize(statement);
    require(sqlite3_prepare_v2(db,"INSERT INTO files(root_id,path,path_fold,name,name_fold,folder,size,seen,size_known) VALUES(1,?1,?2,?3,?4,?5,?6,1,?7)",-1,&statement,nullptr)==SQLITE_OK,"cannot seed unknown/folder cursor cases");
    for(int extra=0;extra<3;++extra) {
        desk::SearchItem item;item.id=total+extra+1;item.name=extra==0 ? L"Report-unknown.txt" : L"Report-shared";item.path=L"c:\\fixture\\extra"+std::to_wstring(extra)+L"\\"+item.name;item.folder=extra!=0;item.size=777;item.sizeKnown=false;
        auto bind=[&](int index,const std::wstring& value) {auto bytes=fixtureUtf8(value);sqlite3_bind_text(statement,index,bytes.data(),static_cast<int>(bytes.size()),SQLITE_TRANSIENT);};
        bind(1,item.path);bind(2,lowerAscii(item.path));bind(3,item.name);bind(4,lowerAscii(item.name));sqlite3_bind_int(statement,5,item.folder);sqlite3_bind_int64(statement,6,item.size);sqlite3_bind_int(statement,7,0);
        require(sqlite3_step(statement)==SQLITE_DONE,"unknown-size/folder fixture insertion failed");sqlite3_reset(statement);sqlite3_clear_bindings(statement);rows.push_back(item);
    }
    sqlite3_finalize(statement);require(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,"sort fixture commit failed");
    while(desk::search_detail::backfillShortGrams(db,1024)) {}
    while(desk::search_detail::prepareSortIndexes(db,1024)) {}
    sqlite3_close(db);
    for(auto sort:{desk::SearchSort::Name,desk::SearchSort::Path,desk::SearchSort::Size,desk::SearchSort::Type,desk::SearchSort::Id}) for(bool descending:{false,true}) {
        auto expected=rows;
        std::sort(expected.begin(),expected.end(),[&](const auto& a,const auto& b) {
            int relation=0;
            if(sort==desk::SearchSort::Size) {
                bool knownA=a.sizeKnown && !a.folder,knownB=b.sizeKnown && !b.folder;
                if(knownA!=knownB) return knownA;
                if(knownA) relation=a.size<b.size ? -1 : a.size>b.size ? 1 : 0;
            }
            else if(sort!=desk::SearchSort::Id) {
                auto ka=sort==desk::SearchSort::Name ? lowerAscii(a.name) : sort==desk::SearchSort::Path ? lowerAscii(a.path) : fixtureType(a);
                auto kb=sort==desk::SearchSort::Name ? lowerAscii(b.name) : sort==desk::SearchSort::Path ? lowerAscii(b.path) : fixtureType(b);
                relation=ka<kb ? -1 : ka>kb ? 1 : 0;
            }
            if(!relation) relation=a.id<b.id ? -1 : a.id>b.id ? 1 : 0;
            return descending ? relation>0 : relation<0;
        });
        desk::SearchQuery spec;spec.text=L"report";spec.sort=sort;spec.descending=descending;
        size_t offset=0;
        for(int page=0;page<30;++page) {
            auto actual=completeSorted(store,spec,113);
            for(const auto& item:actual) {
                require(offset<expected.size() && item.id==expected[offset].id,"full-set order/cursor differs from actual complete matching set");
                require(item.sizeKnown==(expected[offset].sizeKnown && !expected[offset].folder),"unknown metadata was displayed as a real size");++offset;
            }
            if(actual.empty()) break;
            spec.after=actual.back();
        }
        require(offset==rows.size(),"global sort pagination omitted or duplicated matches beyond page one");
    }
    desk::SearchQuery spec;spec.text=L"Report";spec.matchCase=true;
    require(completeSorted(store,spec,1000).size()==1000,"case-sensitive query returned wrong first page");
    spec.text=L"REPORT";require(completeSorted(store,spec).empty(),"case matching ignored the original filename");
    spec.text=L"fixture";spec.matchCase=false;require(completeSorted(store,spec).empty(),"default filename matching searched paths");
    spec.matchPath=true;require(completeSorted(store,spec).size()==100,"match-path switch did not search complete paths");
    spec.matchPath=false;spec.text=L"regex:^Report-.*";spec.matchCase=true;
    auto upper=completeSorted(store,spec,1000);require(upper.size()==1000 && upper.front().name.starts_with(L"Report-"),"regex case flag failed");
    spec.text=L"ext:TXT;png";spec.matchCase=false;require(completeSorted(store,spec).size()==100,"extension list filter failed");
    spec.text=L"pic:";require(completeSorted(store,spec).size()==100,"common image filter failed");
    spec.text=L"audio:";require(completeSorted(store,spec).size()==100,"common audio filter failed");
    spec.text=L"video:";require(completeSorted(store,spec).empty(),"video filter included unrelated extensions");
    spec.text=L"folder:";auto folders=completeSorted(store,spec,1);require(folders.size()==1 && folders[0].folder,"folder filter failed");spec.after=folders[0];auto nextFolder=completeSorted(store,spec,1);require(nextFolder.size()==1 && nextFolder[0].id!=folders[0].id,"duplicate-name folder cursor failed");
    spec.after.reset();spec.text=L"Report !unknown !shared";require(completeSorted(store,spec).size()==100,"negative terms failed");
    spec.text=L"unknown|shared";require(completeSorted(store,spec).size()==3,"simple OR groups failed");
    spec.text=L"report-000?*";require(!completeSorted(store,spec).empty(),"wildcards with global sorting failed");
    spec.matchPath=true;spec.text=L"regex:fixture";require(completeSorted(store,spec).size()==100,"regex path switch failed");
    spec.matchPath=false;require(completeSorted(store,spec).empty(),"regex path scope leaked into filename scope");
    require(sqlite3_open((data/L"files.db").string().c_str(),&db)==SQLITE_OK,"open held writer fixture");
    require(sqlite3_exec(db,"BEGIN IMMEDIATE;UPDATE search_meta SET value=value WHERE key='total'",nullptr,nullptr,nullptr)==SQLITE_OK,"hold index writer transaction");
    auto readerStart=std::chrono::steady_clock::now();
    {desk::SearchStore concurrentReader(data);desk::SearchQuery ready;ready.text=L"report";
        require(completeSorted(concurrentReader,ready).size()==100,"existing index reader must remain usable during writer transaction");}
    require(std::chrono::steady_clock::now()-readerStart<std::chrono::milliseconds(500),"read-only startup must not wait for index schema writes");
    sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(db);
}

static int sortedBenchmark(const fs::path& data) {
    desk::SearchStore store(data);sqlite3* db=nullptr;
    require(sqlite3_open((data/L"files.db").string().c_str(),&db)==SQLITE_OK,"cannot open sorted benchmark");desk::search_detail::registerShortGrams(db);
    sqlite3_stmt* verify=nullptr;
    require(sqlite3_prepare_v2(db,"SELECT count(*),sum(path NOT LIKE 'c:\\fixture\\%') FROM files",-1,&verify,nullptr)==SQLITE_OK,"cannot validate synthetic benchmark");
    require(sqlite3_step(verify)==SQLITE_ROW && sqlite3_column_int64(verify,0)==1000000 && sqlite3_column_int64(verify,1)==0,"sorted benchmark requires the dedicated one-million-row synthetic fixture");sqlite3_finalize(verify);
    sqlite3_exec(db,"PRAGMA cache_size=-4096; PRAGMA mmap_size=0; PRAGMA temp_store=FILE",nullptr,nullptr,nullptr);
    auto preparation=std::chrono::steady_clock::now();
    require(sqlite3_exec(db,"UPDATE files SET size_known=1 WHERE folder=0 AND size_known=0",nullptr,nullptr,nullptr)==SQLITE_OK,"cannot mark declared synthetic sizes");
    int work=0;while(desk::search_detail::prepareSortIndexes(db,1024)) {if(++work%100==0) std::cout<<"sort_prepare_batches="<<work<<"\n"<<std::flush;}
    std::cout<<"sort_prepare_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-preparation).count()<<"\n";
    for(auto sort:{desk::SearchSort::Name,desk::SearchSort::Path,desk::SearchSort::Size,desk::SearchSort::Type}) for(bool down:{false,true}) {
        std::vector<double> times;
        desk::SearchQuery spec;spec.text=L"report";spec.sort=sort;spec.descending=down;
        for(int sample=0;sample<40;++sample) {
            auto start=std::chrono::steady_clock::now();auto hits=completeSorted(store,spec);
            require(hits.size()==100,"common globally sorted query lost a first page");
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        }
        std::sort(times.begin(),times.end());std::cout<<"sort="<<static_cast<int>(sort)<<" descending="<<down<<" p50_ms="<<times[20]<<" p95_ms="<<times[38]<<" max_ms="<<times.back()<<"\n";
        auto first=completeSorted(store,spec);spec.after=first.back();auto next=completeSorted(store,spec);
        require(next.size()==100 && std::none_of(next.begin(),next.end(),[&](const auto& item){return std::any_of(first.begin(),first.end(),[&](const auto& prior){return prior.id==item.id;});}),"million-row globally sorted cursor repeated page one");
    }
    desk::SearchQuery rare;rare.text=L"needle-finance";
    auto rareStart=std::chrono::steady_clock::now();require(completeSorted(store,rare).size()==100,"selective full-set sorted query failed");
    std::cout<<"rare_sorted_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-rareStart).count()<<"\n";
    rare.text=L"中文";auto chineseStart=std::chrono::steady_clock::now();require(completeSorted(store,rare).size()==1,"short Chinese globally sorted query failed");
    std::cout<<"chinese_sorted_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-chineseStart).count()<<"\n";
    desk::SearchQuery scan;scan.text=L"regex:^[A-Z]+$";
    auto scanStart=std::chrono::steady_clock::now();auto hits=store.query(scan);int batches=1;
    while(store.queryPending() && std::chrono::steady_clock::now()-scanStart<std::chrono::seconds(15)) {
        require(sqlite3_exec(db,"UPDATE search_meta SET value='heartbeat' WHERE key='message'",nullptr,nullptr,nullptr)==SQLITE_OK,"heartbeat update failed");
        hits=store.query(scan);++batches;
    }
    require(!store.queryPending() && hits.empty(),"non-file heartbeat writes restarted a sorted continuation indefinitely");
    std::cout<<"heartbeat_safe_regex_batches="<<batches<<" total_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-scanStart).count()<<"\n";
    sqlite3_stmt* plan=nullptr;
    sqlite3_prepare_v2(db,"EXPLAIN QUERY PLAN SELECT id FROM files INDEXED BY files_sort_name WHERE (name_fold,id)>('report-10500000.txt',500001) ORDER BY name_fold,id LIMIT 100",-1,&plan,nullptr);
    while(sqlite3_step(plan)==SQLITE_ROW) std::cout<<"sort_plan="<<sqlite3_column_text(plan,3)<<"\n";sqlite3_finalize(plan);
    sqlite3_close(db);std::cout<<"db_bytes="<<fs::file_size(data/L"files.db")<<"\n";return 0;
}

struct IdleSample {
    int64_t revision=0,count=0,seen=0;
    uint64_t writeOps=0,writeBytes=0,cpuTicks=0,walBytes=0,walStamp=0;
};
static uint64_t fileTicks(FILETIME value) {return (static_cast<uint64_t>(value.dwHighDateTime)<<32)|value.dwLowDateTime;}
static IdleSample idleSample(const fs::path& data,HANDLE process) {
    IdleSample value;sqlite3* db=nullptr;require(sqlite3_open((data/L"files.db").string().c_str(),&db)==SQLITE_OK,"cannot measure dedicated idle index");
    sqlite3_stmt* statement=nullptr;
    require(sqlite3_prepare_v2(db,"SELECT (SELECT CAST(value AS INTEGER) FROM search_meta WHERE key='files_revision'),count(*),coalesce(max(seen),0) FROM files",-1,&statement,nullptr)==SQLITE_OK,"cannot read idle counters");
    require(sqlite3_step(statement)==SQLITE_ROW,"idle counters unavailable");value.revision=sqlite3_column_int64(statement,0);value.count=sqlite3_column_int64(statement,1);value.seen=sqlite3_column_int64(statement,2);sqlite3_finalize(statement);sqlite3_close(db);
    IO_COUNTERS io{};GetProcessIoCounters(process,&io);value.writeOps=io.WriteOperationCount;value.writeBytes=io.WriteTransferCount;
    FILETIME creation{},exit{},kernel{},user{};GetProcessTimes(process,&creation,&exit,&kernel,&user);value.cpuTicks=fileTicks(kernel)+fileTicks(user);
    WIN32_FILE_ATTRIBUTE_DATA wal{};if(GetFileAttributesExW((data/L"files.db-wal").c_str(),GetFileExInfoStandard,&wal)) {value.walBytes=(static_cast<uint64_t>(wal.nFileSizeHigh)<<32)|wal.nFileSizeLow;value.walStamp=fileTicks(wal.ftLastWriteTime);}
    return value;
}
static void selfFeedbackCase(const fs::path& base,int mode,bool enforce) {
    const auto root=base/L"watch";fs::create_directories(root);
    const auto physical=mode==0 ? base/L"external-data" : root/L"physical-data";fs::create_directories(physical);
    auto data=physical;
    if(mode==2) {data=base/L"logical-alias";require(junction(data,physical),"cannot make controlled alias fixture");}
    put(root/L"legitimate-document.txt");
    put(physical/L"kept-user-document.txt");
    put(root/L"user-databases"/L"files.db","legitimate synthetic user database");
    {
        desk::SearchStore store(data);Child worker(L"--worker \""+data.wstring()+L"\" \""+root.wstring()+L"\" "+std::to_wstring(GetCurrentProcessId()));
        require(eventually([&]{return !store.status().building && store.query(L"legitimate-document").size()==1;},20),"idle fixture initial scan did not finish");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        auto before=idleSample(data,worker.process.hProcess);auto start=std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        auto after=idleSample(data,worker.process.hProcess);double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        SYSTEM_INFO system{};GetSystemInfo(&system);
        double cpu=100.0*static_cast<double>(after.cpuTicks-before.cpuTicks)/10000000.0/seconds/std::max<DWORD>(1,system.dwNumberOfProcessors);
        std::cout<<"idle_mode="<<mode<<" start_count="<<before.count<<" end_count="<<after.count<<" revision_delta="<<after.revision-before.revision<<" seen_changed="<<(after.seen!=before.seen)<<" write_ops="<<after.writeOps-before.writeOps<<" write_bytes="<<after.writeBytes-before.writeBytes<<" wal_before="<<before.walBytes<<" wal_after="<<after.walBytes<<" wal_mtime_changed="<<(after.walStamp!=before.walStamp)<<" machine_cpu_percent="<<cpu<<"\n"<<std::flush;
        if(enforce) {
            require(after.revision==before.revision && after.count==before.count && after.seen==before.seen,"own index writes caused idle indexing feedback");
            auto runtime=store.query(L"in:\""+physical.wstring()+L"\" files.db");
            require(runtime.empty(),"runtime index file or SQLite sidecar appeared in file results");
            if(mode!=0) require(store.query(L"kept-user-document").size()==1,"runtime exclusion hid a legitimate document inside the data directory");
            auto userDb=store.query(L"files.db");require(userDb.size()==1 && userDb[0].path.find(L"user-databases")!=std::wstring::npos,"runtime exclusion hid an unrelated user database");
        }
        put(root/L"timely-external-change.txt");
        require(eventually([&]{return store.query(L"timely-external-change").size()==1;},5),"own-file exclusion suppressed external directory changes");
        require(desk::stopIndexWorker(data),"cannot stop own audit worker");require(WaitForSingleObject(worker.process.hProcess,5000)==WAIT_OBJECT_0,"own audit worker did not exit");
    }
    if(mode==2) require(RemoveDirectoryW(data.c_str())!=FALSE,"cannot remove controlled audit junction");
    fs::remove_all(base);
}
static int selfFeedbackAudit(bool enforce) {
    wchar_t temporary[MAX_PATH]{};GetTempPathW(MAX_PATH,temporary);
    auto base=fs::path(temporary)/(L"desk-index-idle-audit-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::cout<<"idle_audit_fixture="<<base.string()<<"\n"<<std::flush;
    for(int mode=0;mode<3;++mode) selfFeedbackCase(base/std::to_wstring(mode),mode,enforce);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"--worker")
        return desk::indexWorkerMain(argv[2], {argv[3]}, static_cast<DWORD>(std::stoul(argv[4])));
    if (argc == 2 && std::wstring(argv[1]) == L"--short-parent") {
        Sleep(500);
        return 0;
    }
    if(argc>=2 && std::wstring(argv[1])==L"--benchmark") {
        try {return benchmark(argc>2 ? std::stoull(argv[2]) : 1000000,argc>3 ? fs::path(argv[3]) : fs::path{});} catch(const std::exception& error) {std::cerr<<"BENCHMARK FAIL: "<<error.what()<<"\n";return 1;}
    }
    if(argc==3 && std::wstring(argv[1])==L"--sorted-benchmark") {
        try {return sortedBenchmark(argv[2]);} catch(const std::exception& error) {std::cerr<<"SORT BENCHMARK FAIL: "<<error.what()<<"\n";return 1;}
    }
    if(argc==2 && (std::wstring(argv[1])==L"--self-feedback-audit" || std::wstring(argv[1])==L"--self-feedback-regression")) {
        try {return selfFeedbackAudit(std::wstring(argv[1])==L"--self-feedback-regression");} catch(const std::exception& error) {std::cerr<<"IDLE AUDIT FAIL: "<<error.what()<<"\n";return 1;}
    }
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temporary);
    const auto base = fs::path(temporary) / (L"desk-search-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto root = base / L"root";
    const auto data = base / L"data";
    try {
        require(desk::search_detail::shortQueryToken(L"😀笑")=="b0001f60000007b11","short gram encoding does not preserve Unicode scalar pairs");
        require(desk::search_detail::shortQueryToken(L"中")=="u00004e2d","short unigram encoding failed");
        {
            const std::wstring name=L"测试文件😀.txt";
            std::vector<unsigned char> buffer(offsetof(USN_RECORD_V2,FileName)+name.size()*sizeof(wchar_t));
            auto* row=reinterpret_cast<USN_RECORD_V2*>(buffer.data());
            row->RecordLength=static_cast<DWORD>(buffer.size());row->MajorVersion=2;
            row->FileReferenceNumber=42;row->ParentFileReferenceNumber=5;row->Usn=123;
            row->Reason=USN_REASON_RENAME_NEW_NAME;row->FileAttributes=FILE_ATTRIBUTE_NORMAL;
            row->FileNameOffset=offsetof(USN_RECORD_V2,FileName);row->FileNameLength=static_cast<WORD>(name.size()*sizeof(wchar_t));
            memcpy(buffer.data()+row->FileNameOffset,name.data(),row->FileNameLength);
            int visited=0;
            require(desk::search_detail::visitUsnRecords(buffer.data(),buffer.size(),[&](uint64_t file,uint64_t parent,int64_t stamp,DWORD reason,DWORD attributes,const std::wstring& text) {
                require(file==42 && parent==5 && stamp==123 && reason==USN_REASON_RENAME_NEW_NAME && attributes==FILE_ATTRIBUTE_NORMAL && text==name,"USN metadata or Unicode name was corrupted");++visited;
            }) && visited==1,"valid NTFS change record was rejected");
            row->FileNameOffset=static_cast<WORD>(buffer.size());
            require(!desk::search_detail::visitUsnRecords(buffer.data(),buffer.size(),[](auto,auto,auto,auto,auto,const auto&) {}),"out-of-bounds NTFS record was accepted");
            row->FileNameOffset=offsetof(USN_RECORD_V2,FileName);row->MajorVersion=3;
            require(!desk::search_detail::visitUsnRecords(buffer.data(),buffer.size(),[](auto,auto,auto,auto,auto,const auto&) {}),"unsupported NTFS record version did not request fallback");
        }
        {
            auto broken=base/L"broken-data";
            put(broken/L"files.db","not a SQLite database");
            desk::SearchStore unavailable(broken);
            require(unavailable.query(L"example").empty() && unavailable.status().message.find(L"无法打开")!=std::wstring::npos,"corrupt index should produce a usable failure status");
            require(fs::file_size(broken/L"files.db")==21,"opening a corrupt index altered the preserved original");
        }
        sortedSearchTests(base/L"sort-data");
        selfFeedbackAudit(true);
        put(root / L"上海发票2026.txt");
        put(root / L"项目资料" / L"报告_财务.txt");
        put(root / L"literal100%_.txt");
        put(root / L"notes.md");
        {
            desk::SearchStore store(data);
            Child worker(L"--worker \"" + data.wstring() + L"\" \"" + root.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId()));
            require(eventually([&] { return store.query(L"上海发票").size() == 1; }), "initial scan does not expose a real file");
            require(eventually([&] { return !store.status().building && store.status().total == 5; }), "scan never reaches a complete persistent snapshot");
            require(store.query(L"财务").size() == 1, "two-character Chinese substring failed");
            require(store.query(L"务").size() == 1, "one-character Chinese substring failed");
            require(store.query(L"上海 2026").size() == 1, "multiple terms should intersect");
            require(store.query(L"type:folder").size() == 1, "folder filter failed");
            require(store.query(L"ext:md").size() == 1, "extension filter failed");
            require(store.query(L"*.txt").size() == 3, "wildcard names failed");
            require(store.query(L"100%_").size() == 1, "literal SQL wildcard characters must stay literal");
            require(store.query(L"in:\"" + (root / L"项目资料").wstring() + L"\" 财务").size() == 1, "directory restriction failed");
            require(store.query(L"regex:^上海.*txt$").size() == 1, "bounded basic regular expression failed");
            require(store.query(L"", 1).size() == 1, "result limit failed");
            auto firstPage=store.query(L"",2),secondPage=store.query(L"",2,firstPage.back().id),lastPage=store.query(L"",2,secondPage.back().id);
            require(firstPage.size()==2 && secondPage.size()==2 && lastPage.size()==1 && firstPage.front().id>firstPage.back().id && secondPage.front().id<firstPage.back().id && lastPage.front().id<secondPage.back().id,"stable file rowid paging duplicated or lost entries");
            auto folders=store.query(L"type:folder",1);
            require(folders.size()==1 && store.query(L"type:folder",1,folders.back().id).empty(),"folder filter did not preserve the page cursor");
            require(CreateHardLinkW((root / L"发票硬链接.txt").c_str(), (root / L"上海发票2026.txt").c_str(), nullptr) != FALSE, "cannot create hardlink fixture");
            require(eventually([&] { return store.query(L"发票硬链接").size() == 1; }), "hardlink path was not indexed");
            fs::remove(root / L"发票硬链接.txt");
            require(eventually([&] { return store.query(L"发票硬链接").empty() && store.query(L"上海发票").size() == 1; }), "hardlink removal removed the surviving link");
            bool linked=CreateSymbolicLinkW((root / L"循环链接").c_str(),root.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)!=FALSE;
            if(!linked) linked=junction(root/L"循环链接",root);
            if(linked) {
                require(eventually([&] { return store.query(L"循环链接").size()==1 && !store.status().building; }),"reparse loop was traversed or not indexed as a directory");
                fs::remove(root / L"循环链接");
                require(eventually([&] { return store.query(L"循环链接").empty(); }),"deleted reparse directory remained indexed");
            } else std::cout<<"SKIP: filesystem does not allow a junction or directory symlink fixture\n";

            // Stop only our own test worker briefly so the OS notification
            // buffer really overflows. Recovery must use directory reconciliation.
            HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME,FALSE,worker.process.dwThreadId);
            require(thread!=nullptr && SuspendThread(thread)!=static_cast<DWORD>(-1),"cannot pause test worker for notification overflow");
            // Long names guarantee overflow of the enlarged 256 KiB local
            // queue on systems that coalesce create/write notifications.
            for(int i=0;i<2500;++i) put(root / L"burst" / (L"overflow-checkpoint-"+std::to_wstring(i)+std::wstring(96,L'x')+L".txt"));
            ResumeThread(thread);CloseHandle(thread);
            require(eventually([&] {return store.query(L"overflow-checkpoint-2499").size()==1 && !store.status().building && store.status().message.find(L"通知溢出校准")!=std::wstring::npos;},120),"notification overflow did not reconcile the affected root");
            fs::remove_all(root / L"burst");
            require(eventually([&] {return store.query(L"overflow-checkpoint").empty();},30),"bulk directory deletion left descendants indexed");

            put(root / L"新增报告.txt");
            require(eventually([&] { return store.query(L"新增报告").size() == 1; }), "directory notification did not index creation");
            fs::rename(root / L"新增报告.txt", root / L"重命名凭证.txt");
            require(eventually([&] { return store.query(L"新增报告").empty() && store.query(L"重命名凭证").size() == 1; }), "rename left stale paths");
            fs::rename(root / L"项目资料", root / L"归档资料");
            require(eventually([&] {
                auto hits = store.query(L"财务");
                return hits.size() == 1 && hits.front().path.find(L"归档资料") != std::wstring::npos;
            }), "folder rename did not update descendant paths");
            fs::remove(root / L"重命名凭证.txt");
            fs::remove_all(root / L"归档资料");
            require(eventually([&] { return store.query(L"凭证").empty() && store.query(L"财务").empty(); }), "deletion left stale rows");
            worker.stopAbruptly();
        }
        {
            fs::rename(root, base / L"temporarily-offline");
            desk::SearchStore offline(data);
            Child worker(L"--worker \"" + data.wstring() + L"\" \"" + root.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId()));
            require(eventually([&] { return offline.status().message.find(L"不可监听") != std::wstring::npos && !offline.status().building; }), "offline root was not reported");
            require(offline.query(L"上海发票").size() == 1, "unavailable root discarded the existing index");
            worker.stopAbruptly();
            fs::rename(base / L"temporarily-offline", root);
            fs::remove(root / L"notes.md");
            put(root / L"离线期间新增.txt");
        }
        {
            desk::SearchStore restarted(data);
            require(restarted.query(L"上海发票").size() == 1, "reconciliation hid previous results before scanning");
            Child worker(L"--worker \"" + data.wstring() + L"\" \"" + root.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId()));
            require(eventually([&] { return restarted.query(L"离线期间新增").size() == 1 && restarted.query(L"notes").empty(); }), "restart did not reconcile offline changes");
            require(desk::stopIndexWorker(data),"graceful stop event could not be signaled");
            require(WaitForSingleObject(worker.process.hProcess,5000)==WAIT_OBJECT_0,"graceful stop left the worker running");
            DWORD exitCode=1;GetExitCodeProcess(worker.process.hProcess,&exitCode);
            require(exitCode==0,"graceful stop returned an error");
        }
        {
            desk::SearchStore reopened(data);
            require(reopened.query(L"上海发票").size() == 1, "restart discarded persistent index");
            require(reopened.query(L"literal").size() == 1, "WAL recovery lost committed index rows");
            sqlite3* database = nullptr;
            require(sqlite3_open((data / L"files.db").string().c_str(), &database) == SQLITE_OK, "cannot inspect actual index database");
            sqlite3_stmt* statement = nullptr;
            require(sqlite3_prepare_v2(database, "PRAGMA quick_check", -1, &statement, nullptr) == SQLITE_OK, "cannot run index integrity check");
            require(sqlite3_step(statement) == SQLITE_ROW && std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) == "ok", "index integrity failed after worker interruption");
            sqlite3_finalize(statement);
            sqlite3_close(database);
        }
        {
            Child parent(L"--short-parent");
            Child worker(L"--worker \"" + data.wstring() + L"\" \"" + root.wstring() + L"\" " + std::to_wstring(parent.process.dwProcessId));
            require(WaitForSingleObject(worker.process.hProcess, 7000) == WAIT_OBJECT_0, "index worker survived its parent process");
        }
        fs::remove_all(base);
        std::cout << "PASS: USN validation, persistent search, Unicode/filters, hardlinks, notification overflow, live rename/delete, offline recovery, WAL recovery, graceful stop, parent lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        std::cerr << "Fixture retained at: " << base.string() << "\n";
        return 1;
    }
}
