#include "clipboard.hpp"
#include "common.hpp"
#include "json.hpp"
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
static constexpr uint64_t MiB = 1024 * 1024;
static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
static uint64_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                              sizeof(memory)) != 0, "Cannot measure private memory");
    return memory.PrivateUsage;
}
template<typename T> static T percentile(std::vector<T> values, double fraction) {
    if (values.empty()) return T{};
    std::sort(values.begin(), values.end());
    const size_t position = std::min(values.size() - 1,
        static_cast<size_t>(std::ceil(values.size() * fraction)) - 1);
    return values[position];
}
static uint64_t allocation(const std::filesystem::path& file) {
    DWORD high = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD low = GetCompressedFileSizeW(file.c_str(), &high);
    check(low != INVALID_FILE_SIZE || GetLastError() == ERROR_SUCCESS,
          "Cannot inspect fixture physical allocation");
    return (static_cast<uint64_t>(high) << 32) | low;
}
static void validateRows(const std::vector<desk::HistoryItem>& rows, const std::wstring& query,
                         int64_t beforeId, bool pinnedOnly) {
    check(rows.size() <= 100, "History query exceeded first-page bound");
    int64_t prior = beforeId ? beforeId : INT64_MAX;
    for (const auto& row : rows) {
        check(row.id < prior, "History cursor order repeated or skipped its boundary");
        check(!pinnedOnly || row.pinned, "Favorites query returned an unpinned row");
        check(query.empty() || row.preview.find(query) != std::wstring::npos,
              "History query returned a non-matching literal substring");
        prior = row.id;
    }
}

int wmain(int argc, wchar_t** argv) {
    try {
        check(argc >= 2, "Usage: history_benchmark.exe <synthetic-fixture-directory> [report.json]");
        const auto directory = std::filesystem::weakly_canonical(argv[1]);
        const auto marker = directory / L"fixture.json";
        std::ifstream input(marker);
        Json fixture;
        input >> fixture;
        check(fixture.value("owner", "") == "DeskFlow synthetic history benchmark fixture v1" &&
              fixture.value("complete", false), "Benchmark accepts only its complete synthetic fixture");
        check(fixture.value("textRecords", 0) >= 100000, "Fixture needs 100,000 searchable text rows");
        const auto reportPath = argc >= 3 ? std::filesystem::path(argv[2]) :
                                           directory.parent_path() / L"history-benchmark.json";
        const uint64_t baselineMemory = privateBytes();
        uint64_t objectBytes = 0, objectAllocatedBytes = 0, objectCount = 0;
        for (const auto& item : std::filesystem::directory_iterator(directory / L"objects")) {
            if (!item.is_regular_file()) continue;
            objectBytes += item.file_size();
            objectAllocatedBytes += allocation(item.path());
            ++objectCount;
        }
        check(objectBytes >= 2ULL * 1024 * MiB && objectAllocatedBytes >= 2ULL * 1024 * MiB,
              "Fixture must have at least 2 GiB of real object data");
        check(objectBytes == fixture.at("objectBytes").get<uint64_t>(), "Fixture object byte count differs");
        const auto openBegin = Clock::now();
        desk::HistoryStore store(directory);
        const double openMs = elapsed(openBegin);
        check(store.count() == fixture.at("totalRecords").get<int64_t>(), "Fixture row count differs");
        const auto firstBegin = Clock::now();
        auto firstPage = store.list(L"", 0, 100);
        const double firstPageMs = elapsed(firstBegin);
        validateRows(firstPage, L"", 0, false);
        check(firstPage.size() == 100, "Initial history page must contain 100 rows");
        const int64_t secondCursor = firstPage.back().id;
        auto secondPage = store.list(L"", secondCursor, 100);
        validateRows(secondPage, L"", secondCursor, false);
        check(secondPage.size() == 100, "Second page must expose older history");
        const int64_t textCursor = fixture.at("textRecords").get<int64_t>() + 1;
        auto textPage = store.list(L"", textCursor, 100);
        validateRows(textPage, L"", textCursor, false);
        check(textPage.size() == 100 && textPage.front().kind == L"文本", "Text page cursor failed");
        auto favorites = store.list(L"", 0, 100, true);
        validateRows(favorites, L"", 0, true);
        check(favorites.size() == 100, "Seeded favorites first page failed");
        const int64_t favoriteCursor = favorites.back().id;
        auto favoritePage = store.list(L"", favoriteCursor, 100, true);
        validateRows(favoritePage, L"", favoriteCursor, true);
        check(favoritePage.size() == 100, "Favorites cursor omitted older rows");

        struct Query {const char* name; std::wstring text; int64_t cursor; bool pinned; size_t expected;};
        const std::vector<Query> queries{
            {"first100", L"", 0, false, 100},
            {"page2", L"", secondCursor, false, 100},
            {"textPage", L"", textCursor, false, 100},
            {"chinese2Common", L"历史", 0, false, 100},
            {"chinese4Common", L"项目资料", 0, false, 100},
            {"selectiveTrigram", L"采购预算", 0, false, 100},
            {"uniqueTrigram", L"独有编号073421", 0, false, 1},
            {"literalWildcards", L"literal%_\\marker", 0, false, 100},
            {"chinese2NoMatch", L"鲸鲨", 0, false, 0},
            {"trigramNoMatch", L"从不存在的稀有样本", 0, false, 0},
            {"favorites", L"", 0, true, 100},
            {"favoritePage2", L"", favoriteCursor, true, 100},
        };
        Json results = Json::array();
        bool accepted = true;
        uint64_t maximumMemory = baselineMemory;
        constexpr size_t samples = 40, warmups = 3;
        for (const auto& query : queries) {
            std::vector<double> times;
            std::vector<uint64_t> memories;
            std::vector<std::string> errors;
            for (size_t iteration = 0; iteration < warmups + samples; ++iteration) {
                const auto begin = Clock::now();
                try {
                    auto rows = store.list(query.text, query.cursor, 100, query.pinned);
                    const double queryMs = elapsed(begin);
                    validateRows(rows, query.text, query.cursor, query.pinned);
                    check(rows.size() == query.expected, "Representative query returned an unexpected count");
                    if (iteration >= warmups) times.push_back(queryMs);
                } catch (const std::exception& error) {
                    errors.push_back(error.what());
                }
                const auto memory = privateBytes();
                maximumMemory = std::max(maximumMemory, memory);
                if (iteration >= warmups) memories.push_back(memory);
            }
            const double p95 = percentile(times, .95);
            const double targetMs = query.text.empty() ? 100 : 150;
            const bool passed = times.size() == samples && errors.empty() && p95 <= targetMs;
            accepted = accepted && passed;
            Json result{{"name", query.name}, {"query", desk::utf8(query.text)},
                        {"samples", times.size()}, {"warmups", warmups},
                        {"expectedRows", query.expected}, {"p50Ms", percentile(times, .50)},
                        {"p95Ms", p95}, {"targetP95Ms", targetMs},
                        {"privateBytesP50", percentile(memories, .50)},
                        {"privateBytesP95", percentile(memories, .95)},
                        {"passed", passed}, {"errors", errors}};
            std::cout << result.dump() << '\n';
            results.push_back(std::move(result));
        }
        // Exercise a genuine metadata write without leaving the fixture altered.
        store.setPinned(1, true);
        auto oldestPinned = store.list(L"", 2, 100, true);
        check(oldestPinned.size() == 1 && oldestPinned.front().id == 1, "Pin write was not queryable");
        store.setPinned(1, false);
        auto textPayload = store.load(73421);
        check(textPayload.formats.size() == 1 && textPayload.formats.front().format == CF_UNICODETEXT,
              "Valid Unicode payload round-trip failed");
        Json loadResults = Json::array();
        const int64_t imageIds[]{100001, 100512, 101024};
        for (const auto id : imageIds) {
            const auto begin = Clock::now();
            auto payload = store.load(id);
            const double loadMs = elapsed(begin);
            check(payload.formats.size() == 1 && payload.formats.front().format == CF_DIB,
                  "Sample image did not retain CF_DIB");
            const auto& bytes = payload.formats.front().data;
            check(bytes.size() >= sizeof(BITMAPINFOHEADER), "Sample DIB header missing");
            BITMAPINFOHEADER header{};
            memcpy(&header, bytes.data(), sizeof(header));
            check(header.biSize == 40 && header.biWidth == 1024 && header.biHeight == 512 &&
                  header.biBitCount == 32 && header.biCompression == BI_RGB &&
                  bytes.size() == 40 + 1024 * 512 * 4, "Sample DIB is not valid pixel data");
            const auto memory = privateBytes();
            maximumMemory = std::max(maximumMemory, memory);
            loadResults.push_back({{"id", id}, {"bytes", bytes.size()}, {"loadMs", loadMs},
                                   {"privateBytes", memory}});
        }
        accepted = accepted && maximumMemory <= 80 * MiB;
        Json report{
            {"scope", "Native HistoryStore only; warm query timings; synthetic on-disk history"},
            {"cacheConditions", "Three warmups per query; OS cache not flushed; first page is first-call, not a verified cold-cache run"},
            {"fixtureDirectory", desk::utf8(directory.wstring())}, {"fixture", fixture},
            {"sqliteNativeVersion", sqlite3_libversion()},
            {"objectCount", objectCount}, {"objectBytes", objectBytes},
            {"objectAllocatedBytes", objectAllocatedBytes}, {"databaseBytes", std::filesystem::file_size(directory / L"history.db")},
            {"baselinePrivateBytes", baselineMemory}, {"maximumPrivateBytes", maximumMemory},
            {"privateBytesTarget", 80 * MiB}, {"storeOpenMs", openMs},
            {"firstCallPageMs", firstPageMs}, {"queries", results}, {"samplePayloadLoads", loadResults},
            {"pagingValidated", true}, {"pinWriteValidated", true}, {"passed", accepted},
            {"limitations", "Does not measure Host plus index-worker memory, UI wake latency, idle CPU, cold OS cache, or 24-hour longevity"},
        };
        std::ofstream output(reportPath, std::ios::binary | std::ios::trunc);
        output << report.dump(2);
        output.close();
        check(output.good(), "Cannot save native benchmark report");
        std::cout << "REPORT " << desk::utf8(reportPath.wstring()) << '\n';
        return accepted ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
