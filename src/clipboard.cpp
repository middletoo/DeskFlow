#include "clipboard.hpp"
#include "common.hpp"
#include <fstream>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
namespace desk {
static constexpr size_t StoredSafetyLimit = 128 * 1024 * 1024;
static std::string likePattern(const std::wstring &v) {
    std::string p = "%";
    for (char c : utf8(v)) {
        if (c == '%' || c == '_' || c == '\\')
            p += '\\';
        p += c;
    }
    return p + "%";
}
struct HistoryStore::Impl {
    std::filesystem::path dir;
    Database db;
    std::mutex mutex;
    std::atomic_size_t entryLimit{32 * 1024 * 1024};
    std::atomic_uint64_t imageQuota{5ULL * 1024 * 1024 * 1024};
    explicit Impl(const std::filesystem::path &d) : dir(d), db(d / L"history.db") {
        {
            Statement version(db, "PRAGMA user_version");
            version.step();
            if (version.integer(0) > 2)
                throw std::runtime_error("历史库来自较新版本，已保留数据，请使用对应版本打开");
        }
        std::filesystem::create_directories(dir / L"objects");
        db.exec("CREATE TABLE IF NOT EXISTS clips(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,title "
                "TEXT NOT NULL,body TEXT NOT NULL,source TEXT NOT NULL,created INTEGER NOT "
                "NULL,pinned INTEGER NOT NULL DEFAULT 0,size INTEGER NOT NULL,hash TEXT NOT NULL);"
                "CREATE TABLE IF NOT EXISTS formats(clip_id INTEGER REFERENCES clips(id) ON DELETE "
                "CASCADE,format INTEGER NOT NULL,name TEXT NOT NULL,path TEXT NOT NULL,size "
                "INTEGER NOT NULL,PRIMARY KEY(clip_id,format));"
                "CREATE INDEX IF NOT EXISTS clips_date ON clips(created DESC);CREATE INDEX IF NOT "
                "EXISTS formats_object ON formats(path);"
                "CREATE INDEX IF NOT EXISTS clips_favorites ON clips(id DESC) WHERE pinned=1;"
                "CREATE INDEX IF NOT EXISTS clips_kind ON clips(kind,id DESC);"
                "CREATE VIRTUAL TABLE IF NOT EXISTS clips_fts USING "
                "fts5(body,content='clips',content_rowid='id',tokenize='trigram');"
                "CREATE TRIGGER IF NOT EXISTS clips_ai AFTER INSERT ON clips BEGIN INSERT INTO "
                "clips_fts(rowid,body) VALUES(new.id,new.body);END;"
                "CREATE TRIGGER IF NOT EXISTS clips_ad AFTER DELETE ON clips BEGIN INSERT INTO "
                "clips_fts(clips_fts,rowid,body) VALUES('delete',old.id,old.body);END;"
                "CREATE TABLE IF NOT EXISTS history_totals(key TEXT PRIMARY KEY,value INTEGER NOT "
                "NULL);"
                "CREATE TRIGGER IF NOT EXISTS history_count_add AFTER INSERT ON clips BEGIN UPDATE "
                "history_totals SET value=value+1 WHERE key='clips';END;"
                "CREATE TRIGGER IF NOT EXISTS history_count_remove AFTER DELETE ON clips BEGIN "
                "UPDATE history_totals SET value=value-1 WHERE key='clips';END;"
                "CREATE TRIGGER IF NOT EXISTS history_image_add AFTER INSERT ON formats WHEN "
                "new.format IN(8,17) AND (SELECT count(*) FROM formats WHERE path=new.path AND "
                "format IN(8,17))=1 BEGIN UPDATE history_totals SET value=value+new.size WHERE "
                "key='images';END;"
                "CREATE TRIGGER IF NOT EXISTS history_image_remove AFTER DELETE ON formats WHEN "
                "old.format IN(8,17) AND NOT EXISTS(SELECT 1 FROM formats WHERE path=old.path AND "
                "format IN(8,17)) BEGIN UPDATE history_totals SET value=value-old.size WHERE "
                "key='images';END;"
                "PRAGMA user_version=2;");
        {
            Statement check(db, "SELECT count(*) FROM history_totals");
            check.step();
            if (check.integer(0) < 2)
                db.exec("INSERT OR REPLACE INTO history_totals SELECT 'clips',count(*) FROM "
                        "clips;INSERT OR REPLACE INTO history_totals SELECT "
                        "'images',COALESCE(sum(size),0) FROM (SELECT path,max(size) AS size FROM "
                        "formats WHERE format IN(8,17) GROUP BY path);");
        }
    }
};
HistoryStore::HistoryStore(const std::filesystem::path &d) {
    std::filesystem::create_directories(d);
    impl = std::make_unique<Impl>(d);
}
HistoryStore::~HistoryStore() = default;
int64_t HistoryStore::append(const ClipPayload &payload) {
    if (payload.formats.empty())
        return 0;
    std::lock_guard lock(impl->mutex);
    size_t bytes = 0;
    std::wstring body, kind = L"格式", title;
    std::string composite;
    std::vector<std::string> hashes;
    for (const auto &f : payload.formats) {
        bytes += f.data.size();
        if (bytes > impl->entryLimit)
            throw std::runtime_error("条目超过设置的单条上限，未记录；可在设置增加上限");
        auto hash = sha256(f.data.data(), f.data.size());
        hashes.push_back(hash);
        composite += std::to_string(f.format) + utf8(f.name) + hash;
        if (f.format == CF_UNICODETEXT && f.data.size() >= 2) {
            size_t n = f.data.size() / 2;
            std::wstring s(n, 0);
            memcpy(s.data(), f.data.data(), n * 2);
            auto end = s.find(L'\0');
            if (end != std::wstring::npos)
                s.resize(end);
            body = s;
            kind = L"文本";
        } else if (f.format == CF_DIB || f.format == CF_DIBV5) {
            if (body.empty())
                kind = L"图片";
        } else if (f.format == CF_HDROP) {
            if (body.empty()) {
                kind = L"文件";
                if (f.data.size() >= sizeof(DROPFILES)) {
                    DROPFILES drop{};
                    memcpy(&drop, f.data.data(), sizeof(drop));
                    if (drop.pFiles < f.data.size() && drop.fWide && drop.pFiles % 2 == 0) {
                        std::wstring names((f.data.size() - drop.pFiles) / 2, 0);
                        memcpy(names.data(), f.data.data() + drop.pFiles, names.size() * 2);
                        size_t begin = 0;
                        while (begin < names.size()) {
                            auto end = names.find(L'\0', begin);
                            if (end == begin || end == std::wstring::npos)
                                break;
                            if (!body.empty())
                                body += L'\n';
                            body += names.substr(begin, end - begin);
                            begin = end + 1;
                        }
                    } else if (drop.pFiles < f.data.size() && !drop.fWide) {
                        std::string names((const char *)f.data.data() + drop.pFiles,
                                          f.data.size() - drop.pFiles);
                        size_t begin = 0;
                        while (begin < names.size()) {
                            auto end = names.find('\0', begin);
                            if (end == begin || end == std::string::npos)
                                break;
                            auto path = names.substr(begin, end - begin);
                            int n = MultiByteToWideChar(CP_ACP, 0, path.data(), (int)path.size(),
                                                        nullptr, 0);
                            if (n > 0) {
                                std::wstring name(n, 0);
                                MultiByteToWideChar(CP_ACP, 0, path.data(), (int)path.size(),
                                                    name.data(), n);
                                if (!body.empty())
                                    body += L'\n';
                                body += name;
                            }
                            begin = end + 1;
                        }
                    }
                }
            }
        }
    }
    if (bytes == 0)
        return 0;
    auto hash = sha256(composite.data(), composite.size());
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    {
        Statement last(impl->db, "SELECT id,hash FROM clips ORDER BY id DESC LIMIT 1");
        if (last.step() && last.text(1) == hash) {
            auto id = last.integer(0);
            Statement u(impl->db, "UPDATE clips SET created=? WHERE id=?");
            u.bind(1, now);
            u.bind(2, id);
            u.step();
            return id;
        }
    }
    if (kind == L"图片") {
        Statement used(impl->db, "SELECT value FROM history_totals WHERE key='images'");
        used.step();
        uint64_t additional = 0;
        for (size_t i = 0; i < payload.formats.size(); i++) {
            auto &f = payload.formats[i];
            if (f.format != CF_DIB && f.format != CF_DIBV5)
                continue;
            Statement existing(impl->db,
                               "SELECT 1 FROM formats WHERE path=? AND format IN(8,17) LIMIT 1");
            existing.bind(1, hashes[i]);
            if (!existing.step())
                additional += f.data.size();
        }
        if (used.integer(0) + (int64_t)additional > (int64_t)impl->imageQuota.load())
            throw std::runtime_error("图片历史达到存储预算，请在设置增加预算，或导出、清理后继续");
    }
    if (std::filesystem::space(impl->dir).available < bytes + 64ULL * 1024 * 1024)
        throw std::runtime_error("磁盘空间不足，已保留原有历史并暂停新增");
    for (size_t i = 0; i < payload.formats.size(); i++) {
        auto object = impl->dir / L"objects" / wide(hashes[i]);
        if (std::filesystem::exists(object))
            continue;
        auto temporary = object;
        temporary += L".pending-" + std::to_wstring(GetCurrentProcessId());
        {
            HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                throw std::runtime_error("历史附件写入失败");
            DWORD written = 0;
            auto &bytes = payload.formats[i].data;
            bool complete = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) &&
                            written == bytes.size() && FlushFileBuffers(file);
            CloseHandle(file);
            if (!complete)
                throw std::runtime_error("历史附件写入失败");
        }
        if (!MoveFileExW(temporary.c_str(), object.c_str(), MOVEFILE_WRITE_THROUGH)) {
            DWORD error = GetLastError();
            DeleteFileW(temporary.c_str());
            if (!std::filesystem::exists(object))
                throw std::runtime_error("历史附件提交失败（Windows " + std::to_string(error) + "）");
        }
    }
    title = body.empty() ? (kind + L"内容") : body.substr(0, 120);
    for (auto &c : title)
        if (c == L'\r' || c == L'\n' || c == L'\t')
            c = L' ';
    impl->db.exec("BEGIN IMMEDIATE");
    int64_t id = 0;
    try {
        Statement s(
            impl->db,
            "INSERT INTO clips(kind,title,body,source,created,size,hash)VALUES(?,?,?,?,?,?,?)");
        s.bind(1, utf8(kind));
        s.bind(2, utf8(title));
        s.bind(3, utf8(body));
        s.bind(4, utf8(payload.source));
        s.bind(5, now);
        s.bind(6, (int64_t)bytes);
        s.bind(7, hash);
        s.step();
        id = sqlite3_last_insert_rowid(impl->db.handle);
        for (size_t i = 0; i < payload.formats.size(); i++) {
            const auto &f = payload.formats[i];
            Statement z(impl->db,
                        "INSERT INTO formats(clip_id,format,name,path,size)VALUES(?,?,?,?,?)");
            z.bind(1, id);
            z.bind(2, (int64_t)f.format);
            z.bind(3, utf8(f.name));
            z.bind(4, hashes[i]);
            z.bind(5, (int64_t)f.data.size());
            z.step();
        }
        impl->db.exec("COMMIT");
    } catch (...) {
        impl->db.exec("ROLLBACK");
        throw;
    }
    return id;
}
std::vector<HistoryItem> HistoryStore::list(const std::wstring &query, int64_t beforeId,
                                            size_t limit, bool pinnedOnly, const std::wstring& kind) {
    std::lock_guard lock(impl->mutex);
    std::string sql =
        "SELECT c.id,c.kind,c.title,substr(c.body,1,800),c.source,c.created,c.pinned,c.size FROM "
        "clips c ";
    if (query.size() >= 3)
        sql += "JOIN clips_fts f ON f.rowid=c.id ";
    sql += "WHERE 1=1 ";
    if(beforeId>0)sql+=query.size()>=3?"AND f.rowid<? ":"AND c.id<? ";
    if (pinnedOnly)
        sql += "AND c.pinned=1 ";
    if (!kind.empty()) sql += "AND c.kind=? ";
    if (!query.empty())
        sql += query.size() >= 3 ? "AND f.body MATCH ? " : "AND c.body LIKE ? ESCAPE '\\' ";
    sql += query.size() >= 3 ? "ORDER BY f.rowid DESC LIMIT ?" : "ORDER BY c.id DESC LIMIT ?";
    Statement s(impl->db, sql.c_str());
    int bound = 1;
    if(beforeId>0)s.bind(bound++,beforeId);
    if(!kind.empty())s.bind(bound++,utf8(kind));
    if (!query.empty()) {
        auto pattern = likePattern(query);
        if (query.size() >= 3) {
            pattern = "\"";
            for (auto c : utf8(query)) {
                pattern += c;
                if (c == '\"')
                    pattern += c;
            }
            pattern += '\"';
        }
        s.bind(bound++, pattern);
    }
    s.bind(bound, (int64_t)std::min<size_t>(limit, 500));
    struct Deadline {
        ULONGLONG end;
    };
    Deadline deadline{GetTickCount64() + 180};
    sqlite3_progress_handler(
        impl->db.handle, 512,
        [](void *p) { return GetTickCount64() > ((Deadline *)p)->end ? 1 : 0; }, &deadline);
    struct ProgressGuard {
        sqlite3 *db;
        ~ProgressGuard() {
            sqlite3_progress_handler(db, 0, nullptr, nullptr);
        }
    } progress{impl->db.handle};
    std::vector<HistoryItem> items;
    while (s.step()) {
        HistoryItem i;
        i.id = s.integer(0);
        i.kind = wide(s.text(1));
        i.title = wide(s.text(2));
        i.preview = wide(s.text(3));
        i.source = wide(s.text(4));
        i.created = s.integer(5);
        i.pinned = s.integer(6) != 0;
        i.bytes = s.integer(7);
        items.push_back(std::move(i));
    }
    return items;
}
ClipPayload HistoryStore::load(int64_t id) {
    std::lock_guard lock(impl->mutex);
    ClipPayload p;
    Statement s(impl->db, "SELECT format,name,path,size FROM formats WHERE clip_id=?");
    s.bind(1, id);
    size_t total = 0;
    while (s.step()) {
        ClipFormat f;
        f.format = (UINT)s.integer(0);
        f.name = wide(s.text(1));
        auto name = s.text(2);
        auto size = s.integer(3);
        if (name.size() != 64 || name.find_first_not_of("0123456789abcdef") != std::string::npos ||
            size < 0 || (total += (size_t)size) > StoredSafetyLimit)
            throw std::runtime_error("历史附件记录无效");
        f.data.resize((size_t)size);
        std::ifstream file(impl->dir / L"objects" / wide(name), std::ios::binary);
        file.read((char *)f.data.data(), size);
        if (!file || sha256(f.data.data(), f.data.size()) != name)
            throw std::runtime_error("历史附件缺失或校验失败，原记录已保留");
        p.formats.push_back(std::move(f));
    }
    return p;
}
void HistoryStore::setPinned(int64_t id, bool pinned) {
    std::lock_guard lock(impl->mutex);
    Statement s(impl->db, "UPDATE clips SET pinned=? WHERE id=?");
    s.bind(1, (int64_t)pinned);
    s.bind(2, id);
    s.step();
}
void HistoryStore::erase(int64_t id) {
    std::lock_guard lock(impl->mutex);
    std::vector<std::string> objects;
    {
        Statement q(impl->db, "SELECT path FROM formats WHERE clip_id=?");
        q.bind(1, id);
        while (q.step())
            objects.push_back(q.text(0));
    }
    {
        Statement s(impl->db, "DELETE FROM clips WHERE id=?");
        s.bind(1, id);
        s.step();
    }
    for (auto &name : objects) {
        if (name.size() != 64 || name.find_first_not_of("0123456789abcdef") != std::string::npos)
            continue;
        Statement refs(impl->db, "SELECT 1 FROM formats WHERE path=? LIMIT 1");
        refs.bind(1, name);
        if (!refs.step()) {
            std::error_code ec;
            std::filesystem::remove(impl->dir / L"objects" / wide(name), ec);
            if (ec)
                throw std::runtime_error("记录已删除，附件被占用，请关闭占用它的程序后清理");
        }
    }
}
int64_t HistoryStore::count() {
    std::lock_guard lock(impl->mutex);
    Statement s(impl->db, "SELECT value FROM history_totals WHERE key='clips'");
    s.step();
    return s.integer(0);
}
void HistoryStore::backup(const std::filesystem::path &target) {
    std::lock_guard lock(impl->mutex);
    if (std::filesystem::exists(target))
        throw std::runtime_error("备份目标已存在，请使用一个新的备份目录；已有备份保持完整");
    auto staging = target;
    staging += L".pending-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64());
    std::filesystem::create_directories(staging / L"objects");
    try {
        {
            Database dest(staging / L"history.db");
            auto b = sqlite3_backup_init(dest.handle, "main", impl->db.handle, "main");
            if (!b)
                throw std::runtime_error("无法开始一致性备份");
            int rc = SQLITE_OK;
            auto deadline = GetTickCount64() + 10000;
            do {
                rc = sqlite3_backup_step(b, 128);
                if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
                    Sleep(20);
            } while ((rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
                     GetTickCount64() < deadline);
            sqlite3_backup_finish(b);
            if (rc != SQLITE_DONE)
                throw std::runtime_error("备份未完成，原始历史未改变");
            Statement refs(impl->db, "SELECT DISTINCT path FROM formats");
            while (refs.step()) {
                auto token = refs.text(0);
                if (token.size() != 64 ||
                    token.find_first_not_of("0123456789abcdef") != std::string::npos)
                    throw std::runtime_error("备份附件引用无效，原始历史保持不变");
                auto name = wide(token);
                std::filesystem::copy_file(impl->dir / L"objects" / name,
                                           staging / L"objects" / name,
                                           std::filesystem::copy_options::overwrite_existing);
            }
            dest.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        }
        if (!MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("备份目录提交失败，已有备份保持不变");
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        throw;
    }
}
void HistoryStore::setLimits(size_t maximumEntryBytes, uint64_t imageQuotaBytes) {
    if (maximumEntryBytes < 1024 * 1024 || maximumEntryBytes > StoredSafetyLimit ||
        imageQuotaBytes < 1024ULL * 1024 * 1024)
        throw std::runtime_error("存储预算设置无效");
    impl->entryLimit = maximumEntryBytes;
    impl->imageQuota = imageQuotaBytes;
}
ClipPayload readClipboardPayload(HWND owner, size_t maximumBytes) {
    ClipPayload payload;
    bool open = false;
    for (int a = 0; a < 5; a++) {
        if (OpenClipboard(owner)) {
            open = true;
            break;
        }
        Sleep(15 + a * 10);
    }
    if (!open)
        throw std::runtime_error("剪贴贴板被其他程序占用，未记录这次内容");
    struct Guard {
        ~Guard() {
            CloseClipboard();
        }
    } guard;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetClipboardOwner(), &pid);
    auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t name[32768];
        DWORD n = 32768;
        if (QueryFullProcessImageNameW(process, 0, name, &n))
            payload.source = std::filesystem::path(name).filename().wstring();
        CloseHandle(process);
    }
    UINT excluded = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (IsClipboardFormatAvailable(excluded))
        return payload;
    auto includeHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(includeHistory)) {
        auto flag = GetClipboardData(includeHistory);
        if (flag && GlobalSize(flag) >= sizeof(DWORD)) {
            auto value = (DWORD *)GlobalLock(flag);
            bool exclude = value && *value == 0;
            if (value)
                GlobalUnlock(flag);
            if (exclude)
                return payload;
        }
    }
    std::vector<std::pair<UINT, std::wstring>> formats = {
        {CF_UNICODETEXT, L""},
        {IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB, L""},
        {CF_HDROP, L""},
        {RegisterClipboardFormatW(L"Preferred DropEffect"), L"Preferred DropEffect"},
        {RegisterClipboardFormatW(L"HTML Format"), L"HTML Format"},
        {RegisterClipboardFormatW(L"Rich Text Format"), L"Rich Text Format"}};
    size_t total = 0;
    for (auto [format, name] : formats) {
        if (!IsClipboardFormatAvailable(format))
            continue;
        auto h = GetClipboardData(format);
        if (!h)
            continue;
        auto size = GlobalSize(h);
        if (!size)
            continue;
        total += size;
        if (total > maximumBytes)
            throw std::runtime_error("复制内容超过设置的单条上限，未记录这次内容");
        auto ptr = GlobalLock(h);
        if (!ptr)
            continue;
        ClipFormat f;
        f.format = format;
        f.name = name;
        f.data.resize(size);
        memcpy(f.data.data(), ptr, size);
        GlobalUnlock(h);
        payload.formats.push_back(std::move(f));
    }
    return payload;
}
bool restoreClipboardPayload(HWND owner, const ClipPayload &payload) {
    if (payload.formats.empty())
        return false;
    std::vector<std::pair<UINT, HGLOBAL>> prepared;
    for (auto &f : payload.formats) {
        auto m = GlobalAlloc(GMEM_MOVEABLE, f.data.size());
        if (!m) {
            for (auto p : prepared)
                GlobalFree(p.second);
            return false;
        }
        auto ptr = GlobalLock(m);
        if (!ptr) {
            GlobalFree(m);
            for (auto p : prepared)
                GlobalFree(p.second);
            return false;
        }
        memcpy(ptr, f.data.data(), f.data.size());
        GlobalUnlock(m);
        prepared.push_back(
            {f.name.empty() ? f.format : RegisterClipboardFormatW(f.name.c_str()), m});
    }
    if (!OpenClipboard(owner)) {
        for (auto p : prepared)
            GlobalFree(p.second);
        return false;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        for (auto p : prepared)
            GlobalFree(p.second);
        return false;
    }
    bool okay = true;
    for (auto [format, m] : prepared) {
        if (!SetClipboardData(format, m)) {
            GlobalFree(m);
            okay = false;
        }
    }
    CloseClipboard();
    return okay;
}
} // namespace desk
