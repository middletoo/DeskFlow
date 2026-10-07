#include "search.hpp"
#include "index_pacing.hpp"
#include "sqlite3.h"
#include <winioctl.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace desk {
namespace search_detail {
using UsnVisitor = std::function<void(uint64_t,uint64_t,int64_t,DWORD,DWORD,const std::wstring&)>;
std::vector<uint32_t> scalarCharacters(const std::wstring& text) {
    std::vector<uint32_t> result;
    for(size_t index=0;index<text.size();++index) {
        uint32_t point=text[index];
        if(point>=0xd800 && point<=0xdbff && index+1<text.size() && text[index+1]>=0xdc00 && text[index+1]<=0xdfff)
            point=0x10000+((point-0xd800)<<10)+(text[++index]-0xdc00);
        result.push_back(point);
    }
    return result;
}
std::string hexPoint(uint32_t point) {
    constexpr char hex[]="0123456789abcdef";
    std::string result(8,'0');
    for(int index=7;index>=0;--index) {result[index]=hex[point&15];point>>=4;}
    return result;
}
std::string shortQueryToken(const std::wstring& text) {
    auto points=scalarCharacters(text);
    if(points.size()==1) return "u"+hexPoint(points[0]);
    if(points.size()==2) return "b"+hexPoint(points[0])+hexPoint(points[1]);
    return {};
}
void shortGramsFunction(sqlite3_context* context,int count,sqlite3_value** values) {
    if(count!=1) {sqlite3_result_null(context);return;}
    try {
        const auto* text=static_cast<const wchar_t*>(sqlite3_value_text16(values[0]));
        if(!text) {sqlite3_result_text(context,"",0,SQLITE_STATIC);return;}
        auto points=scalarCharacters(std::wstring(text,sqlite3_value_bytes16(values[0])/sizeof(wchar_t)));
        std::vector<std::string> tokens;
        tokens.reserve(points.size()*2);
        for(size_t index=0;index<points.size();++index) {
            tokens.push_back("u"+hexPoint(points[index]));
            if(index+1<points.size()) tokens.push_back("b"+hexPoint(points[index])+hexPoint(points[index+1]));
        }
        std::sort(tokens.begin(),tokens.end());tokens.erase(std::unique(tokens.begin(),tokens.end()),tokens.end());
        std::string grams;
        for(const auto& token:tokens) {if(!grams.empty()) grams+=' ';grams+=token;}
        sqlite3_result_text(context,grams.data(),static_cast<int>(grams.size()),SQLITE_TRANSIENT);
    } catch(...) {sqlite3_result_error_nomem(context);}
}
void typeKeyFunction(sqlite3_context* context,int count,sqlite3_value** values) {
    if(count!=2) {sqlite3_result_null(context);return;}
    if(sqlite3_value_int(values[1])) {sqlite3_result_text(context,"0folder",-1,SQLITE_STATIC);return;}
    try {
        const auto* raw=sqlite3_value_text(values[0]);
        std::string name=raw ? reinterpret_cast<const char*>(raw) : "";
        auto dot=name.rfind('.');
        auto key=std::string("1:")+(dot==std::string::npos || dot==0 ? std::string{} : name.substr(dot+1));
        sqlite3_result_text(context,key.c_str(),static_cast<int>(key.size()),SQLITE_TRANSIENT);
    } catch(...) {sqlite3_result_error_nomem(context);}
}
void registerShortGrams(sqlite3* database) {
    if(sqlite3_create_function_v2(database,"desk_shortgrams",1,SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS,nullptr,shortGramsFunction,nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
    if(sqlite3_create_function_v2(database,"desk_typekey",2,SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_INNOCUOUS,nullptr,typeKeyFunction,nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
}
bool backfillShortGrams(sqlite3*,size_t);
bool prepareSortIndexes(sqlite3*,size_t);
bool visitUsnRecords(const void* input,size_t length,const UsnVisitor& visitor) {
    const auto* bytes=static_cast<const unsigned char*>(input);
    // Validate the entire kernel buffer before making any database changes.
    for(int pass=0;pass<2;++pass) {
        size_t offset=0;
        while(offset<length) {
            constexpr size_t fixed=offsetof(USN_RECORD_V2,FileName);
            if(length-offset<fixed) return false;
            USN_RECORD_V2 record{};
            memcpy(&record,bytes+offset,fixed);
            if(record.MajorVersion!=2 || record.RecordLength<fixed || record.RecordLength>length-offset ||
               record.FileNameOffset<fixed || record.FileNameLength%sizeof(wchar_t) ||
               record.FileNameOffset>record.RecordLength || record.FileNameLength>record.RecordLength-record.FileNameOffset) return false;
            if(pass==1) {
                std::wstring name(record.FileNameLength/sizeof(wchar_t),L'\0');
                memcpy(name.data(),bytes+offset+record.FileNameOffset,record.FileNameLength);
                visitor(record.FileReferenceNumber,record.ParentFileReferenceNumber,record.Usn,record.Reason,record.FileAttributes,name);
            }
            offset+=record.RecordLength;
        }
    }
    return true;
}
}
namespace {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring wide(const unsigned char* value) {
    if (!value) return {};
    const char* bytes = reinterpret_cast<const char*>(value);
    int count = MultiByteToWideChar(CP_UTF8, 0, bytes, -1, nullptr, 0);
    if (count < 2) return {};
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes, -1, result.data(), count);
    result.pop_back();
    return result;
}
std::wstring fold(const std::wstring& value) {
    if (value.empty()) return {};
    int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!length) return value;
    std::wstring result(length, L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr, 0);
    return result;
}
std::wstring normalize(const std::wstring& path) {
    std::wstring result(32768, L'\0');
    DWORD count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
    if (!count || count >= result.size()) throw std::runtime_error("Index root path exceeds Windows path limits");
    result.resize(count);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    if (result.starts_with(L"\\\\?\\") && !result.starts_with(L"\\\\?\\UNC\\")) result.erase(0, 4);
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}
std::wstring extended(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}
std::wstring join(const std::wstring& path, const std::wstring& name) {
    return path + (path.empty() || path.back() != L'\\' ? L"\\" : L"") + name;
}
bool under(const std::wstring& path, const std::wstring& directory) {
    auto candidate = fold(path), prefix = fold(directory);
    return candidate == prefix || (candidate.starts_with(prefix) &&
           (prefix.back() == L'\\' || (candidate.size() > prefix.size() && candidate[prefix.size()] == L'\\')));
}
int64_t epoch() {
    FILETIME stamp{};
    GetSystemTimeAsFileTime(&stamp);
    return static_cast<int64_t>((static_cast<uint64_t>(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime);
}
size_t codepoints(const std::wstring& value) {
    size_t count = 0;
    for (size_t i = 0; i < value.size(); ++i, ++count)
        if (value[i] >= 0xd800 && value[i] <= 0xdbff && i + 1 < value.size() && value[i + 1] >= 0xdc00 && value[i + 1] <= 0xdfff) ++i;
    return count;
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value = other.value; other.value = nullptr; }
        return *this;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    void reset(HANDLE handle = nullptr) {
        if (*this) CloseHandle(value);
        value = handle;
    }
};
std::wstring finalDirectory(const std::wstring& path) {
    Handle directory(CreateFileW(extended(path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(!directory) return {};
    std::wstring result(32768,L'\0');
    DWORD count=GetFinalPathNameByHandleW(directory.value,result.data(),static_cast<DWORD>(result.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!count || count>=result.size()) return {};
    result.resize(count);
    if(result.starts_with(L"\\\\?\\UNC\\")) result=L"\\\\"+result.substr(8);
    return normalize(result);
}

struct Statement {
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
    void text(int column, const std::wstring& valueToBind) {
        auto bytes = utf8(valueToBind);
        sqlite3_bind_text(value, column, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
    }
    void bytes(int column, const std::string& bytes) {
        sqlite3_bind_text(value, column, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
    }
    void number(int column, int64_t numberToBind) { sqlite3_bind_int64(value, column, numberToBind); }
    bool row() {
        int result = sqlite3_step(value);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(value)));
    }
    void run() { while (row()) {} }
};

void execute(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "SQLite error";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}
struct Database {
    sqlite3* value = nullptr;
    explicit Database(const fs::path& directory, int cacheKiB,bool queryReader=false) {
        fs::create_directories(directory);
        auto path=directory / L"files.db";
        std::error_code fileError;
        const bool readOnly=queryReader&&fs::is_regular_file(path,fileError)&&fs::file_size(path,fileError)>0&&!fileError;
        auto name = utf8(path.wstring());
        if (sqlite3_open_v2(name.c_str(), &value, (readOnly?SQLITE_OPEN_READONLY:SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE) | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            auto message = std::string(value ? sqlite3_errmsg(value) : "Cannot open files.db");
            if (value) sqlite3_close(value);
            value = nullptr;
            throw std::runtime_error(message);
        }
        try {
        if(readOnly){
            sqlite3_busy_timeout(value,80);search_detail::registerShortGrams(value);
            execute(value,"PRAGMA temp_store=FILE;PRAGMA mmap_size=0;");
            execute(value,("PRAGMA cache_size=-"+std::to_string(cacheKiB)).c_str());
            return;
        }
        sqlite3_busy_timeout(value, 5000);
        search_detail::registerShortGrams(value);
        execute(value, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=FILE; PRAGMA mmap_size=0; PRAGMA wal_autocheckpoint=1000;");
        execute(value, ("PRAGMA cache_size=-" + std::to_string(cacheKiB)).c_str());
        execute(value,
            "CREATE TABLE IF NOT EXISTS search_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
            "INSERT OR IGNORE INTO search_meta VALUES('total','0'),('building','0'),('message','索引进程尚未启动'),('pid','0'),('short_complete','0'),('short_cursor','0'),('type_complete','0'),('type_cursor','0'),('size_complete','0'),('size_cursor','0'),('size_unknown','0'),('sort_name','0'),('sort_path','0'),('sort_size','0'),('sort_type','0'),('files_revision','0');"
            "CREATE TABLE IF NOT EXISTS search_roots(id INTEGER PRIMARY KEY,path TEXT NOT NULL,path_key TEXT UNIQUE NOT NULL,mode TEXT NOT NULL DEFAULT 'directory',journal_id TEXT,next_usn INTEGER,serial INTEGER,complete INTEGER NOT NULL DEFAULT 0);"
            "CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY,root_id INTEGER NOT NULL,frn INTEGER,path TEXT NOT NULL,path_fold TEXT NOT NULL,name TEXT NOT NULL,name_fold TEXT NOT NULL,folder INTEGER NOT NULL,size INTEGER NOT NULL,seen INTEGER NOT NULL,short_ready INTEGER NOT NULL DEFAULT 0,type_key TEXT NOT NULL DEFAULT '',type_ready INTEGER NOT NULL DEFAULT 0,size_known INTEGER NOT NULL DEFAULT 1,UNIQUE(root_id,path_fold));"
            "CREATE INDEX IF NOT EXISTS files_by_root ON files(root_id,seen);"
            "CREATE INDEX IF NOT EXISTS files_by_frn ON files(root_id,frn);"
            "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5(name_fold,path_fold,content=files,content_rowid=id,tokenize='trigram');"
            "CREATE TRIGGER IF NOT EXISTS files_insert AFTER INSERT ON files BEGIN INSERT INTO files_fts(rowid,name_fold,path_fold) VALUES(new.id,new.name_fold,new.path_fold); UPDATE search_meta SET value=CAST(value AS INTEGER)+1 WHERE key='total'; END;"
            "CREATE TRIGGER IF NOT EXISTS files_delete AFTER DELETE ON files BEGIN INSERT INTO files_fts(files_fts,rowid,name_fold,path_fold) VALUES('delete',old.id,old.name_fold,old.path_fold); UPDATE search_meta SET value=CAST(value AS INTEGER)-1 WHERE key='total'; END;"
            "CREATE TRIGGER IF NOT EXISTS files_update AFTER UPDATE OF name_fold,path_fold ON files WHEN old.name_fold<>new.name_fold OR old.path_fold<>new.path_fold BEGIN INSERT INTO files_fts(files_fts,rowid,name_fold,path_fold) VALUES('delete',old.id,old.name_fold,old.path_fold); INSERT INTO files_fts(rowid,name_fold,path_fold) VALUES(new.id,new.name_fold,new.path_fold); END;"
            "CREATE TABLE IF NOT EXISTS ntfs_nodes(root_id INTEGER NOT NULL,frn INTEGER NOT NULL,parent_frn INTEGER NOT NULL,name TEXT NOT NULL,folder INTEGER NOT NULL,seen INTEGER NOT NULL,attributes INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(root_id,frn)) WITHOUT ROWID;"
            "CREATE INDEX IF NOT EXISTS nodes_by_parent ON ntfs_nodes(root_id,parent_frn);"
            "CREATE TABLE IF NOT EXISTS scan_jobs(id INTEGER PRIMARY KEY,root_id INTEGER NOT NULL,prefix TEXT NOT NULL,stamp INTEGER NOT NULL,dirty INTEGER NOT NULL DEFAULT 0);"
            "CREATE TABLE IF NOT EXISTS scan_queue(id INTEGER PRIMARY KEY,job_id INTEGER NOT NULL,path TEXT NOT NULL,path_key TEXT NOT NULL,UNIQUE(job_id,path_key));"
            "CREATE TABLE IF NOT EXISTS ntfs_refresh(root_id INTEGER NOT NULL,frn INTEGER NOT NULL,stamp INTEGER NOT NULL,PRIMARY KEY(root_id,frn)) WITHOUT ROWID;"
        );
        auto ensureColumn=[&](const char* table,const char* column,const char* declaration) {
            Statement columns(value,(std::string("PRAGMA table_info(")+table+")").c_str());
            bool found=false;
            while(columns.row()) if(std::string(reinterpret_cast<const char*>(sqlite3_column_text(columns.value,1)))==column) found=true;
            if(!found) execute(value,(std::string("ALTER TABLE ")+table+" ADD COLUMN "+column+" "+declaration).c_str());
            return !found;
        };
        ensureColumn("ntfs_nodes","attributes","INTEGER NOT NULL DEFAULT 0");
        ensureColumn("scan_jobs","dirty","INTEGER NOT NULL DEFAULT 0");
        if(ensureColumn("files","short_ready","INTEGER NOT NULL DEFAULT 0")) execute(value,"UPDATE search_meta SET value='0' WHERE key IN('short_complete','short_cursor')");
        ensureColumn("files","type_key","TEXT NOT NULL DEFAULT ''");
        if(ensureColumn("files","type_ready","INTEGER NOT NULL DEFAULT 0")) execute(value,"UPDATE search_meta SET value='0' WHERE key IN('type_complete','type_cursor','sort_type')");
        if(ensureColumn("files","size_known","INTEGER NOT NULL DEFAULT 0")) execute(value,"UPDATE search_meta SET value='0' WHERE key IN('size_complete','size_cursor','sort_size')");
        execute(value,
            "CREATE VIRTUAL TABLE IF NOT EXISTS files_short USING fts5(grams,content='',detail=none,tokenize='ascii');"
            "CREATE TRIGGER IF NOT EXISTS files_short_insert AFTER INSERT ON files BEGIN INSERT INTO files_short(rowid,grams) VALUES(new.id,desk_shortgrams(new.name_fold)); UPDATE files SET short_ready=1 WHERE id=new.id; END;"
            "CREATE TRIGGER IF NOT EXISTS files_short_delete AFTER DELETE ON files WHEN old.short_ready=1 BEGIN INSERT INTO files_short(files_short,rowid,grams) VALUES('delete',old.id,desk_shortgrams(old.name_fold)); END;"
            "CREATE TRIGGER IF NOT EXISTS files_short_update AFTER UPDATE OF name_fold ON files WHEN old.name_fold<>new.name_fold BEGIN INSERT INTO files_short(files_short,rowid,grams) SELECT 'delete',old.id,desk_shortgrams(old.name_fold) WHERE old.short_ready=1; INSERT INTO files_short(rowid,grams) VALUES(new.id,desk_shortgrams(new.name_fold)); UPDATE files SET short_ready=1 WHERE id=new.id; END;"
            "CREATE TRIGGER IF NOT EXISTS files_type_insert AFTER INSERT ON files BEGIN UPDATE files SET type_key=desk_typekey(new.name_fold,new.folder),type_ready=1,size_known=CASE WHEN new.folder=1 THEN 0 ELSE new.size_known END WHERE id=new.id; END;"
            "CREATE TRIGGER IF NOT EXISTS files_type_update AFTER UPDATE OF name_fold,folder ON files WHEN old.name_fold<>new.name_fold OR old.folder<>new.folder OR new.type_ready=0 BEGIN UPDATE files SET type_key=desk_typekey(new.name_fold,new.folder),type_ready=1,size_known=CASE WHEN new.folder=1 THEN 0 ELSE new.size_known END WHERE id=new.id; END;"
            "CREATE TRIGGER IF NOT EXISTS files_size_insert AFTER INSERT ON files WHEN new.folder=0 AND new.size_known=0 BEGIN UPDATE search_meta SET value=CAST(value AS INTEGER)+1 WHERE key='size_unknown'; UPDATE search_meta SET value='0' WHERE key='size_complete'; UPDATE search_meta SET value=min(CAST(value AS INTEGER),new.id-1) WHERE key='size_cursor'; END;"
            "CREATE TRIGGER IF NOT EXISTS files_size_delete AFTER DELETE ON files WHEN old.folder=0 AND old.size_known=0 BEGIN UPDATE search_meta SET value=max(0,CAST(value AS INTEGER)-1) WHERE key='size_unknown'; END;"
            "CREATE TRIGGER IF NOT EXISTS files_size_update AFTER UPDATE OF size_known,folder ON files WHEN old.size_known<>new.size_known OR old.folder<>new.folder BEGIN UPDATE search_meta SET value=max(0,CAST(value AS INTEGER)+(CASE WHEN new.folder=0 AND new.size_known=0 THEN 1 ELSE 0 END)-(CASE WHEN old.folder=0 AND old.size_known=0 THEN 1 ELSE 0 END)) WHERE key='size_unknown'; UPDATE search_meta SET value='0' WHERE key='size_complete' AND new.folder=0 AND new.size_known=0; UPDATE search_meta SET value=min(CAST(value AS INTEGER),new.id-1) WHERE key='size_cursor' AND new.folder=0 AND new.size_known=0; END;"
            "CREATE TRIGGER IF NOT EXISTS files_revision_insert AFTER INSERT ON files BEGIN UPDATE search_meta SET value=CAST(value AS INTEGER)+1 WHERE key='files_revision'; END;"
            "CREATE TRIGGER IF NOT EXISTS files_revision_delete AFTER DELETE ON files BEGIN UPDATE search_meta SET value=CAST(value AS INTEGER)+1 WHERE key='files_revision'; END;"
            "CREATE TRIGGER IF NOT EXISTS files_revision_update AFTER UPDATE OF name,name_fold,path,path_fold,folder,size,size_known,type_key ON files WHEN old.name<>new.name OR old.name_fold<>new.name_fold OR old.path<>new.path OR old.path_fold<>new.path_fold OR old.folder<>new.folder OR old.size<>new.size OR old.size_known<>new.size_known OR old.type_key<>new.type_key BEGIN UPDATE search_meta SET value=CAST(value AS INTEGER)+1 WHERE key='files_revision'; END;"
        );
        } catch(...) {sqlite3_close(value);value=nullptr;throw;}
    }
    ~Database() { if (value) sqlite3_close(value); }
};
struct Transaction {
    sqlite3* db;
    bool committed = false;
    explicit Transaction(sqlite3* database) : db(database) { execute(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { execute(db, "COMMIT"); committed = true; }
};

void meta(sqlite3* db, const char* key, const std::wstring& value) {
    Statement statement(db, "INSERT INTO search_meta VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    statement.bytes(1, key); statement.text(2, value); statement.run();
}
void upsert(sqlite3* db, int64_t rootId, const std::wstring& path, const std::wstring& name,
            DWORD attributes, uint64_t size, int64_t stamp, uint64_t frn = 0,bool sizeKnown=true) {
    Statement statement(db, "INSERT INTO files(root_id,frn,path,path_fold,name,name_fold,folder,size,seen,size_known) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) ON CONFLICT(root_id,path_fold) DO UPDATE SET frn=coalesce(excluded.frn,files.frn),path=excluded.path,name=excluded.name,name_fold=excluded.name_fold,folder=excluded.folder,size=excluded.size,size_known=excluded.size_known,seen=max(files.seen,excluded.seen)");
    statement.number(1, rootId);
    if (frn) statement.number(2, static_cast<int64_t>(frn)); else sqlite3_bind_null(statement.value, 2);
    statement.text(3, path); statement.text(4, fold(path)); statement.text(5, name); statement.text(6, fold(name));
    statement.number(7, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0); statement.number(8, static_cast<int64_t>(size)); statement.number(9, stamp);
    statement.number(10,(attributes&FILE_ATTRIBUTE_DIRECTORY) ? 0 : sizeKnown);
    statement.run();
}
std::wstring subtreePrefix(const std::wstring& path) { return fold(path) + (path.back() == L'\\' ? L"" : L"\\"); }
void erase(sqlite3* db, int64_t rootId, const std::wstring& path) {
    Statement statement(db, "DELETE FROM files WHERE root_id=?1 AND (path_fold=?2 OR (path_fold>=?3 AND path_fold<?4))");
    auto prefix = subtreePrefix(path), upper = prefix; upper.back() = L']';
    statement.number(1, rootId); statement.text(2, fold(path)); statement.text(3, prefix); statement.text(4, upper); statement.run();
}
void eraseDescendants(sqlite3* db,int64_t rootId,const std::wstring& path) {
    Statement statement(db,"DELETE FROM files WHERE root_id=?1 AND path_fold>=?2 AND path_fold<?3");
    auto prefix=subtreePrefix(path),upper=prefix;upper.back()=L']';
    statement.number(1,rootId);statement.text(2,prefix);statement.text(3,upper);statement.run();
}
void protect(sqlite3* db, int64_t rootId, const std::wstring& path, int64_t stamp) {
    Statement statement(db, "UPDATE files SET seen=max(seen,?5) WHERE root_id=?1 AND (path_fold=?2 OR (path_fold>=?3 AND path_fold<?4))");
    auto prefix = subtreePrefix(path), upper = prefix; upper.back() = L']';
    statement.number(1, rootId); statement.text(2, fold(path)); statement.text(3, prefix); statement.text(4, upper); statement.number(5, stamp); statement.run();
}

// A small Thompson-style matcher keeps regex work cancellable and linear in
// text length * pattern length. Deliberately supports literals, ., [] and *+?
// with ^/$ anchors; groups, alternation and backreferences are not accepted.
struct BasicRegex {
    struct Atom { wchar_t literal = 0; bool any = false, negate = false; std::vector<std::pair<wchar_t,wchar_t>> ranges; wchar_t repeat = 0; };
    std::vector<Atom> atoms;
    bool start = false, end = false, valid = true;
    explicit BasicRegex(std::wstring pattern) {
        if (pattern.size() > 256) { valid = false; return; }
        if (!pattern.empty() && pattern.front() == L'^') { start = true; pattern.erase(0, 1); }
        if (!pattern.empty() && pattern.back() == L'$' && (pattern.size() < 2 || pattern[pattern.size()-2] != L'\\')) { end = true; pattern.pop_back(); }
        for (size_t i = 0; i < pattern.size() && valid; ++i) {
            Atom atom;
            auto ch = pattern[i];
            if (ch == L'\\') { if (++i == pattern.size()) { valid = false; break; } atom.literal = pattern[i]; }
            else if (ch == L'.') atom.any = true;
            else if (ch == L'[') {
                if (i + 1 < pattern.size() && pattern[i+1] == L'^') { atom.negate = true; ++i; }
                while (++i < pattern.size() && pattern[i] != L']') {
                    wchar_t first = pattern[i], last = first;
                    if (i+2 < pattern.size() && pattern[i+1] == L'-' && pattern[i+2] != L']') { last = pattern[i+2]; i += 2; }
                    atom.ranges.emplace_back(first,last);
                }
                if (i == pattern.size() || atom.ranges.empty()) { valid = false; break; }
            } else if (ch == L'(' || ch == L')' || ch == L'|' || ch == L'*' || ch == L'+' || ch == L'?' || ch == L'^' || ch == L'$') { valid = false; break; }
            else atom.literal = ch;
            if (i+1 < pattern.size() && (pattern[i+1] == L'*' || pattern[i+1] == L'+' || pattern[i+1] == L'?')) atom.repeat = pattern[++i];
            atoms.push_back(std::move(atom));
        }
    }
    bool match(const std::wstring& input, Clock::time_point deadline) const {
        if (!valid) return false;
        std::vector<unsigned char> previous(atoms.size()+1), current(atoms.size()+1);
        previous[0] = 1;
        auto empty = [&](std::vector<unsigned char>& states) {
            for (size_t k=0;k<atoms.size();++k) if (states[k] && (atoms[k].repeat == L'*' || atoms[k].repeat == L'?')) states[k+1] = 1;
        };
        empty(previous);
        if (previous.back() && (!end || input.empty())) return true;
        for (size_t i=0;i<input.size();++i) {
            if ((i & 127) == 0 && Clock::now() >= deadline) return false;
            std::fill(current.begin(),current.end(),static_cast<unsigned char>(0));
            if (!start) current[0] = 1;
            for (size_t k=0;k<atoms.size();++k) {
                const auto& atom = atoms[k];
                bool accepts = atom.any || (atom.ranges.empty() && atom.literal == input[i]);
                if (!atom.ranges.empty()) {
                    bool inside=false;
                    for (const auto& range:atom.ranges) inside |= input[i]>=range.first && input[i]<=range.second;
                    accepts = atom.negate ? !inside : inside;
                }
                if (accepts && previous[k]) current[k+1] = 1;
                if (accepts && (atom.repeat == L'*' || atom.repeat == L'+') && previous[k+1]) current[k+1] = 1;
            }
            empty(current);
            if (current.back() && (!end || i+1 == input.size())) return true;
            previous.swap(current);
        }
        return previous.back() != 0;
    }
};
struct QueryBudget { Clock::time_point deadline; std::atomic<uint64_t>* sequence; uint64_t own; };
int progress(void* raw) {
    auto& budget = *static_cast<QueryBudget*>(raw);
    return Clock::now() >= budget.deadline || budget.sequence->load() != budget.own;
}
void regexFunction(sqlite3_context* context, int argc, sqlite3_value** arguments) {
    if (argc != 2) { sqlite3_result_int(context,0); return; }
    auto* budget = static_cast<QueryBudget*>(sqlite3_user_data(context));
    if (progress(budget)) { sqlite3_result_int(context,0); return; }
    auto* matcher = static_cast<BasicRegex*>(sqlite3_get_auxdata(context,0));
    if (!matcher) {
        matcher = new BasicRegex(wide(sqlite3_value_text(arguments[0])));
        sqlite3_set_auxdata(context,0,matcher,[](void* ptr){delete static_cast<BasicRegex*>(ptr);});
        // sqlite3_set_auxdata can release data immediately on allocation failure.
        matcher = static_cast<BasicRegex*>(sqlite3_get_auxdata(context,0));
        if (!matcher) { sqlite3_result_error_nomem(context); return; }
    }
    sqlite3_result_int(context,matcher->match(wide(sqlite3_value_text(arguments[1])),budget->deadline));
}
std::vector<std::wstring> tokens(const std::wstring& query) {
    std::vector<std::wstring> result;
    std::wstring token;
    bool quoted = false;
    for (auto ch : query) {
        if (ch == L'"') { quoted = !quoted; continue; }
        if (iswspace(ch) && !quoted) { if (!token.empty()) { result.push_back(std::move(token)); token.clear(); } }
        else token += ch;
        if (result.size() >= 32) break;
    }
    if (!token.empty() && result.size()<32) result.push_back(std::move(token));
    return result;
}
std::wstring longestLiteral(const std::wstring& token) {
    std::wstring longest, current;
    for (auto ch:token) {
        if (ch == L'*' || ch == L'?') { if (codepoints(current)>codepoints(longest)) longest=current; current.clear(); }
        else current += ch;
    }
    if (codepoints(current)>codepoints(longest)) longest=current;
    return longest;
}
std::wstring ftsQuote(const std::wstring& token) {
    std::wstring result=L"\"";
    for (auto ch:token) { result+=ch; if (ch==L'"') result+=ch; }
    return result+L"\"";
}

struct DirectoryEntry { std::wstring name; DWORD attributes = 0; uint64_t size = 0, frn = 0; };
struct DirectoryReader {
    Handle handle;
    HANDLE fallback = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW fallbackData{};
    std::vector<unsigned char> buffer = std::vector<unsigned char>(64*1024);
    size_t offset = std::numeric_limits<size_t>::max();
    bool first = true, fallbackFirst = true, ended = false;
    DWORD error = 0;
    uint64_t parentFrn = 0;
    std::wstring path;
    explicit DirectoryReader(std::wstring directory) : path(std::move(directory)) {
        handle.reset(CreateFileW(extended(path).c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if (!handle) { error=GetLastError(); ended=true; return; }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if(GetFileInformationByHandleEx(handle.value,FileAttributeTagInfo,&tag,sizeof(tag)) && (tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {ended=true;return;}
        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(handle.value,&information)) parentFrn=(static_cast<uint64_t>(information.nFileIndexHigh)<<32)|information.nFileIndexLow;
    }
    ~DirectoryReader() { if (fallback != INVALID_HANDLE_VALUE) FindClose(fallback); }
    bool next(DirectoryEntry& entry) {
        if (ended) return false;
        if (fallback != INVALID_HANDLE_VALUE) {
            if (!fallbackFirst && !FindNextFileW(fallback,&fallbackData)) { error=GetLastError(); ended=true; return false; }
            fallbackFirst=false;
            entry={fallbackData.cFileName,fallbackData.dwFileAttributes,(static_cast<uint64_t>(fallbackData.nFileSizeHigh)<<32)|fallbackData.nFileSizeLow,0};
            return true;
        }
        if (offset == std::numeric_limits<size_t>::max()) {
            if (!GetFileInformationByHandleEx(handle.value,first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,buffer.data(),static_cast<DWORD>(buffer.size()))) {
                error=GetLastError();
                if (first && (error==ERROR_INVALID_PARAMETER || error==ERROR_INVALID_FUNCTION || error==ERROR_NOT_SUPPORTED)) {
                    fallback=FindFirstFileExW(join(extended(path),L"*").c_str(),FindExInfoBasic,&fallbackData,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
                    if (fallback != INVALID_HANDLE_VALUE) return next(entry);
                    error=GetLastError();
                }
                ended=true; return false;
            }
            first=false; offset=0;
        }
        auto* row = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer.data()+offset);
        entry.name.assign(row->FileName,row->FileNameLength/sizeof(wchar_t));
        entry.attributes=row->FileAttributes; entry.size=static_cast<uint64_t>(row->EndOfFile.QuadPart); entry.frn=static_cast<uint64_t>(row->FileId.QuadPart);
        offset = row->NextEntryOffset ? offset+row->NextEntryOffset : std::numeric_limits<size_t>::max();
        return true;
    }
};

struct Watcher {
    int64_t rootId;
    std::wstring path;
    Handle directory, event;
    OVERLAPPED operation{};
    std::vector<unsigned char> buffer=std::vector<unsigned char>(64*1024);
    bool pending=false;
    Clock::time_point retry=Clock::now();
    Watcher(int64_t id,std::wstring directoryPath) : rootId(id),path(std::move(directoryPath)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
        // SMB limits requests to 64 KiB. Local roots retain a larger queue
        // during build/install bursts, avoiding repeated whole-root repair.
        if(!path.starts_with(L"\\") && GetDriveTypeW(fs::path(path).root_path().c_str())!=DRIVE_REMOTE)
            buffer.resize(256*1024);
        open();
    }
    ~Watcher() {close();}
    void close() {
        if (pending && directory) {
            CancelIoEx(directory.value,&operation);
            DWORD ignored=0;
            // The buffer and OVERLAPPED remain alive until cancellation has
            // completed; closing the handle alone is insufficient.
            GetOverlappedResult(directory.value,&operation,&ignored,TRUE);
        }
        pending=false;directory.reset();
    }
    void open() {
        close();
        directory.reset(CreateFileW(extended(path).c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr));
        if (directory) arm();
        retry=Clock::now()+std::chrono::seconds(30);
    }
    void arm() {
        ResetEvent(event.value); operation={}; operation.hEvent=event.value;
        pending=ReadDirectoryChangesW(directory.value,buffer.data(),static_cast<DWORD>(buffer.size()),TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_ATTRIBUTES,
            nullptr,&operation,nullptr)!=FALSE;
        if (!pending) { directory.reset(); retry=Clock::now()+std::chrono::seconds(30); }
    }
};

enum class NtfsPhase {Directory,Enumerate,Materialize,Journal};
struct Root {
    int64_t id=0;
    std::wstring path;
    uint64_t rootFrn=0,journalId=0,enumCursor=0;
    DWORD serial=0;
    Handle volume;
    NtfsPhase phase=NtfsPhase::Directory;
    int64_t buildStamp=0,initialUsn=0,nextUsn=0,materialCursor=0;
    bool materialFirst=true;
    Clock::time_point journalTick=Clock::now();
    std::unordered_map<uint64_t,std::pair<std::wstring,DWORD>> pathCache;
    size_t cacheCharacters=0;
    void clearCache() {pathCache.clear();cacheCharacters=0;}
};
struct Scan { int64_t id=0,rootId=0,stamp=0,queueId=0; std::wstring prefix,path; std::unique_ptr<DirectoryReader> directory; };

class Indexer {
public:
    Database database;
    std::wstring dataPath;
    std::vector<std::wstring> ownDataAliases;
    std::vector<Root> roots;
    std::vector<std::unique_ptr<Watcher>> watchers;
    std::optional<Scan> active;
    std::map<int64_t,Scan> pausedScans;
    int64_t lastScanJob=0;
    uint64_t inaccessible=0,reparse=0,overflows=0;
    std::vector<unsigned char> journalBuffer=std::vector<unsigned char>(64*1024);
    explicit Indexer(const fs::path& data) : database(data,4096),dataPath(normalize(data.wstring())) {
        ownDataAliases.push_back(fold(dataPath));
        auto physical=finalDirectory(dataPath);
        if(!physical.empty() && fold(physical)!=ownDataAliases.front()) ownDataAliases.push_back(fold(physical));
    }
    bool ownIndexPath(const std::wstring& path) {
        auto leaf=fold(fs::path(path).filename().wstring());
        if(leaf!=L"files.db" && leaf!=L"files.db-wal" && leaf!=L"files.db-shm" && leaf!=L"files.db-journal") return false;
        auto parent=normalize(fs::path(path).parent_path().wstring());
        auto key=fold(parent);
        if(std::find(ownDataAliases.begin(),ownDataAliases.end(),key)!=ownDataAliases.end()) return true;
        // RDCW reports the watched spelling, while packaged AppData can resolve
        // onto another volume. Resolve only runtime-name candidates, including
        // deleted sidecars via their still-existing parent directory.
        auto resolved=finalDirectory(parent);
        if(resolved.empty() || std::find(ownDataAliases.begin(),ownDataAliases.end(),fold(resolved))==ownDataAliases.end()) return false;
        if(ownDataAliases.size()<16) ownDataAliases.push_back(std::move(key));
        return true;
    }
    void removeOwnIndexRows() {
        int64_t after=0;
        for(;;) {
            Statement rows(database.value,"SELECT f.id,f.path FROM files f JOIN files_fts ON files_fts.rowid=f.id WHERE files_fts MATCH 'name_fold:\"files.db\"' AND files_fts.rowid>?1 AND f.name_fold IN('files.db','files.db-wal','files.db-shm','files.db-journal') ORDER BY files_fts.rowid LIMIT 256");rows.number(1,after);
            std::vector<std::pair<int64_t,std::wstring>> batch;
            while(rows.row()) batch.emplace_back(sqlite3_column_int64(rows.value,0),wide(sqlite3_column_text(rows.value,1)));
            if(batch.empty()) return;
            std::vector<int64_t> own;
            for(const auto& entry:batch) if(ownIndexPath(entry.second)) own.push_back(entry.first);
            if(!own.empty()) {
                Transaction transaction(database.value);Statement remove(database.value,"DELETE FROM files WHERE id=?1");
                for(auto id:own) {remove.number(1,id);remove.run();sqlite3_reset(remove.value);sqlite3_clear_bindings(remove.value);}
                transaction.commit();
            }
            after=batch.back().first;
        }
    }
    void initialize(const std::vector<std::wstring>& requested) {
        // Old aliased indexes may already contain their own runtime files.
        // Remove only matching physical identities, leaving user DBs untouched.
        removeOwnIndexRows();
        auto rootPaths=requested;
        if (rootPaths.empty()) {
            wchar_t drives[512]{};
            DWORD length=GetLogicalDriveStringsW(512,drives);
            if (length && length<512) for (const wchar_t* drive=drives;*drive;drive+=wcslen(drive)+1)
                if (GetDriveTypeW(drive)==DRIVE_FIXED) rootPaths.emplace_back(drive);
        }
        if (rootPaths.size()>32) throw std::runtime_error("At most 32 index roots are supported");
        Transaction transaction(database.value);
        execute(database.value,"UPDATE search_roots SET complete=0 WHERE id IN(SELECT root_id FROM scan_jobs); DELETE FROM scan_queue; DELETE FROM scan_jobs;");
        for (auto& path:rootPaths) {
            path=normalize(path);
            bool covered=false;
            for (const auto& existing:roots) if (under(path,existing.path)) covered=true;
            if (covered) continue;
            Statement insert(database.value,"INSERT INTO search_roots(path,path_key) VALUES(?1,?2) ON CONFLICT(path_key) DO UPDATE SET path=excluded.path");
            insert.text(1,path); insert.text(2,fold(path)); insert.run();
            Statement select(database.value,"SELECT id FROM search_roots WHERE path_key=?1"); select.text(1,fold(path)); select.row();
            Root root; root.id=sqlite3_column_int64(select.value,0); root.path=path;
            roots.push_back(std::move(root));
        }
        transaction.commit();
        for (auto& root:roots) {
            watchers.push_back(std::make_unique<Watcher>(root.id,root.path));
            if(!tryNtfs(root)) schedule(root.id,root.path);
        }
        publish();
    }
    Root* root(int64_t id) { for(auto& candidate:roots) if(candidate.id==id) return &candidate; return nullptr; }
    bool reparseAncestor(int64_t rootId,std::wstring path) {
        auto* volume=root(rootId);if(!volume) return false;
        for(;;) {
            DWORD attributes=GetFileAttributesW(extended(path).c_str());
            if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return true;
            if(fold(path)==fold(volume->path)) return false;
            auto parent=fs::path(path).parent_path().wstring();
            if(parent.empty() || parent==path || !under(parent,volume->path)) return false;
            path=std::move(parent);
        }
    }
    void enqueue(int64_t job,const std::wstring& path) {
        if (ownIndexPath(path)) return;
        Statement statement(database.value,"INSERT OR IGNORE INTO scan_queue(job_id,path,path_key) VALUES(?1,?2,?3)");
        statement.number(1,job); statement.text(2,path); statement.text(3,fold(path)); statement.run();
    }
    void schedule(int64_t rootId,const std::wstring& path,bool reconcileAgain=false) {
        if (ownIndexPath(path)) return;
        // A newly created directory can arrive after its parent was scanned.
        // Put it on the in-progress root's disk queue instead of losing it.
        Statement jobs(database.value,"SELECT id,prefix FROM scan_jobs WHERE root_id=?1 ORDER BY id"); jobs.number(1,rootId);
        while(jobs.row()) if(under(path,wide(sqlite3_column_text(jobs.value,1)))) {
            auto job=sqlite3_column_int64(jobs.value,0);
            enqueue(job,path);
            if(reconcileAgain) {Statement dirty(database.value,"UPDATE scan_jobs SET dirty=1 WHERE id=?1");dirty.number(1,job);dirty.run();}
            return;
        }
        Statement insert(database.value,"INSERT INTO scan_jobs(root_id,prefix,stamp) VALUES(?1,?2,?3)");
        insert.number(1,rootId); insert.text(2,path); insert.number(3,epoch()); insert.run();
        enqueue(sqlite3_last_insert_rowid(database.value),path);
    }
    void node(int64_t rootId,uint64_t frn,uint64_t parent,const std::wstring& name,bool folder,int64_t stamp,DWORD attributes=0) {
        if (!frn) return;
        Statement insert(database.value,"INSERT INTO ntfs_nodes(root_id,frn,parent_frn,name,folder,seen,attributes) VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(root_id,frn) DO UPDATE SET parent_frn=excluded.parent_frn,name=excluded.name,folder=excluded.folder,seen=max(ntfs_nodes.seen,excluded.seen),attributes=excluded.attributes");
        insert.number(1,rootId); insert.number(2,static_cast<int64_t>(frn)); insert.number(3,static_cast<int64_t>(parent)); insert.text(4,name); insert.number(5,folder); insert.number(6,stamp); insert.number(7,attributes); insert.run();
    }
    bool scanning() {
        for(const auto& volume:roots) if(volume.phase==NtfsPhase::Enumerate || volume.phase==NtfsPhase::Materialize) return true;
        Statement dirty(database.value,"SELECT 1 FROM ntfs_refresh LIMIT 1");if(dirty.row()) return true;
        Statement grams(database.value,"SELECT 1 FROM search_meta WHERE key='short_complete' AND value='0'");if(grams.row()) return true;
        Statement sorting(database.value,"SELECT 1 FROM search_meta WHERE key IN('sort_name','sort_path','sort_size','sort_type','size_complete') AND value='0' LIMIT 1");if(sorting.row()) return true;
        Statement jobs(database.value,"SELECT 1 FROM scan_jobs LIMIT 1"); return jobs.row();
    }
    bool tryNtfs(Root& volume) {
        // Directory roots use the same native implementation on every filesystem.
        // A volume handle is attempted using the existing token; no UAC, journal
        // creation, or privilege adjustment happens in this process.
        if(volume.path.size()!=3 || volume.path[1]!=L':' || volume.path[2]!=L'\\') return false;
        wchar_t filesystem[32]{};
        if(!GetVolumeInformationW(volume.path.c_str(),nullptr,0,&volume.serial,nullptr,nullptr,filesystem,32) || _wcsicmp(filesystem,L"NTFS")) return false;
        volume.volume.reset(CreateFileW((L"\\\\.\\"+volume.path.substr(0,2)).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!volume.volume) return false;
        USN_JOURNAL_DATA_V0 journal{};DWORD returned=0;
        if(!DeviceIoControl(volume.volume.value,FSCTL_QUERY_USN_JOURNAL,nullptr,0,&journal,sizeof(journal),&returned,nullptr)) {volume.volume.reset();return false;}
        Handle directory(CreateFileW(extended(volume.path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if(!directory || !GetFileInformationByHandle(directory.value,&information)) {volume.volume.reset();return false;}
        volume.rootFrn=(static_cast<uint64_t>(information.nFileIndexHigh)<<32)|information.nFileIndexLow;
        Statement stored(database.value,"SELECT journal_id,next_usn,serial,complete,mode FROM search_roots WHERE id=?1");stored.number(1,volume.id);stored.row();
        bool resume=false;
        if(sqlite3_column_type(stored.value,0)!=SQLITE_NULL) {
            auto id=wide(sqlite3_column_text(stored.value,0));
            auto serial=static_cast<DWORD>(sqlite3_column_int64(stored.value,2));
            auto checkpoint=sqlite3_column_int64(stored.value,1);
            resume=id==std::to_wstring(journal.UsnJournalID) && serial==volume.serial && sqlite3_column_int(stored.value,3)!=0 &&
                checkpoint>=journal.FirstUsn && checkpoint<=journal.NextUsn && wide(sqlite3_column_text(stored.value,4))==L"ntfs";
            if(resume) {volume.journalId=journal.UsnJournalID;volume.nextUsn=checkpoint;volume.phase=NtfsPhase::Journal;}
        }
        if(!resume) beginMft(volume,journal);
        return true;
    }
    void beginMft(Root& volume,const USN_JOURNAL_DATA_V0& journal) {
        volume.phase=NtfsPhase::Enumerate;volume.enumCursor=0;volume.buildStamp=epoch();volume.initialUsn=journal.NextUsn;
        volume.journalId=journal.UsnJournalID;volume.nextUsn=journal.NextUsn;volume.materialFirst=true;volume.clearCache();
        Transaction transaction(database.value);
        Statement reset(database.value,"DELETE FROM ntfs_nodes WHERE root_id=?1");reset.number(1,volume.id);reset.run();
        Statement state(database.value,"UPDATE search_roots SET mode='ntfs',complete=0,serial=?2 WHERE id=?1");state.number(1,volume.id);state.number(2,volume.serial);state.run();
        node(volume.id,volume.rootFrn,volume.rootFrn,L"",true,volume.buildStamp,FILE_ATTRIBUTE_DIRECTORY);
        transaction.commit();
    }
    void directoryFallback(Root& volume) {
        volume.phase=NtfsPhase::Directory;volume.volume.reset();volume.clearCache();
        Statement state(database.value,"UPDATE search_roots SET mode='directory',journal_id=NULL,next_usn=NULL WHERE id=?1");state.number(1,volume.id);state.run();
        schedule(volume.id,volume.path,true);
    }
    std::optional<std::wstring> nodePath(Root& volume,uint64_t file) {
        if(file==volume.rootFrn) return volume.path;
        if(auto cached=volume.pathCache.find(file);cached!=volume.pathCache.end()) return cached->second.first;
        std::vector<std::wstring> components;
        std::vector<uint64_t> visited;
        auto current=file;
        size_t total=volume.path.size();
        DWORD targetAttributes=0;
        std::wstring prefix=volume.path;
        while(current!=volume.rootFrn) {
            if(std::find(visited.begin(),visited.end(),current)!=visited.end()) return std::nullopt;
            visited.push_back(current);
            if(current!=file) if(auto cached=volume.pathCache.find(current);cached!=volume.pathCache.end()) {
                if(cached->second.second&FILE_ATTRIBUTE_REPARSE_POINT) return std::nullopt;
                prefix=cached->second.first;break;
            }
            Statement get(database.value,"SELECT parent_frn,name,attributes FROM ntfs_nodes WHERE root_id=?1 AND frn=?2");
            get.number(1,volume.id);get.number(2,static_cast<int64_t>(current));
            if(!get.row()) return std::nullopt;
            if(current==file) targetAttributes=static_cast<DWORD>(sqlite3_column_int64(get.value,2));
            if(current!=file && (sqlite3_column_int64(get.value,2)&FILE_ATTRIBUTE_REPARSE_POINT)) return std::nullopt;
            auto parent=static_cast<uint64_t>(sqlite3_column_int64(get.value,0));
            auto name=wide(sqlite3_column_text(get.value,1));
            if(name.empty() || parent==current || name.find_first_of(L"\\/")!=std::wstring::npos) return std::nullopt;
            total+=name.size()+1;if(total>32760) return std::nullopt;
            components.push_back(std::move(name));current=parent;
        }
        for(auto component=components.rbegin();component!=components.rend();++component) prefix=join(prefix,*component);
        if(volume.pathCache.size()>=2048 || volume.cacheCharacters+prefix.size()>512*1024) volume.clearCache();
        volume.cacheCharacters+=prefix.size();volume.pathCache.emplace(file,std::make_pair(prefix,targetAttributes));
        return prefix;
    }
    void materialize(Root& volume,uint64_t file,int64_t stamp) {
        if(file==volume.rootFrn) return;
        auto path=nodePath(volume,file);
        if(!path || ownIndexPath(*path)) return;
        Statement metadata(database.value,"SELECT name,attributes,folder FROM ntfs_nodes WHERE root_id=?1 AND frn=?2");metadata.number(1,volume.id);metadata.number(2,static_cast<int64_t>(file));
        if(!metadata.row()) return;
        auto name=wide(sqlite3_column_text(metadata.value,0));auto attributes=static_cast<DWORD>(sqlite3_column_int64(metadata.value,1));
        if(sqlite3_column_int(metadata.value,2)) attributes|=FILE_ATTRIBUTE_DIRECTORY;
        uint64_t size=0;
        bool known=false;
        WIN32_FILE_ATTRIBUTE_DATA actual{};
        if(GetFileAttributesExW(extended(*path).c_str(),GetFileExInfoStandard,&actual)) {attributes=actual.dwFileAttributes;size=(static_cast<uint64_t>(actual.nFileSizeHigh)<<32)|actual.nFileSizeLow;known=true;}
        upsert(database.value,volume.id,*path,name,attributes,size,stamp,file,known);
    }
    void refresh(Root& volume,uint64_t file,int64_t stamp) {
        Statement insert(database.value,"INSERT INTO ntfs_refresh VALUES(?1,?2,?3) ON CONFLICT(root_id,frn) DO UPDATE SET stamp=max(stamp,excluded.stamp)");
        insert.number(1,volume.id);insert.number(2,static_cast<int64_t>(file));insert.number(3,stamp);insert.run();
    }
    bool refreshBatch() {
        Statement queued(database.value,"SELECT root_id,frn,stamp FROM ntfs_refresh ORDER BY root_id,frn LIMIT 128");
        struct Pending {int64_t root,frn,stamp;};std::vector<Pending> batch;
        while(queued.row()) batch.push_back({sqlite3_column_int64(queued.value,0),sqlite3_column_int64(queued.value,1),sqlite3_column_int64(queued.value,2)});
        if(batch.empty()) return false;
        Transaction transaction(database.value);
        for(const auto& item:batch) {
            auto* volume=root(item.root);
            if(volume && volume->phase!=NtfsPhase::Directory) {
                materialize(*volume,static_cast<uint64_t>(item.frn),item.stamp);
                Statement type(database.value,"SELECT attributes FROM ntfs_nodes WHERE root_id=?1 AND frn=?2");type.number(1,item.root);type.number(2,item.frn);
                bool reparseParent=type.row() && (sqlite3_column_int64(type.value,0)&FILE_ATTRIBUTE_REPARSE_POINT);
                if(!reparseParent) {
                    Statement children(database.value,"INSERT OR IGNORE INTO ntfs_refresh SELECT root_id,frn,?3 FROM ntfs_nodes WHERE root_id=?1 AND parent_frn=?2 AND frn<>parent_frn");
                    children.number(1,item.root);children.number(2,item.frn);children.number(3,item.stamp);children.run();
                }
            }
            Statement remove(database.value,"DELETE FROM ntfs_refresh WHERE root_id=?1 AND frn=?2");remove.number(1,item.root);remove.number(2,item.frn);remove.run();
        }
        transaction.commit();return true;
    }
    void hardLinks(Root& volume,uint64_t file,const std::wstring& path,int64_t stamp) {
        std::wstring name(32768,L'\0');DWORD capacity=static_cast<DWORD>(name.size());
        HANDLE links=FindFirstFileNameW(extended(path).c_str(),0,&capacity,name.data());
        if(links==INVALID_HANDLE_VALUE) return;
        do {
            auto linked=volume.path.substr(0,2)+std::wstring(name.c_str());
            if(!ownIndexPath(linked)) {
                WIN32_FILE_ATTRIBUTE_DATA actual{};
                if(GetFileAttributesExW(extended(linked).c_str(),GetFileExInfoStandard,&actual)) upsert(database.value,volume.id,linked,fs::path(linked).filename().wstring(),actual.dwFileAttributes,(static_cast<uint64_t>(actual.nFileSizeHigh)<<32)|actual.nFileSizeLow,stamp,file);
            }
            capacity=static_cast<DWORD>(name.size());
        } while(FindNextFileNameW(links,&capacity,name.data()));
        FindClose(links);
    }
    void journalChange(Root& volume,uint64_t file,uint64_t parent,int64_t,DWORD reason,DWORD attributes,const std::wstring& name) {
        if(file==volume.rootFrn || name.empty()) return;
        auto parentPath=nodePath(volume,parent);
        if(parentPath && ownIndexPath(join(*parentPath,name))) return;
        auto oldPath=nodePath(volume,file);
        auto stamp=epoch();
        if(reason&USN_REASON_FILE_DELETE) {
            if(oldPath) erase(database.value,volume.id,*oldPath);
            Statement removeFiles(database.value,"DELETE FROM files WHERE root_id=?1 AND frn=?2");removeFiles.number(1,volume.id);removeFiles.number(2,static_cast<int64_t>(file));removeFiles.run();
            Statement removeNode(database.value,"DELETE FROM ntfs_nodes WHERE root_id=?1 AND frn=?2");removeNode.number(1,volume.id);removeNode.number(2,static_cast<int64_t>(file));removeNode.run();
            volume.clearCache();return;
        }
        if(reason&USN_REASON_RENAME_OLD_NAME) {if(oldPath) erase(database.value,volume.id,*oldPath);return;}
        node(volume.id,file,parent,name,(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0,stamp,attributes);
        volume.clearCache();
        auto newPath=nodePath(volume,file);
        if(oldPath && newPath && fold(*oldPath)!=fold(*newPath)) erase(database.value,volume.id,*oldPath);
        materialize(volume,file,stamp);
        if((attributes&FILE_ATTRIBUTE_DIRECTORY) && (reason&USN_REASON_RENAME_NEW_NAME) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT)) refresh(volume,file,stamp);
        if(newPath && !(attributes&FILE_ATTRIBUTE_DIRECTORY) && (reason&(USN_REASON_HARD_LINK_CHANGE|USN_REASON_FILE_CREATE|USN_REASON_RENAME_NEW_NAME))) hardLinks(volume,file,*newPath,stamp);
    }
    bool ntfsBatch() {
        bool working=false;
        for(auto& volume:roots) {
            if(volume.phase==NtfsPhase::Directory) continue;
            DWORD length=0;
            if(volume.phase==NtfsPhase::Enumerate) {
                working=true;
                MFT_ENUM_DATA_V0 request{};request.StartFileReferenceNumber=volume.enumCursor;request.LowUsn=0;request.HighUsn=volume.initialUsn;
                if(!DeviceIoControl(volume.volume.value,FSCTL_ENUM_USN_DATA,&request,sizeof(request),journalBuffer.data(),static_cast<DWORD>(journalBuffer.size()),&length,nullptr)) {
                    if(GetLastError()==ERROR_HANDLE_EOF) {volume.phase=NtfsPhase::Materialize;volume.materialFirst=true;}
                    else directoryFallback(volume);
                    continue;
                }
                if(length<sizeof(uint64_t)) {directoryFallback(volume);continue;}
                uint64_t next=0;memcpy(&next,journalBuffer.data(),sizeof(next));
                if(next<=volume.enumCursor) {directoryFallback(volume);continue;}
                std::vector<uint64_t> partial;
                Transaction transaction(database.value);
                bool valid=search_detail::visitUsnRecords(journalBuffer.data()+sizeof(uint64_t),length-sizeof(uint64_t),[&](uint64_t file,uint64_t parent,int64_t,DWORD,DWORD attributes,const std::wstring& name) {
                    node(volume.id,file,parent,name,(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0,volume.buildStamp,attributes);partial.push_back(file);
                });
                if(valid) {
                    volume.clearCache();
                    for(auto file:partial) materialize(volume,file,volume.buildStamp);
                    volume.enumCursor=next;
                }
                transaction.commit();
                if(!valid) directoryFallback(volume);
            } else if(volume.phase==NtfsPhase::Materialize) {
                working=true;
                Statement nodes(database.value,"SELECT frn FROM ntfs_nodes WHERE root_id=?1 AND (?2 IS NULL OR frn>?2) ORDER BY frn LIMIT 256");
                nodes.number(1,volume.id);if(volume.materialFirst) sqlite3_bind_null(nodes.value,2);else nodes.number(2,volume.materialCursor);
                std::vector<uint64_t> pending;while(nodes.row()) pending.push_back(static_cast<uint64_t>(sqlite3_column_int64(nodes.value,0)));
                if(pending.empty()) {
                    volume.phase=NtfsPhase::Journal;volume.nextUsn=volume.initialUsn;
                    // MFT contains one name per file ID. A native supplement
                    // records every hardlink path and accessible-directory scope.
                    schedule(volume.id,volume.path);
                } else {
                    Transaction transaction(database.value);
                    for(auto file:pending) materialize(volume,file,volume.buildStamp);
                    transaction.commit();volume.materialFirst=false;volume.materialCursor=static_cast<int64_t>(pending.back());
                }
            } else if(Clock::now()>=volume.journalTick) {
                volume.journalTick=Clock::now()+std::chrono::milliseconds(750);
                USN_JOURNAL_DATA_V0 journal{};
                if(!DeviceIoControl(volume.volume.value,FSCTL_QUERY_USN_JOURNAL,nullptr,0,&journal,sizeof(journal),&length,nullptr)) {directoryFallback(volume);continue;}
                if(journal.UsnJournalID!=volume.journalId || volume.nextUsn<journal.FirstUsn || volume.nextUsn>journal.NextUsn) {beginMft(volume,journal);working=true;continue;}
                READ_USN_JOURNAL_DATA_V0 request{};request.StartUsn=volume.nextUsn;request.ReasonMask=0xffffffff;request.ReturnOnlyOnClose=FALSE;request.Timeout=0;request.BytesToWaitFor=0;request.UsnJournalID=volume.journalId;
                if(!DeviceIoControl(volume.volume.value,FSCTL_READ_USN_JOURNAL,&request,sizeof(request),journalBuffer.data(),static_cast<DWORD>(journalBuffer.size()),&length,nullptr)) {directoryFallback(volume);continue;}
                if(length<sizeof(USN)) {directoryFallback(volume);continue;}
                int64_t next=0;memcpy(&next,journalBuffer.data(),sizeof(next));
                Transaction transaction(database.value);
                bool valid=search_detail::visitUsnRecords(journalBuffer.data()+sizeof(USN),length-sizeof(USN),[&](uint64_t file,uint64_t parent,int64_t stamp,DWORD reason,DWORD attributes,const std::wstring& name) {journalChange(volume,file,parent,stamp,reason,attributes,name);});
                if(valid) {
                    Statement checkpoint(database.value,"UPDATE search_roots SET journal_id=?2,next_usn=?3,serial=?4,mode='ntfs' WHERE id=?1");
                    checkpoint.number(1,volume.id);checkpoint.text(2,std::to_wstring(volume.journalId));checkpoint.number(3,next);checkpoint.number(4,volume.serial);checkpoint.run();
                    volume.nextUsn=next;
                }
                transaction.commit();
                if(!valid) directoryFallback(volume);
                else if(next<journal.NextUsn) {volume.journalTick=Clock::now();working=true;}
            }
        }
        return working;
    }
    bool scanBatch() {
        if (!active) {
            Statement first(database.value,"SELECT id,root_id,prefix,stamp FROM scan_jobs ORDER BY (id<=?1),id LIMIT 1");first.number(1,lastScanJob);
            if (!first.row()) return false;
            Scan scan; scan.id=sqlite3_column_int64(first.value,0); scan.rootId=sqlite3_column_int64(first.value,1); scan.prefix=wide(sqlite3_column_text(first.value,2)); scan.stamp=sqlite3_column_int64(first.value,3);
            auto retained=pausedScans.find(scan.id);
            if(retained!=pausedScans.end()){active=std::move(retained->second);pausedScans.erase(retained);}else active=std::move(scan);
            lastScanJob=active->id;
        }
        auto& scan=*active;
        Transaction transaction(database.value);
        auto deadline=Clock::now()+std::chrono::milliseconds(60);
        for(size_t count=0;count<512 && Clock::now()<deadline;++count) {
            if (!scan.directory) {
                Statement queued(database.value,"SELECT id,path FROM scan_queue WHERE job_id=?1 ORDER BY id LIMIT 1"); queued.number(1,scan.id);
                if (!queued.row()) {
                    Statement repeat(database.value,"SELECT dirty FROM scan_jobs WHERE id=?1");repeat.number(1,scan.id);
                    bool dirty=repeat.row() && sqlite3_column_int(repeat.value,0)!=0;
                    Statement cleanup(database.value,"DELETE FROM files WHERE root_id=?1 AND seen<?2 AND (path_fold=?3 OR (path_fold>=?4 AND path_fold<?5))");
                    auto prefix=subtreePrefix(scan.prefix),upper=prefix;upper.back()=L']';
                    cleanup.number(1,scan.rootId); cleanup.number(2,scan.stamp); cleanup.text(3,fold(scan.prefix)); cleanup.text(4,prefix); cleanup.text(5,upper); cleanup.run();
                    Statement finish(database.value,"DELETE FROM scan_jobs WHERE id=?1"); finish.number(1,scan.id);finish.run();
                    if(auto* volume=root(scan.rootId);volume && fold(scan.prefix)==fold(volume->path)) {
                        if(volume->phase==NtfsPhase::Directory || volume->phase==NtfsPhase::Journal) {
                            Statement complete(database.value,"UPDATE search_roots SET complete=1 WHERE id=?1");complete.number(1,scan.rootId);complete.run();
                        }
                        Statement oldNodes(database.value,"DELETE FROM ntfs_nodes WHERE root_id=?1 AND seen<?2 AND frn<>?3 AND NOT EXISTS(SELECT 1 FROM files WHERE files.root_id=ntfs_nodes.root_id AND files.frn=ntfs_nodes.frn)");
                        oldNodes.number(1,scan.rootId);oldNodes.number(2,scan.stamp);oldNodes.number(3,static_cast<int64_t>(volume->rootFrn));oldNodes.run();
                    }
                    if(dirty) schedule(scan.rootId,scan.prefix);
                    transaction.commit();active.reset();return true;
                }
                scan.queueId=sqlite3_column_int64(queued.value,0);scan.path=wide(sqlite3_column_text(queued.value,1));
                // A directory can turn into a junction after being queued by
                // FILE_ACTION_ADDED. Recheck ancestors at dequeue time.
                if(reparseAncestor(scan.rootId,scan.path)) {
                    ++reparse;eraseDescendants(database.value,scan.rootId,scan.path);
                    Statement remove(database.value,"DELETE FROM scan_queue WHERE id=?1");remove.number(1,scan.queueId);remove.run();continue;
                }
                scan.directory=std::make_unique<DirectoryReader>(scan.path);
            }
            DirectoryEntry item;
            if (!scan.directory->next(item)) {
                auto failure=scan.directory->error;
                const bool rootUnavailable=fold(scan.path)==fold(scan.prefix) &&
                    (failure==ERROR_FILE_NOT_FOUND || failure==ERROR_PATH_NOT_FOUND || failure==ERROR_NOT_READY);
                if(rootUnavailable || (failure!=ERROR_NO_MORE_FILES && failure!=ERROR_FILE_NOT_FOUND && failure!=ERROR_PATH_NOT_FOUND && failure!=0)) {
                    ++inaccessible;protect(database.value,scan.rootId,scan.path,scan.stamp);
                }
                Statement remove(database.value,"DELETE FROM scan_queue WHERE id=?1");remove.number(1,scan.queueId);remove.run();
                scan.directory.reset();continue;
            }
            if(item.name==L"." || item.name==L"..") continue;
            auto path=join(scan.path,item.name);
            if (ownIndexPath(path)) {erase(database.value,scan.rootId,path);continue;}
            WIN32_FILE_ATTRIBUTE_DATA current{};
            if(GetFileAttributesExW(extended(path).c_str(),GetFileExInfoStandard,&current)) {
                item.attributes=current.dwFileAttributes;item.size=(static_cast<uint64_t>(current.nFileSizeHigh)<<32)|current.nFileSizeLow;
            } else {
                DWORD failure=GetLastError();
                if(failure==ERROR_FILE_NOT_FOUND || failure==ERROR_PATH_NOT_FOUND) continue;
                // Directory enumeration already supplied metadata. A later
                // attribute denial must not hide the visible entry itself.
            }
            upsert(database.value,scan.rootId,path,item.name,item.attributes,item.size,scan.stamp,item.frn);
            if(auto* volume=root(scan.rootId);volume && volume->phase!=NtfsPhase::Directory) {
                node(scan.rootId,item.frn,scan.directory->parentFrn,item.name,(item.attributes&FILE_ATTRIBUTE_DIRECTORY)!=0,scan.stamp,item.attributes);
                volume->clearCache();
            }
            if(item.attributes&FILE_ATTRIBUTE_DIRECTORY) {
                if(item.attributes&FILE_ATTRIBUTE_REPARSE_POINT) ++reparse;
                else enqueue(scan.id,path);
            }
        }
        transaction.commit();
        // Preserve directory-reader positions while rotating disk roots. A
        // large C: scan must not postpone D:/E: results until its completion.
        if(pausedScans.size()<3){pausedScans.emplace(scan.id,std::move(*active));active.reset();}
        return true;
    }
    void changed(int64_t rootId,const std::wstring& path,DWORD action) {
        if(ownIndexPath(path)) return;
        if(action==FILE_ACTION_REMOVED || action==FILE_ACTION_RENAMED_OLD_NAME) { erase(database.value,rootId,path);return; }
        WIN32_FILE_ATTRIBUTE_DATA information{};
        if(!GetFileAttributesExW(extended(path).c_str(),GetFileExInfoStandard,&information)) {
            DWORD error=GetLastError();
            if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) erase(database.value,rootId,path);
            return;
        }
        auto name=fs::path(path).filename().wstring();
        if((information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && (information.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            eraseDescendants(database.value,rootId,path);
            if(active && active->rootId==rootId && active->directory && under(active->path,path)) active->directory.reset();
        }
        upsert(database.value,rootId,path,name,information.dwFileAttributes,(static_cast<uint64_t>(information.nFileSizeHigh)<<32)|information.nFileSizeLow,epoch());
        if((action==FILE_ACTION_ADDED || action==FILE_ACTION_RENAMED_NEW_NAME) && (information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(information.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) schedule(rootId,path);
    }
    void notifications() {
        for(auto& watcher:watchers) {
            if(!watcher->directory) {
                if(Clock::now()>=watcher->retry) {
                    watcher->open();
                    if(watcher->directory) schedule(watcher->rootId,watcher->path,true);
                }
                continue;
            }
            if(!watcher->pending || WaitForSingleObject(watcher->event.value,0)!=WAIT_OBJECT_0) continue;
            DWORD length=0;
            BOOL success=GetOverlappedResult(watcher->directory.value,&watcher->operation,&length,FALSE);
            DWORD error=success ? ERROR_SUCCESS : GetLastError();
            watcher->pending=false;
            std::vector<unsigned char> delivered;
            if(success && length) delivered.assign(watcher->buffer.begin(),watcher->buffer.begin()+length);
            watcher->arm();
            Transaction transaction(database.value);
            if(!success || !length) {
                ++overflows;
                schedule(watcher->rootId,watcher->path,true);
                if(error==ERROR_ACCESS_DENIED || error==ERROR_PATH_NOT_FOUND || error==ERROR_FILE_NOT_FOUND || error==ERROR_INVALID_HANDLE) watcher->close();
            } else {
                size_t offset=0;
                while(offset+offsetof(FILE_NOTIFY_INFORMATION,FileName)<=delivered.size()) {
                    auto* item=reinterpret_cast<FILE_NOTIFY_INFORMATION*>(delivered.data()+offset);
                    if(offset+offsetof(FILE_NOTIFY_INFORMATION,FileName)+item->FileNameLength>delivered.size()) {++overflows;schedule(watcher->rootId,watcher->path,true);break;}
                    changed(watcher->rootId,join(watcher->path,std::wstring(item->FileName,item->FileNameLength/sizeof(wchar_t))),item->Action);
                    if(!item->NextEntryOffset) break;
                    if(item->NextEntryOffset>delivered.size()-offset) {++overflows;schedule(watcher->rootId,watcher->path,true);break;}
                    offset+=item->NextEntryOffset;
                }
            }
            transaction.commit();
        }
    }
    void publish() {
        Transaction transaction(database.value);
        meta(database.value,"pid",std::to_wstring(GetCurrentProcessId()));
        meta(database.value,"building",scanning()?L"1":L"0");
        size_t ntfs=0;for(const auto& volume:roots) if(volume.phase!=NtfsPhase::Directory) ++ntfs;
        std::wstring message=ntfs ? L"NTFS MFT / USN 模式 · "+std::to_wstring(ntfs)+L" 个卷" : L"普通目录模式";
        if(ntfs && ntfs<roots.size()) message+=L" · "+std::to_wstring(roots.size()-ntfs)+L" 个普通目录";
        if(!ntfs) message+=L" · "+std::to_wstring(roots.size())+L" 个根目录";
        if(scanning()) message+=L" · 分批校准中，已有结果可用";
        else message+=L" · 实时目录通知";
        if(inaccessible) message+=L" · 无权限目录 "+std::to_wstring(inaccessible);
        if(reparse) message+=L" · 跳过重解析目录 "+std::to_wstring(reparse);
        if(overflows) message+=L" · 通知溢出校准 "+std::to_wstring(overflows);
        Statement grams(database.value,"SELECT 1 FROM search_meta WHERE key='short_complete' AND value='0'");if(grams.row()) message+=L" · 短词索引分批补全中";
        size_t offline=0;for(const auto& watcher:watchers) if(!watcher->directory) ++offline;
        if(offline) message+=L" · "+std::to_wstring(offline)+L" 个根目录不可监听，等待恢复";
        message+=L" · 排除本工具索引文件及侧文件";
        meta(database.value,"message",message);transaction.commit();
    }
};
HANDLE controlStop=nullptr;
BOOL WINAPI consoleControl(DWORD control) {
    if(controlStop && (control==CTRL_C_EVENT || control==CTRL_BREAK_EVENT || control==CTRL_CLOSE_EVENT || control==CTRL_SHUTDOWN_EVENT)) {SetEvent(controlStop);return TRUE;}
    return FALSE;
}
HANDLE createControllerEvent(const std::wstring& name) {
    HANDLE rawToken=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&rawToken)) return nullptr;
    Handle token(rawToken);
    DWORD length=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&length);
    if(!length || length>64*1024) return nullptr;
    std::vector<unsigned char> buffer(length);
    if(!GetTokenInformation(token.value,TokenUser,buffer.data(),length,&length)) return nullptr;
    LPWSTR sid=nullptr;
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid)) return nullptr;
    // Same-user medium-IL UI can stop an elevated worker. Other users cannot
    // control it, and no filesystem ACL or global privilege is changed.
    std::wstring descriptor=L"O:"+std::wstring(sid)+L"D:(A;;GA;;;"+sid+L")(A;;GA;;;SY)S:(ML;;NW;;;ME)";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR security=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(),SDDL_REVISION_1,&security,nullptr)) return nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),security,FALSE};
    HANDLE event=CreateEventW(&attributes,TRUE,FALSE,name.c_str());
    LocalFree(security);return event;
}
}

namespace search_detail {
bool backfillShortGrams(sqlite3* database,size_t limit) {
    Statement state(database,"SELECT key,value FROM search_meta WHERE key IN('short_complete','short_cursor')");
    bool done=false;int64_t cursor=0;
    while(state.row()) {
        auto key=std::string(reinterpret_cast<const char*>(sqlite3_column_text(state.value,0)));
        if(key=="short_complete") done=sqlite3_column_int(state.value,1)!=0;
        else cursor=sqlite3_column_int64(state.value,1);
    }
    if(done) return false;
    std::vector<int64_t> pending;
    Statement query(database,("SELECT id FROM files WHERE id>?1 AND short_ready=0 ORDER BY id LIMIT "+std::to_string(std::min<size_t>(limit,1024))).c_str());query.number(1,cursor);
    while(query.row()) pending.push_back(sqlite3_column_int64(query.value,0));
    if(pending.empty()) {meta(database,"short_complete",L"1");return false;}
    Transaction transaction(database);
    Statement insert(database,"INSERT INTO files_short(rowid,grams) SELECT id,desk_shortgrams(name_fold) FROM files WHERE id=?1 AND short_ready=0");
    Statement complete(database,"UPDATE files SET short_ready=1 WHERE id=?1");
    for(auto id:pending) {
        insert.number(1,id);insert.run();sqlite3_reset(insert.value);sqlite3_clear_bindings(insert.value);
        complete.number(1,id);complete.run();sqlite3_reset(complete.value);sqlite3_clear_bindings(complete.value);
    }
    meta(database,"short_cursor",std::to_wstring(pending.back()));
    transaction.commit();return true;
}
bool prepareSortIndexes(sqlite3* database,size_t limit) {
    auto flag=[&](const char* key) {Statement value(database,"SELECT value FROM search_meta WHERE key=?1");value.bytes(1,key);return value.row() && sqlite3_column_int(value.value,0)!=0;};
    for(const auto& entry:std::vector<std::pair<const char*,const char*>>{
        {"sort_name","CREATE INDEX IF NOT EXISTS files_sort_name ON files(name_fold,id)"},
        {"sort_path","CREATE INDEX IF NOT EXISTS files_sort_path ON files(path_fold,id)"},
        {"sort_size","CREATE INDEX IF NOT EXISTS files_sort_size_asc ON files(size_known DESC,(CASE WHEN size_known=1 THEN size ELSE 0 END),id); CREATE INDEX IF NOT EXISTS files_sort_size_desc ON files(size_known DESC,(CASE WHEN size_known=1 THEN size ELSE 0 END) DESC,id DESC)"}}) {
        if(!flag(entry.first)) {Transaction transaction(database);execute(database,entry.second);meta(database,entry.first,L"1");transaction.commit();return true;}
    }
    if(!flag("type_complete")) {
        Statement position(database,"SELECT value FROM search_meta WHERE key='type_cursor'");position.row();int64_t cursor=sqlite3_column_int64(position.value,0);
        Statement rows(database,("SELECT id FROM files WHERE id>?1 AND type_ready=0 ORDER BY id LIMIT "+std::to_string(std::min<size_t>(limit,1024))).c_str());rows.number(1,cursor);
        std::vector<int64_t> ids;while(rows.row()) ids.push_back(sqlite3_column_int64(rows.value,0));
        Transaction transaction(database);
        if(ids.empty()) meta(database,"type_complete",L"1");
        else {
            Statement update(database,"UPDATE files SET type_key=desk_typekey(name_fold,folder),type_ready=1 WHERE id=?1");
            for(auto id:ids) {update.number(1,id);update.run();sqlite3_reset(update.value);sqlite3_clear_bindings(update.value);}
            meta(database,"type_cursor",std::to_wstring(ids.back()));
        }
        transaction.commit();return true;
    }
    if(!flag("sort_type")) {Transaction transaction(database);execute(database,"CREATE INDEX IF NOT EXISTS files_sort_type ON files(type_key,id)");meta(database,"sort_type",L"1");transaction.commit();return true;}
    return false;
}
bool backfillFileSizes(sqlite3* database,size_t limit) {
    Statement complete(database,"SELECT value FROM search_meta WHERE key='size_complete'");if(complete.row() && sqlite3_column_int(complete.value,0)!=0) return false;
    Statement position(database,"SELECT value FROM search_meta WHERE key='size_cursor'");position.row();auto cursor=sqlite3_column_int64(position.value,0);
    Statement rows(database,("SELECT id,path FROM files WHERE id>?1 AND folder=0 AND size_known=0 ORDER BY id LIMIT "+std::to_string(std::min<size_t>(limit,128))).c_str());rows.number(1,cursor);
    std::vector<std::pair<int64_t,std::wstring>> pending;
    while(rows.row()) pending.emplace_back(sqlite3_column_int64(rows.value,0),wide(sqlite3_column_text(rows.value,1)));
    Transaction transaction(database);
    if(pending.empty()) {
        Statement unknown(database,"SELECT count(*) FROM files WHERE folder=0 AND size_known=0");unknown.row();
        meta(database,"size_unknown",std::to_wstring(sqlite3_column_int64(unknown.value,0)));meta(database,"size_complete",L"1");
    } else {
        Statement update(database,"UPDATE files SET size=?2,size_known=1 WHERE id=?1");
        for(const auto& entry:pending) {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if(GetFileAttributesExW(extended(entry.second).c_str(),GetFileExInfoStandard,&attributes) && !(attributes.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {
                update.number(1,entry.first);update.number(2,static_cast<int64_t>((static_cast<uint64_t>(attributes.nFileSizeHigh)<<32)|attributes.nFileSizeLow));update.run();sqlite3_reset(update.value);sqlite3_clear_bindings(update.value);
            }
        }
        meta(database,"size_cursor",std::to_wstring(pending.back().first));
    }
    transaction.commit();return true;
}
}

namespace {
struct BoundValue {std::wstring text;int64_t number=0;bool numeric=false;};
struct Bindings {
    std::vector<BoundValue> values;
    std::string text(const std::wstring& value) {values.push_back({value,0,false});return "?"+std::to_string(values.size());}
    std::string number(int64_t value) {values.push_back({{},value,true});return "?"+std::to_string(values.size());}
    void bind(Statement& statement) const {for(size_t i=0;i<values.size();++i) if(values[i].numeric) statement.number(static_cast<int>(i+1),values[i].number);else statement.text(static_cast<int>(i+1),values[i].text);}
};
bool metaFlag(sqlite3* db,const char* key) {Statement flag(db,"SELECT value FROM search_meta WHERE key=?1");flag.bytes(1,key);return flag.row() && sqlite3_column_int(flag.value,0)!=0;}
std::wstring typeKey(const SearchItem& item) {
    if(item.folder) return L"0folder";
    auto name=fold(item.name);
    auto dot=name.rfind(L'.');
    return L"1:"+(dot==std::wstring::npos || dot==0 ? std::wstring{} : name.substr(dot+1));
}
struct SortCursor {bool valid=false;int64_t id=0,value=0;bool known=false;std::wstring text;};
struct SortInfo {
    SearchSort kind;
    bool descending;
    std::string key,index,metaKey,order;
    SortInfo(const SearchQuery& query,bool typeComplete) : kind(query.sort),descending(query.descending) {
        auto direction=descending ? " DESC" : " ASC";
        switch(kind) {
        case SearchSort::Name:key="f.name_fold";index="files_sort_name";metaKey="sort_name";break;
        case SearchSort::Path:key="f.path_fold";index="files_sort_path";metaKey="sort_path";break;
        case SearchSort::Size:key="(CASE WHEN f.size_known=1 THEN f.size ELSE 0 END)";index=descending ? "files_sort_size_desc" : "files_sort_size_asc";metaKey="sort_size";break;
        case SearchSort::Type:key=typeComplete ? "f.type_key" : "desk_typekey(f.name_fold,f.folder)";index="files_sort_type";metaKey="sort_type";break;
        default:key="f.id";break;
        }
        order=(kind==SearchSort::Size ? std::string("f.size_known DESC,") : std::string{})+key+direction+(kind==SearchSort::Id ? std::string{} : ",f.id"+std::string(direction));
    }
    SortCursor cursor(const SearchItem& item) const {
        SortCursor result;result.valid=item.id>0;result.id=item.id;
        switch(kind) {
        case SearchSort::Name:result.text=fold(item.name);break;
        case SearchSort::Path:result.text=fold(item.path);break;
        case SearchSort::Type:result.text=typeKey(item);break;
        case SearchSort::Size:result.known=item.sizeKnown && !item.folder;result.value=result.known ? static_cast<int64_t>(item.size) : 0;break;
        default:result.value=item.id;break;
        }
        return result;
    }
    std::string after(const SortCursor& cursor,Bindings& binds,bool inclusive=false,bool reverse=false,bool withinGroup=false) const {
        if(!cursor.valid) return "1";
        bool down=descending!=reverse;
        auto comparison=down ? "<" : ">";
        auto idComparison=std::string(comparison)+(inclusive ? "=" : "");
        auto id=binds.number(cursor.id);
        if(kind==SearchSort::Id) return "f.id"+idComparison+id;
        auto value=kind==SearchSort::Size ? binds.number(cursor.value) : binds.text(cursor.text);
        std::string condition="("+key+",f.id)"+idComparison+"("+value+","+id+")";
        if(kind==SearchSort::Size && !withinGroup) {
            auto known=binds.number(cursor.known ? 1 : 0);
            condition="(f.size_known"+std::string(reverse ? ">" : "<")+known+" OR (f.size_known="+known+" AND "+condition+"))";
        }
        return condition;
    }
    std::string reversedOrder() const {
        auto direction=descending ? " ASC" : " DESC";
        return (kind==SearchSort::Size ? std::string("f.size_known ASC,") : std::string{})+key+direction+(kind==SearchSort::Id ? std::string{} : ",f.id"+std::string(direction));
    }
};
struct ParsedSearch {Bindings binds;std::string where="1";std::wstring fts,shortFts,error;bool regex=false;};
std::wstring escapedGlob(const std::wstring& pattern) {std::wstring result;for(auto ch:pattern) result+=ch==L'[' ? L"[[]" : std::wstring(1,ch);return result;}
std::vector<std::wstring> orGroups(const std::wstring& text) {
    std::vector<std::wstring> groups;std::wstring current;bool quote=false;
    for(auto ch:text) {if(ch==L'"') quote=!quote;if(ch==L'|' && !quote) {groups.push_back(current);current.clear();} else current+=ch;}
    groups.push_back(current);return groups;
}
std::wstring regexLiteral(const BasicRegex& regex) {
    std::wstring longest,current;
    for(const auto& atom:regex.atoms) {
        if(atom.literal && !atom.repeat && !atom.any && atom.ranges.empty()) current+=atom.literal;
        else {if(codepoints(current)>codepoints(longest)) longest=current;current.clear();}
    }
    if(codepoints(current)>codepoints(longest)) longest=current;return longest;
}
ParsedSearch parseSearch(const SearchQuery& query,bool typeComplete) {
    ParsedSearch result;auto groups=orGroups(query.text);std::vector<std::string> alternatives;
    const std::string type=typeComplete ? "f.type_key" : "desk_typekey(f.name_fold,f.folder)";
    auto extensions=[&](std::wstring list) {
        std::vector<std::wstring> parts;std::wstring part;
        for(auto ch:list) {if(ch==L';' || ch==L',') {if(!part.empty()) parts.push_back(part);part.clear();}else part+=ch;}
        if(!part.empty()) parts.push_back(part);
        std::string condition="f.folder=0 AND (";
        for(auto extension:parts) {
            extension=fold(extension);if(!extension.empty() && extension.front()==L'.') extension.erase(0,1);
            if(condition.back()!=L'(') condition+=" OR ";
            if(extension.find_first_of(L"*?")!=std::wstring::npos) condition+=type+" GLOB "+result.binds.text(L"1:"+escapedGlob(extension));
            else condition+=type+"="+result.binds.text(L"1:"+extension);
        }
        return parts.empty() ? std::string("0") : condition+")";
    };
    auto preset=[&](const std::wstring& value)->std::optional<std::wstring> {
        if(value==L"pic" || value==L"image" || value==L"images") return L"jpg;jpeg;png;bmp;gif;webp;tif;tiff;ico;avif;heic;svg";
        if(value==L"audio") return L"mp3;wav;flac;aac;ogg;m4a;wma;opus;aiff;ape";
        if(value==L"video") return L"mp4;mkv;avi;mov;wmv;webm;m4v;mpg;mpeg;ts;flv";
        if(value==L"doc" || value==L"document" || value==L"documents") return L"pdf;doc;docx;xls;xlsx;ppt;pptx;txt;rtf;md;odt;ods;odp;csv;html;htm";
        if(value==L"exe" || value==L"executable") return L"exe;msi;bat;cmd;ps1;com";
        if(value==L"archive" || value==L"compressed") return L"zip;rar;7z;gz;tar;bz2;xz";
        return std::nullopt;
    };
    auto hint=[&](const std::wstring& literal,bool path,bool negated) {
        if(negated || groups.size()!=1 || literal.empty()) return;
        auto normalized=fold(literal);
        if(codepoints(normalized)>=3) {if(!result.fts.empty()) result.fts+=L" AND ";result.fts+=(path ? L"path_fold:" : L"name_fold:")+ftsQuote(normalized);}
        else if(!path) {auto gram=search_detail::shortQueryToken(normalized);if(!gram.empty()) {if(!result.shortFts.empty()) result.shortFts+=L" AND ";result.shortFts+=ftsQuote(std::wstring(gram.begin(),gram.end()));}}
    };
    for(const auto& group:groups) {
        std::vector<std::string> terms;
        for(auto token:tokens(group)) {
            bool negate=false;while(!token.empty() && token.front()==L'!') {negate=!negate;token.erase(0,1);}if(token.empty()) continue;
            auto colon=token.find(L':');auto command=colon==std::wstring::npos ? std::wstring{} : fold(token.substr(0,colon));
            auto value=colon==std::wstring::npos ? token : token.substr(colon+1);
            std::string condition;
            bool handled=true;
            if(command==L"ext") condition=extensions(value);
            else if(command==L"type") {
                auto kind=fold(value);
                if(kind==L"folder" || kind==L"folders") condition="f.folder=1";
                else if(kind==L"file" || kind==L"files") condition="f.folder=0";
                else if(auto list=preset(kind)) condition=extensions(*list);
                else condition=extensions(kind);
            } else if(command==L"file" || command==L"folder") {
                condition=command==L"folder" ? "f.folder=1" : "f.folder=0";
                if(!value.empty()) {
                    auto text=query.matchCase ? value : fold(value);auto field=query.matchPath ? (query.matchCase ? "f.path" : "f.path_fold") : (query.matchCase ? "f.name" : "f.name_fold");
                    condition+=" AND instr("+std::string(field)+","+result.binds.text(text)+")>0";hint(value,query.matchPath,negate);
                }
            } else if(auto list=preset(command);list && value.empty()) condition=extensions(*list);
            else if(command==L"in") {
                try {auto path=fold(normalize(value));auto prefix=subtreePrefix(path),upper=prefix;upper.back()=L']';condition="(f.path_fold="+result.binds.text(path)+" OR (f.path_fold>="+result.binds.text(prefix)+" AND f.path_fold<"+result.binds.text(upper)+"))";}
                catch(...) {result.error=L"限定目录路径无效";return result;}
            } else if(command==L"regex") {
                auto pattern=query.matchCase ? value : fold(value);BasicRegex regex(pattern);
                if(!regex.valid) {result.error=L"正则支持字符、.、[]、*+? 与 ^/$；不支持分组、分支或反向引用";return result;}
                auto field=query.matchPath ? (query.matchCase ? "f.path" : "f.path_fold") : (query.matchCase ? "f.name" : "f.name_fold");
                condition="desk_regex("+result.binds.text(pattern)+","+field+")";result.regex=true;hint(regexLiteral(regex),query.matchPath,negate);
            } else handled=false;
            if(!handled) {
                bool path=command==L"path" || (command!=L"name" && (query.matchPath || token.find_first_of(L"\\/")!=std::wstring::npos || (token.size()>=2 && token[1]==L':' && iswalpha(token[0]))));
                auto text=(command==L"path" || command==L"name") ? value : token;
                bool wildcard=text.find_first_of(L"*?")!=std::wstring::npos;
                auto literal=wildcard ? longestLiteral(text) : text;
                auto normalized=query.matchCase ? text : fold(text);auto field=path ? (query.matchCase ? "f.path" : "f.path_fold") : (query.matchCase ? "f.name" : "f.name_fold");
                condition=wildcard ? std::string(field)+" GLOB "+result.binds.text(escapedGlob(normalized)) : "instr("+std::string(field)+","+result.binds.text(normalized)+")>0";
                hint(literal,path,negate);
            }
            terms.push_back(negate ? "NOT ("+condition+")" : "("+condition+")");
        }
        std::string condition;
        for(const auto& term:terms) {if(!condition.empty()) condition+=" AND ";condition+=term;}
        alternatives.push_back(condition.empty() ? "1" : condition);
    }
    result.where.clear();for(const auto& alternative:alternatives) {if(!result.where.empty()) result.where+=" OR ";result.where+="("+alternative+")";}
    return result;
}
bool sameItem(const std::optional<SearchItem>& a,const std::optional<SearchItem>& b) {
    if(a.has_value()!=b.has_value()) return false;if(!a) return true;
    return a->id==b->id && a->name==b->name && a->path==b->path && a->size==b->size && a->folder==b->folder && a->sizeKnown==b->sizeKnown;
}
bool sameQuery(const SearchQuery& a,const SearchQuery& b) {return a.text==b.text && a.sort==b.sort && a.descending==b.descending && a.matchCase==b.matchCase && a.matchPath==b.matchPath && sameItem(a.after,b.after);}
SearchItem itemRow(sqlite3_stmt* row) {
    SearchItem item;item.id=sqlite3_column_int64(row,0);item.name=wide(sqlite3_column_text(row,1));item.path=wide(sqlite3_column_text(row,2));item.folder=sqlite3_column_int(row,3)!=0;item.size=static_cast<uint64_t>(sqlite3_column_int64(row,4));item.sizeKnown=!item.folder && sqlite3_column_int(row,5)!=0;return item;
}
constexpr const char* itemProjection="f.id,f.name,f.path,f.folder,f.size,f.size_known";
}

struct SearchStore::Impl {
    fs::path directory;
    std::unique_ptr<Database> database;
    std::timed_mutex mutex;
    std::atomic<uint64_t> sequence=0;
    SearchStatus cached;
    std::mutex cachedMutex;
    std::wstring queryMessage;
    std::atomic<bool> pending=false;
    std::wstring continuationText;
    size_t continuationLimit=0,chunkSize=8192;
    int64_t continuationBeforeId=0;
    int64_t continuationCursor=0;
    std::vector<SearchItem> continuationResults;
    bool sortedActive=false,sortedScan=false;
    SearchQuery sortedQuery;
    size_t sortedLimit=0,sortedChunk=2048;
    int sortedSizeGroup=1;
    int64_t sortedVersion=0;
    SortCursor sortedCursor;
    std::vector<SearchItem> sortedResults;
    explicit Impl(const fs::path& dataDirectory):directory(dataDirectory) {
        try {database=std::make_unique<Database>(directory,2048,true);sqlite3_busy_timeout(database->value,80);}
        catch(const std::exception& error) {cached.message=L"文件索引无法打开："+wide(reinterpret_cast<const unsigned char*>(error.what()));}
    }
};
SearchStore::SearchStore(const fs::path& directory) : impl_(std::make_unique<Impl>(directory)) {}
SearchStore::~SearchStore() = default;
bool SearchStore::queryPending() const {return impl_->pending.load();}
std::vector<SearchItem> SearchStore::query(const SearchQuery& spec,size_t limit) {
    const auto request=++impl_->sequence;
    std::unique_lock<std::timed_mutex> lock(impl_->mutex,std::defer_lock);
    if(!lock.try_lock_for(std::chrono::milliseconds(250)) || impl_->sequence.load()!=request) return {};
    const bool wasPending=impl_->pending.exchange(false);
    impl_->queryMessage.clear();
    std::vector<SearchItem> result;
    limit=std::min<size_t>(limit,1000);
    if(!limit)return result;
    if(!impl_->database){try{impl_->database=std::make_unique<Database>(impl_->directory,2048,true);}catch(const std::exception& e){impl_->queryMessage=L"文件索引暂不可用："+wide(reinterpret_cast<const unsigned char*>(e.what()));return result;}}
    if(spec.text.size()>4096) {impl_->queryMessage=L"搜索输入最长 4096 字符";return result;}
    sqlite3* db=impl_->database->value;
    struct ReadSnapshot {
        sqlite3* db;explicit ReadSnapshot(sqlite3* database):db(database) {execute(db,"BEGIN");}
        ~ReadSnapshot() {sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);}
    };
    std::optional<ReadSnapshot> snapshot;
    try {snapshot.emplace(db);} catch(const std::exception& error) {impl_->queryMessage=L"索引查询暂不可用："+wide(reinterpret_cast<const unsigned char*>(error.what()));return {};}
    int64_t version=0;
    {Statement state(db,"SELECT value FROM search_meta WHERE key='files_revision'");state.row();version=sqlite3_column_int64(state.value,0);}
    const bool continuing=wasPending && impl_->sortedActive && impl_->sortedLimit==limit && impl_->sortedVersion==version && sameQuery(impl_->sortedQuery,spec);
    const bool typeComplete=metaFlag(db,"type_complete");
    SortInfo sort(spec,typeComplete);
    auto parsed=parseSearch(spec,typeComplete);
    if(!parsed.error.empty()) {impl_->queryMessage=parsed.error;impl_->sortedActive=false;return {};}
    if(!continuing) {
        impl_->sortedQuery=spec;impl_->sortedLimit=limit;impl_->sortedVersion=version;
        impl_->sortedCursor=spec.after ? sort.cursor(*spec.after) : SortCursor{};
        impl_->sortedSizeGroup=spec.after && !impl_->sortedCursor.known ? 0 : 1;
        impl_->sortedResults.clear();impl_->sortedScan=false;impl_->sortedChunk=parsed.regex ? 64 : 2048;
    }
    impl_->sortedActive=true;
    impl_->continuationResults.clear();
    result=impl_->sortedResults;
    QueryBudget budget{Clock::now()+std::chrono::milliseconds(180),&impl_->sequence,request};
    sqlite3_progress_handler(db,512,progress,&budget);
    sqlite3_create_function_v2(db,"desk_regex",2,SQLITE_UTF8,&budget,regexFunction,nullptr,nullptr,nullptr);
    bool exact=false;
    try {
        auto collect=[&](Statement& statement) {
            while(statement.row()) {
                auto item=itemRow(statement.value);
                if(std::none_of(result.begin(),result.end(),[&](const SearchItem& other){return other.id==item.id;})) result.push_back(std::move(item));
            }
        };
        // A complete small FTS candidate set can be globally sorted directly.
        // Wide searches scan the requested disk index instead of sorting a page.
        if(!impl_->sortedScan && !continuing) {
            std::string candidateTable;
            std::wstring candidateText;
            if(!parsed.fts.empty()) {candidateTable="files_fts";candidateText=parsed.fts;}
            else if(!parsed.shortFts.empty() && metaFlag(db,"short_complete")) {candidateTable="files_short";candidateText=parsed.shortFts;}
            if(!candidateTable.empty()) {
                std::string candidateSql="SELECT rowid FROM "+candidateTable+" WHERE "+candidateTable+" MATCH ?1";
                bool intersection=candidateTable=="files_fts"&&!parsed.shortFts.empty()&&metaFlag(db,"short_complete");
                if(intersection)candidateSql+=" AND rowid IN(SELECT rowid FROM files_short WHERE files_short MATCH ?2)";
                candidateSql+=" LIMIT 8193";
                Statement candidates(db,candidateSql.c_str());candidates.text(1,candidateText);if(intersection)candidates.text(2,parsed.shortFts);
                std::vector<int64_t> ids;while(candidates.row()) ids.push_back(sqlite3_column_int64(candidates.value,0));
                if(ids.size()<=8192) {
                    exact=true;
                    if(!ids.empty()) {
                        auto binds=parsed.binds;std::string idList;
                        for(auto id:ids) {if(!idList.empty()) idList+=",";idList+=binds.number(id);}
                        auto cursor=spec.after ? sort.after(sort.cursor(*spec.after),binds) : "1";
                        Statement statement(db,("SELECT "+std::string(itemProjection)+" FROM files f WHERE ("+parsed.where+") AND f.id IN("+idList+") AND "+cursor+" ORDER BY "+sort.order+" LIMIT "+std::to_string(limit)).c_str());
                        binds.bind(statement);collect(statement);
                    }
                }
            }
            impl_->sortedScan=true;
        }
        if(!exact) {
            if(!sort.metaKey.empty() && !metaFlag(db,sort.metaKey.c_str())) {
                impl_->pending=true;
                impl_->queryMessage=L"排序索引正在后台准备；已有磁盘索引保留，准备完成后结果会显示";
            } else {
                auto table=std::string("files f")+(sort.index.empty() ? "" : " INDEXED BY "+sort.index);
                while(result.size()<limit) {
                    if(progress(&budget)) {impl_->pending=true;break;}
                    Bindings boundaryBindings;
                    auto start=sort.after(impl_->sortedCursor,boundaryBindings,false,false,sort.kind==SearchSort::Size);
                    if(sort.kind==SearchSort::Size) start="f.size_known="+boundaryBindings.number(impl_->sortedSizeGroup)+" AND "+start;
                    auto projection=sort.key+",f.id,f.size_known";
                    Statement boundary(db,("SELECT "+projection+" FROM "+table+" WHERE "+start+" ORDER BY "+sort.order+" LIMIT 1 OFFSET "+std::to_string(impl_->sortedChunk-1)).c_str());boundaryBindings.bind(boundary);
                    SortCursor end;
                    auto readCursor=[&](sqlite3_stmt* row) {
                        SortCursor value;value.valid=true;value.id=sqlite3_column_int64(row,1);value.known=sqlite3_column_int(row,2)!=0;
                        if(sort.kind==SearchSort::Size || sort.kind==SearchSort::Id) value.value=sqlite3_column_int64(row,0);else value.text=wide(sqlite3_column_text(row,0));return value;
                    };
                    if(boundary.row()) end=readCursor(boundary.value);
                    else {
                        Statement last(db,("SELECT "+projection+" FROM "+table+" WHERE "+start+" ORDER BY "+sort.reversedOrder()+" LIMIT 1").c_str());boundaryBindings.bind(last);
                        if(!last.row()) {
                            if(sort.kind==SearchSort::Size && impl_->sortedSizeGroup==1) {impl_->sortedSizeGroup=0;impl_->sortedCursor={};continue;}
                            break;
                        }
                        end=readCursor(last.value);
                    }
                    auto binds=parsed.binds;auto lower=sort.after(impl_->sortedCursor,binds,false,false,sort.kind==SearchSort::Size),upper=sort.after(end,binds,true,true,sort.kind==SearchSort::Size);
                    if(sort.kind==SearchSort::Size) lower="f.size_known="+binds.number(impl_->sortedSizeGroup)+" AND "+lower;
                    Statement rows(db,("SELECT "+std::string(itemProjection)+" FROM "+table+" WHERE "+lower+" AND "+upper+" AND ("+parsed.where+") ORDER BY "+sort.order+" LIMIT "+std::to_string(limit-result.size())).c_str());
                    binds.bind(rows);collect(rows);impl_->sortedCursor=end;
                }
            }
        }
    } catch(const std::exception& error) {
        if(impl_->sequence.load()!=request) {result.clear();impl_->pending=false;}
        else if(Clock::now()>=budget.deadline) {impl_->pending=result.size()<limit;impl_->sortedScan=true;impl_->sortedChunk=std::max<size_t>(1,impl_->sortedChunk/2);}
        else {impl_->pending=false;impl_->queryMessage=L"索引查询暂不可用："+wide(reinterpret_cast<const unsigned char*>(error.what()));}
    }
    if(impl_->sequence.load()!=request) {result.clear();impl_->pending=false;}
    if(impl_->pending.load()) {
        impl_->sortedResults=result;
        if(impl_->queryMessage.empty()) impl_->queryMessage=L"按所选列继续检索 · 已找到 "+std::to_wstring(result.size())+L" 条；未重新排列局部页";
    } else {impl_->sortedActive=false;impl_->sortedResults.clear();}
    sqlite3_progress_handler(db,0,nullptr,nullptr);
    sqlite3_create_function_v2(db,"desk_regex",2,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr);
    if(spec.sort==SearchSort::Size && impl_->queryMessage.empty()) {
        Statement unknown(db,"SELECT value FROM search_meta WHERE key='size_unknown'");
        if(!metaFlag(db,"size_complete") || (unknown.row() && sqlite3_column_int64(unknown.value,0)>0)) impl_->queryMessage=L"未能验证的大小显示为 — 并置于末尾，正在按需补齐可访问元数据";
    }
    return result;
}
bool stopIndexWorker(const fs::path& directory) {
    try {
        auto name=L"Local\\DeskFlowIndexStop-"+std::to_wstring(std::hash<std::wstring>{}(fold(normalize(directory.wstring()))));
        Handle event(OpenEventW(EVENT_MODIFY_STATE,FALSE,name.c_str()));
        return event && SetEvent(event.value)!=FALSE;
    } catch(...) {return false;}
}
std::vector<SearchItem> SearchStore::query(const std::wstring& text,size_t limit,int64_t beforeId) {
    auto request=++impl_->sequence;
    std::unique_lock<std::timed_mutex> lock(impl_->mutex,std::defer_lock);
    if(!lock.try_lock_for(std::chrono::milliseconds(250))) return {};
    if(impl_->sequence.load()!=request) return {};
    bool wasPending=impl_->pending.exchange(false);
    if(impl_->sortedActive) {wasPending=false;impl_->sortedActive=false;impl_->sortedResults.clear();}
    impl_->queryMessage.clear();
    std::vector<SearchItem> result;
    if(!impl_->database) return result;
    limit=std::min<size_t>(limit,1000);
    if(beforeId<0) beforeId=0;
    if(!limit) return result;
    if(text.size()>4096) {impl_->queryMessage=L"搜索输入最长 4096 字符";return result;}
    std::string clauses="1";
    if(beforeId>0) clauses+=" AND f.id<"+std::to_string(beforeId);
    std::vector<std::wstring> parameters;
    std::wstring fullText;
    std::wstring shortMatch;
    bool hasShort=false,hasRegex=false,shortAllowed=true;
    auto parameter=[&](const std::wstring& value) {parameters.push_back(value);return "?"+std::to_string(parameters.size());};
    for(auto token:tokens(text)) {
        token=fold(token);
        if(token==L"type:file" || token==L"file:") clauses+=" AND f.folder=0";
        else if(token==L"type:folder" || token==L"folder:") clauses+=" AND f.folder=1";
        else if(token.starts_with(L"ext:")) {
            auto extension=token.substr(4);
            if(!extension.empty() && extension.front()==L'.') extension.erase(0,1);
            clauses+=" AND f.folder=0 AND f.name_fold GLOB "+parameter(L"*."+extension);
        } else if(token.starts_with(L"in:")) {
            auto path=token.substr(3);
            if(path.empty()) continue;
            try {path=fold(normalize(path));} catch(...) {impl_->queryMessage=L"限定目录路径无效";return {};}
            auto first=parameter(path),prefix=parameter(subtreePrefix(path));
            auto upper=subtreePrefix(path);upper.back()=L']';auto last=parameter(upper);
            clauses+=" AND (f.path_fold="+first+" OR (f.path_fold>="+prefix+" AND f.path_fold<"+last+"))";
        } else if(token.starts_with(L"regex:")) {
            auto pattern=token.substr(6);
            if(!BasicRegex(pattern).valid) {impl_->queryMessage=L"正则支持字符、.、[]、*+? 与 ^/$，不支持分组、分支或反向引用";return {};}
            auto bind=parameter(pattern);
            clauses+=" AND (desk_regex("+bind+",f.name_fold) OR desk_regex("+bind+",f.path_fold))";
            hasShort=true;hasRegex=true;shortAllowed=false;
        } else if(token.starts_with(L"path:")) {
            auto path=token.substr(5);
            auto bind=parameter(path);
            clauses+=" AND instr(f.path_fold,"+bind+")>0";
            if(codepoints(path)>=3) {if(!fullText.empty())fullText+=L" AND ";fullText+=ftsQuote(path);}
            else hasShort=true;
            shortAllowed=false;
        } else {
            bool wildcard=token.find_first_of(L"*?")!=std::wstring::npos;
            bool pathQuery=token.find_first_of(L"\\/")!=std::wstring::npos || (token.size()>=2 && token[1]==L':' && iswalpha(token[0]));
            if(pathQuery) shortAllowed=false;
            auto bind=parameter(token);
            auto field=pathQuery ? std::string("f.path_fold") : std::string("f.name_fold");
            if(wildcard) clauses+=" AND "+field+" GLOB "+bind;
            else clauses+=" AND instr("+field+","+bind+")>0";
            auto literal=wildcard ? longestLiteral(token):token;
            if(codepoints(literal)>=3) {if(!fullText.empty())fullText+=L" AND ";fullText+=ftsQuote(literal);}
            else {
                hasShort=true;
                auto gram=search_detail::shortQueryToken(literal);
                if(gram.empty()) shortAllowed=false;
                else {if(!shortMatch.empty()) shortMatch+=L" AND ";shortMatch+=ftsQuote(std::wstring(gram.begin(),gram.end()));}
            }
        }
    }
    std::string joinSql;
    if(!fullText.empty()) {
        joinSql=" JOIN files_fts ON files_fts.rowid=f.id ";clauses+=" AND files_fts MATCH "+parameter(fullText);
        if(beforeId>0) clauses+=" AND files_fts.rowid<"+std::to_string(beforeId);
    }
    const std::string order=fullText.empty() ? "f.id" : "files_fts.rowid";
    const std::string from="SELECT f.id,f.name,f.path,f.folder,f.size,f.size_known FROM files f "+joinSql;
    const std::string select=from+" WHERE "+clauses;
    bool newContinuation=false;
    if(hasShort) {
        if(!wasPending || impl_->continuationText!=text || impl_->continuationLimit!=limit || impl_->continuationBeforeId!=beforeId) {
            newContinuation=true;
            impl_->continuationText=text;impl_->continuationLimit=limit;impl_->continuationBeforeId=beforeId;
            impl_->continuationCursor=beforeId ? beforeId : std::numeric_limits<int64_t>::max();
            impl_->chunkSize=hasRegex ? 64 : 8192;impl_->continuationResults.clear();
        }
        result=impl_->continuationResults;
    } else impl_->continuationResults.clear();
    QueryBudget budget{Clock::now()+std::chrono::milliseconds(hasShort?180:140),&impl_->sequence,request};
    sqlite3_progress_handler(impl_->database->value,512,progress,&budget);
    sqlite3_create_function_v2(impl_->database->value,"desk_regex",2,SQLITE_UTF8,&budget,regexFunction,nullptr,nullptr,nullptr);
    try {
        auto collect=[&](Statement& statement) {
            while(statement.row()) {
                SearchItem item;
                item.id=sqlite3_column_int64(statement.value,0);item.name=wide(sqlite3_column_text(statement.value,1));item.path=wide(sqlite3_column_text(statement.value,2));
                item.folder=sqlite3_column_int(statement.value,3)!=0;item.size=static_cast<uint64_t>(sqlite3_column_int64(statement.value,4));
                item.sizeKnown=!item.folder && sqlite3_column_int(statement.value,5)!=0;
                if(!hasShort || std::none_of(result.begin(),result.end(),[&](const SearchItem& existing){return existing.id==item.id;})) result.push_back(std::move(item));
            }
        };
        auto bindText=[&](Statement& statement) {for(size_t index=0;index<parameters.size();++index) statement.text(static_cast<int>(index+1),parameters[index]);};
        if(!hasShort) {
            Statement statement(impl_->database->value,(select+" ORDER BY "+order+" DESC LIMIT "+std::to_string(limit)).c_str());
            bindText(statement);collect(statement);
        } else {
            Statement coverage(impl_->database->value,"SELECT value FROM search_meta WHERE key='short_complete'");
            bool gramComplete=coverage.row() && sqlite3_column_int(coverage.value,0)!=0;
            bool usedNameIndex=false;
            if(newContinuation && gramComplete && shortAllowed && !shortMatch.empty()) {
                const auto gramCursor=beforeId>0 ? " AND files_short.rowid<"+std::to_string(beforeId) : std::string{};
                Statement filename(impl_->database->value,(from+" JOIN files_short ON files_short.rowid=f.id WHERE "+clauses+gramCursor+
                    " AND files_short MATCH ?"+std::to_string(parameters.size()+1)+" ORDER BY files_short.rowid DESC LIMIT "+std::to_string(limit)).c_str());
                bindText(filename);filename.text(static_cast<int>(parameters.size()+1),shortMatch);collect(filename);
                usedNameIndex=true;
            }
            // Name grams cover the entire default filename query and preserve
            // rowid pagination. Explicit path/regex queries use bounded scans.
            if(!usedNameIndex)
            while(result.size()<limit) {
                if(progress(&budget)) {impl_->pending=true;break;}
                int64_t end=0;
                Statement boundary(impl_->database->value,("SELECT id FROM files WHERE id<?1 ORDER BY id DESC LIMIT 1 OFFSET "+std::to_string(impl_->chunkSize-1)).c_str());
                boundary.number(1,impl_->continuationCursor);
                if(boundary.row()) end=sqlite3_column_int64(boundary.value,0);
                else {Statement last(impl_->database->value,"SELECT min(id) FROM files WHERE id<?1");last.number(1,impl_->continuationCursor);last.row();end=sqlite3_column_int64(last.value,0);}
                if(end<=0 || end>=impl_->continuationCursor) break;
                auto firstParameter=static_cast<int>(parameters.size()+1),lastParameter=firstParameter+1;
                std::string cursorColumn=fullText.empty() ? "f.id" : "files_fts.rowid";
                auto range=" AND "+cursorColumn+"<?"+std::to_string(firstParameter)+" AND "+cursorColumn+">=?"+std::to_string(lastParameter);
                Statement statement(impl_->database->value,(select+range+" ORDER BY "+order+" DESC LIMIT "+std::to_string(limit-result.size())).c_str());
                bindText(statement);statement.number(firstParameter,impl_->continuationCursor);statement.number(lastParameter,end);collect(statement);
                impl_->continuationCursor=end;
            }
        }
    } catch(const std::exception& error) {
        if(impl_->sequence.load()!=request) {result.clear();impl_->pending=false;impl_->continuationResults.clear();}
        else if(Clock::now()>=budget.deadline) {
            if(hasShort) {impl_->pending=result.size()<limit;impl_->chunkSize=std::max<size_t>(1,impl_->chunkSize/2);}
            else impl_->queryMessage=L"本次检索已达到时间预算；结果可能不完整，可添加关键词或限定目录";
        }
        else impl_->queryMessage=L"索引查询暂不可用："+wide(reinterpret_cast<const unsigned char*>(error.what()));
    }
    if(impl_->sequence.load()!=request) {result.clear();impl_->pending=false;}
    if(impl_->pending.load()) {
        impl_->continuationResults=result;
        impl_->queryMessage=L"短词 / 正则搜索仍在继续 · 已找到 "+std::to_wstring(result.size())+L" 条，结果会继续增加";
    } else impl_->continuationResults.clear();
    sqlite3_progress_handler(impl_->database->value,0,nullptr,nullptr);
    sqlite3_create_function_v2(impl_->database->value,"desk_regex",2,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr);
    return result;
}
SearchStatus SearchStore::status() {
    std::unique_lock<std::timed_mutex> lock(impl_->mutex,std::defer_lock);
    if(!lock.try_lock_for(std::chrono::milliseconds(15))) {std::lock_guard cachedLock(impl_->cachedMutex);return impl_->cached;}
    if(!impl_->database) {std::lock_guard cachedLock(impl_->cachedMutex);return impl_->cached;}
    try {
        SearchStatus state;DWORD pid=0;
        Statement statement(impl_->database->value,"SELECT key,value FROM search_meta");
        while(statement.row()) {
            auto key=std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement.value,0)));
            auto value=wide(sqlite3_column_text(statement.value,1));
            if(key=="total") state.total=std::stoll(value);
            else if(key=="building") state.building=value==L"1";
            else if(key=="message") state.message=std::move(value);
            else if(key=="pid") pid=static_cast<DWORD>(std::stoul(value));
        }
        if(pid) {
            Handle process(OpenProcess(SYNCHRONIZE,FALSE,pid));
            if((process && WaitForSingleObject(process.value,0)==WAIT_OBJECT_0) || (!process && GetLastError()==ERROR_INVALID_PARAMETER)) {state.building=false;state.message=L"索引进程已停止，已有索引仍可查询";}
        }
        if(!impl_->queryMessage.empty()) state.message+=L" · "+impl_->queryMessage;
        std::lock_guard cachedLock(impl_->cachedMutex);impl_->cached=state;
    } catch(...) {}
    std::lock_guard cachedLock(impl_->cachedMutex);return impl_->cached;
}
int indexWorkerMain(const fs::path& directory,const std::vector<std::wstring>& roots,DWORD parentPid) {
    // Exactly one writer process owns a data directory. The UI never elevates.
    std::wstring mutexName=L"Local\\DeskFlowIndex-"+std::to_wstring(std::hash<std::wstring>{}(fold(normalize(directory.wstring()))));
    Handle singleton(CreateMutexW(nullptr,FALSE,mutexName.c_str()));
    if(!singleton) return 1;
    DWORD acquired=WaitForSingleObject(singleton.value,0);
    if(acquired!=WAIT_OBJECT_0 && acquired!=WAIT_ABANDONED) return 0;
    Handle parent(parentPid ? OpenProcess(SYNCHRONIZE,FALSE,parentPid) : nullptr);
    if(parentPid && !parent) {ReleaseMutex(singleton.value);return 0;}
    auto stopName=L"Local\\DeskFlowIndexStop-"+std::to_wstring(std::hash<std::wstring>{}(fold(normalize(directory.wstring()))));
    Handle stop(createControllerEvent(stopName));
    if(!stop) {ReleaseMutex(singleton.value);return 1;}
    ResetEvent(stop.value);
    controlStop=stop.value;SetConsoleCtrlHandler(consoleControl,TRUE);
    SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN);
    int outcome=0;
    try {
        Indexer indexer(directory);
        indexer.initialize(roots);
        auto published=Clock::now();
        const unsigned processors=std::max(1u,(unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        auto cpuTicks=[] {
            FILETIME created{},exited{},kernel{},user{};
            if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)) return uint64_t{0};
            return (uint64_t(kernel.dwHighDateTime)<<32)+kernel.dwLowDateTime+
                   (uint64_t(user.dwHighDateTime)<<32)+user.dwLowDateTime;
        };
        while(WaitForSingleObject(stop.value,0)!=WAIT_OBJECT_0 && (!parent || WaitForSingleObject(parent.value,0)!=WAIT_OBJECT_0)) {
            const auto began=Clock::now(); const auto cpuBefore=cpuTicks();
            indexer.notifications();
            bool working=indexer.ntfsBatch();
            working=indexer.refreshBatch() || working;
            working=indexer.scanBatch() || working;
            working=search_detail::backfillShortGrams(indexer.database.value,256) || working;
            working=search_detail::prepareSortIndexes(indexer.database.value,256) || working;
            working=search_detail::backfillFileSizes(indexer.database.value,64) || working;
            if(Clock::now()-published>=std::chrono::seconds(1) || (!working && indexer.scanning())) {indexer.publish();published=Clock::now();}
            std::vector<HANDLE> waits{stop.value};
            if(parent) waits.push_back(parent.value);
            const auto cpuAfter=cpuTicks();
            const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-began).count()/100;
            const auto rest=search_detail::backgroundDelayMs(cpuAfter-cpuBefore,(uint64_t)std::max<int64_t>(0,elapsed),processors);
            // A continuously signaled watcher must not cancel the CPU rest;
            // stop and parent exit still interrupt it immediately.
            if(rest && WaitForMultipleObjects(static_cast<DWORD>(waits.size()),waits.data(),FALSE,rest)!=WAIT_TIMEOUT) break;
            for(const auto& watcher:indexer.watchers) if(watcher->pending) waits.push_back(watcher->event.value);
            WaitForMultipleObjects(static_cast<DWORD>(waits.size()),waits.data(),FALSE,working?0:1000);
        }
        meta(indexer.database.value,"building",L"0");
        meta(indexer.database.value,"message",L"索引进程已停止，已有索引仍可查询");
        execute(indexer.database.value,"PRAGMA wal_checkpoint(PASSIVE)");
    } catch(const std::exception& error) {
        try {Database db(directory,256);meta(db.value,"building",L"0");meta(db.value,"message",L"索引暂停："+wide(reinterpret_cast<const unsigned char*>(error.what())));} catch(...) {}
        outcome=1;
    }
    SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_END);
    SetConsoleCtrlHandler(consoleControl,FALSE);controlStop=nullptr;
    ReleaseMutex(singleton.value);return outcome;
}
}
