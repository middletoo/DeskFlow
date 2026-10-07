#pragma once
#include "common.hpp"
#include "search.hpp"
#include "json.hpp"
#include <deque>
#include <optional>

namespace desk {
struct ListCursor {
    int64_t history = 0;
    std::optional<SearchItem> file;
};
struct ListPage {
    int64_t number = 0;
    size_t count = 0;
    ListCursor cursor;
};

// Only keyset boundaries are spooled, never list contents. SQLite's unnamed
// temporary database is deleted on close; its page cache has a fixed budget.
class ListCheckpoints {
    std::unique_ptr<Database> database;
    Database& open() {
        if (!database) {
            database = std::make_unique<Database>(std::filesystem::path{});
            database->exec("PRAGMA journal_mode=OFF;PRAGMA synchronous=OFF;"
                "PRAGMA cache_size=-512;PRAGMA mmap_size=0;PRAGMA temp_store=FILE;"
                "CREATE TABLE cursors(page INTEGER PRIMARY KEY,value TEXT NOT NULL)");
        }
        return *database;
    }
  public:
    void clear() { database.reset(); }
    void put(int64_t page, const ListCursor& cursor) {
        nlohmann::json value{{"history", cursor.history}};
        if (cursor.file) {
            auto& file = *cursor.file;
            value["file"] = {{"id", file.id}, {"name", utf8(file.name)},
                {"path", utf8(file.path)}, {"folder", file.folder},
                {"size", file.size}, {"sizeKnown", file.sizeKnown}};
        }
        Statement insert(open(), "INSERT OR REPLACE INTO cursors(page,value) VALUES(?1,?2)");
        insert.bind(1, page);insert.bind(2, value.dump());insert.step();
    }
    ListCursor get(int64_t page) {
        if (page == 0) return {};
        Statement read(open(), "SELECT value FROM cursors WHERE page=?1");
        read.bind(1, page);
        if (!read.step()) throw std::runtime_error("滚动位置暂不可用，请刷新搜索");
        auto value = nlohmann::json::parse(read.text(0));
        ListCursor cursor;cursor.history = value.value("history", int64_t{});
        if (value.contains("file")) {
            auto& file = value["file"];SearchItem item;
            item.id = file.at("id").get<int64_t>();
            item.name = wide(file.at("name").get<std::string>());
            item.path = wide(file.at("path").get<std::string>());
            item.folder = file.at("folder").get<bool>();
            item.size = file.at("size").get<uint64_t>();
            item.sizeKnown = file.at("sizeKnown").get<bool>();
            cursor.file = std::move(item);
        }
        return cursor;
    }
    void eraseAfter(int64_t page) {
        if (!database) return;
        Statement erase(*database, "DELETE FROM cursors WHERE page>?1");
        erase.bind(1, page);erase.step();
    }
};
}
