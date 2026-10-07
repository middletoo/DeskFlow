#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace desk {
struct SearchItem {
    int64_t id = 0;
    std::wstring name, path;
    bool folder = false;
    uint64_t size = 0;
    bool sizeKnown = true;
    uint64_t modified = 0; // UTC Windows FILETIME; zero means unavailable.
};
enum class SearchSort { Name, Path, Size, Type, Id, Modified };
struct SearchQuery {
    std::wstring text;
    SearchSort sort = SearchSort::Name;
    bool descending = false;
    bool matchCase = false;
    bool matchPath = false;
    std::optional<SearchItem> after;
};
struct SearchStatus {
    int64_t total = 0;
    bool building = false;
    std::wstring message;
};
class SearchStore {
public:
    explicit SearchStore(const std::filesystem::path& dataDirectory);
    ~SearchStore();
    SearchStore(const SearchStore&) = delete;
    SearchStore& operator=(const SearchStore&) = delete;
    std::vector<SearchItem> query(const std::wstring& text, size_t limit = 100, int64_t beforeId = 0);
    std::vector<SearchItem> query(const SearchQuery& query, size_t limit = 100);
    bool queryPending() const;
    SearchStatus status();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int indexWorkerMain(const std::filesystem::path& dataDirectory,
                    const std::vector<std::wstring>& roots, DWORD parentPid);
bool stopIndexWorker(const std::filesystem::path& dataDirectory);
}
