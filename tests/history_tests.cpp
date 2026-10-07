#include "clipboard.hpp"
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <shellapi.h>
#include <shlobj.h>
static void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
static desk::ClipPayload text(const std::wstring &value) {
    desk::ClipPayload p;
    desk::ClipFormat f;
    f.format = CF_UNICODETEXT;
    f.data.resize((value.size() + 1) * sizeof(wchar_t));
    memcpy(f.data.data(), value.c_str(), f.data.size());
    p.formats.push_back(f);
    return p;
}
int main() {
    auto dir = std::filesystem::temp_directory_path() /
               (L"DeskFlow-history-test-" + std::to_wstring(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(dir);
        int64_t first = 0;
        {
            desk::HistoryStore db(dir);
            first = db.append(text(L"多年保留剪切历史，不加载全部内容"));
            check(first > 0, "history insert must return persisted id");
            check(db.append(text(L"多年保留剪切历史，不加载全部内容")) == first,
                  "consecutive content must deduplicate");
            check(db.count() == 1, "dedup must keep one row");
            db.append(text(L"another clipboard entry"));
            check(db.list(L"历史").size() == 1, "two-character Chinese substring must match");
            check(db.list(L"不存在").empty(), "unmatched query must be empty");
            check(db.list(L"", 0, 1).size() == 1, "query result must be bounded");
            db.setPinned(first, true);
            auto found = db.list(L"历史");
            check(found.front().pinned, "pin must persist");
            auto loaded = db.load(first);
            check(loaded.formats.front().data ==
                      text(L"多年保留剪切历史，不加载全部内容").formats.front().data,
                  "raw Unicode clipboard bytes must round-trip");
            db.backup(dir / L"backup");
            bool refused = false;
            try {
                db.backup(dir / L"backup");
            } catch (const std::exception &) {
                refused = true;
            }
            check(refused, "existing complete backup must never be overwritten in place");
            for (int i = 0; i < 130; i++)
                db.append(text(L"分页条目 " + std::to_wstring(i)));
            auto page = db.list(L"分页", 0, 100);
            check(page.size() == 100, "first page must contain bounded rows");
            check(db.list(L"分页", page.back().id, 100).size() == 30,
                  "cursor must reach older history without omission");
            auto unique = text(L"删除后释放独立附件");
            auto uniqueId = db.append(unique);
            size_t objectsBefore =
                std::distance(std::filesystem::directory_iterator(dir / L"objects"),
                              std::filesystem::directory_iterator{});
            db.erase(uniqueId);
            size_t objectsAfter =
                std::distance(std::filesystem::directory_iterator(dir / L"objects"),
                              std::filesystem::directory_iterator{});
            check(objectsAfter + 1 == objectsBefore,
                  "delete must release unreferenced object files");
            auto favorites = db.list(L"", 0, 100, true);
            check(favorites.size() == 1 && favorites.front().id == first,
                  "favorites view must preserve its own stable cursor");
            auto rejected = text(std::wstring(600000, L'x'));
            db.setLimits(1024 * 1024, 5ULL * 1024 * 1024 * 1024);
            bool sizeRejected = false;
            try {
                db.append(rejected);
            } catch (const std::exception &) {
                sizeRejected = true;
            }
            check(sizeRejected,
                  "configured capture limit must reject oversized items without deleting history");
            db.setLimits(32 * 1024 * 1024, 5ULL * 1024 * 1024 * 1024);
            desk::ClipPayload drop;
            desk::ClipFormat fileList;
            fileList.format = CF_HDROP;
            std::wstring filename = L"C:\\合成目录\\报价单.pdf";
            DROPFILES header{};
            header.pFiles = sizeof(header);
            header.fWide = TRUE;
            fileList.data.resize(sizeof(header) + (filename.size() + 2) * 2);
            memcpy(fileList.data.data(), &header, sizeof(header));
            memcpy(fileList.data.data() + sizeof(header), filename.c_str(),
                   (filename.size() + 1) * 2);
            drop.formats.push_back(fileList);
            desk::ClipFormat effect;effect.format=RegisterClipboardFormatW(L"Preferred DropEffect");
            effect.name=L"Preferred DropEffect";effect.data.resize(sizeof(DWORD));DWORD move=DROPEFFECT_MOVE;
            memcpy(effect.data.data(),&move,sizeof(move));drop.formats.push_back(effect);
            auto dropId=db.append(drop);
            auto restored=db.load(dropId);bool foundEffect=false;
            for(auto& f:restored.formats)if(f.name==effect.name)foundEffect=f.data==effect.data;
            check(foundEffect,"file history must preserve its original cut/move metadata");
            check(db.list(L"报价单").size() == 1,
                  "file-list history must be searchable by original filename");
            check(db.list(L"", 0, 100, false, L"文件").size() == 1,
                  "file type filter must apply before cursor pagination");
            check(db.list(L"another", 0, 100, false, L"文件").empty(),
                  "type and text filters must both apply");
        }
        {
            desk::HistoryStore reopened(dir);
            check(reopened.count() == 133, "restart must preserve history");
            reopened.erase(first);
            check(reopened.count() == 132, "delete must remove row");
        }
        {
            desk::HistoryStore backup(dir / L"backup");
            check(backup.count() == 2, "backup must preserve consistent records and blobs");
            check(!backup.load(first).formats.empty(), "backup must include referenced data");
        }
        std::filesystem::remove_all(dir);
        std::cout << "PASS history persistence, deduplication, Chinese search, paging, pin, format "
                     "round-trip and backup\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << "\n";
        return 1;
    }
}
