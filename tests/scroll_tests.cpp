#define DESKFLOW_NO_ENTRYPOINT
#include "../src/app.cpp"
#include <iostream>
#include <future>

namespace desk::search_detail {
void registerShortGrams(sqlite3*);
bool prepareSortIndexes(sqlite3*, size_t);
}

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct ReleaseGuard {
    std::promise<void>& signal;
    ~ReleaseGuard() { try { signal.set_value(); } catch (...) {} }
};
static void pumpUntil(const std::function<bool()>& done) {
    auto deadline = GetTickCount64() + 5000;
    while (!done() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            if (message.message != WM_QUIT) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        Sleep(5);
    }
    require(done(), "scroll operation timed out");
}
static ClipPayload historyPayload(int value) {
    ClipPayload payload;
    auto text = L"Synthetic history " + std::to_wstring(value);
    ClipFormat format;format.format = CF_UNICODETEXT;
    format.data.resize((text.size() + 1) * sizeof(wchar_t));
    memcpy(format.data.data(), text.c_str(), format.data.size());
    payload.formats.push_back(std::move(format));
    return payload;
}
static void seedFiles(const std::filesystem::path& directory, int count) {
    sqlite3* database = nullptr;
    require(sqlite3_open(utf8((directory / L"files.db").wstring()).c_str(), &database) == SQLITE_OK,
            "open synthetic index");
    desk::search_detail::registerShortGrams(database);
    require(sqlite3_exec(database, "INSERT INTO search_roots(id,path,path_key,complete) VALUES(1,'c:\\synthetic','c:\\synthetic',1); BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK,
            "start synthetic index fixture");
    sqlite3_stmt* insert = nullptr;
    require(sqlite3_prepare_v2(database, "INSERT INTO files(root_id,path,path_fold,name,name_fold,folder,size,seen,size_known) VALUES(1,?1,?1,?2,?2,0,?3,1,1)", -1, &insert, nullptr) == SQLITE_OK,
            "prepare synthetic file insertion");
    for (int i = 0; i < count; ++i) {
        // Deliberately scramble insertion IDs so continuation must use the complete sort cursor.
        auto number = std::to_string((i * 73) % count);
        auto name = "row-" + std::string(5 - number.size(), '0') + number + ".txt";
        auto path = "c:\\synthetic\\" + name;
        sqlite3_bind_text(insert, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert, 3, i);
        require(sqlite3_step(insert) == SQLITE_DONE, "insert synthetic file");
        sqlite3_reset(insert);sqlite3_clear_bindings(insert);
    }
    sqlite3_finalize(insert);
    require(sqlite3_exec(database, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK, "commit synthetic files");
    while (desk::search_detail::prepareSortIndexes(database, 1024)) {}
    sqlite3_close(database);
}
static void wheel(Application& app, int lines) {
    auto ticks = std::max(1, abs(lines));
    for (int i = 0; i < ticks; ++i)
        SendMessageW(app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, lines > 0 ? (WORD)-WHEEL_DELTA : WHEEL_DELTA), 0);
}

int main() {
    auto originalStation = GetProcessWindowStation();
    auto originalDesktop = GetThreadDesktop(GetCurrentThreadId());
    auto privateStation = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    if (!privateStation || !SetProcessWindowStation(privateStation)) return 1;
    auto privateDesktop = CreateDesktopW(L"DeskFlowScrollTests", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!privateDesktop || !SetThreadDesktop(privateDesktop)) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};InitCommonControlsEx(&controls);
    auto directory = std::filesystem::temp_directory_path() /
        (L"DeskFlow-scroll-tests-" + std::to_wstring(GetCurrentProcessId()));
    int result = 0;
    try {
        Application app(GetModuleHandleW(nullptr), directory, true);
        WNDCLASSW wc{};wc.hInstance = app.instance;wc.lpfnWndProc = Application::procedure;
        wc.lpszClassName = L"DeskFlowScrollTests";RegisterClassW(&wc);
        require(CreateWindowExW(0, wc.lpszClassName, L"Scroll tests", WS_OVERLAPPED,
            0, 0, 1000, 660, nullptr, nullptr, app.instance, &app) != nullptr, "create isolated scroll window");
        KillTimer(app.window, 9);KillTimer(app.window, 2);
        for (int i = 0; i < 357; ++i) app.history->append(historyPayload(i));
        app.mode = 1;app.query();
        pumpUntil([&] { return !app.pendingQuery && app.historyRows.size() == 100; });
        UINT wheelLines = 3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
        int expectedWheelRows = wheelLines == WHEEL_PAGESCROLL ? app.visibleRows() : (int)wheelLines;
        app.scroll = 12;
        for (int i = 0; i < 3; ++i) SendMessageW(app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, 30), 0);
        require(app.scroll == 12, "partial wheel delta must be accumulated instead of dropping or amplifying it");
        SendMessageW(app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, 30), 0);
        require(app.scroll == std::max(0, 12 - expectedWheelRows), "four 30-unit deltas must equal one full wheel step");
        int beforeCancel = app.scroll;
        SendMessageW(app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)-60), 0);
        SendMessageW(app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, 60), 0);
        require(app.scroll == beforeCancel && app.wheelRemainder == 0,
                "opposite half-step wheel deltas must cancel");
        wheel(app, 99);
        pumpUntil([&] { return app.historyRows.size() > 100; });
        require(app.historyRows.front().id == 357, "scroll continuation replaced the first history page");
        require(!app.pendingQuery, "continuation must leave existing history actions usable");
        while (!app.listEnd) {
            wheel(app, 300);
            pumpUntil([&] { return !app.listInFlight; });
        }
        require(app.historyRows.size() == 357, "history scrolling omitted rows after page three");
        for (int i = 0; i < 357; ++i)
            require(app.historyRows[i].id == 357 - i, "history scrolling duplicated or reordered rows");
        auto terminalRequest = app.listRequest;
        wheel(app, 30);wheel(app, 30);
        require(app.listRequest == terminalRequest, "end-of-results wheel must not repeatedly query");
        app.scroll = 80;app.select(83);
        auto topHistoryId = app.historyRows[app.scroll].id;
        auto selectedHistoryId = app.historyRows[app.selected].id;
        app.history->append(historyPayload(358));
        app.query(false, true);
        pumpUntil([&] { return !app.listInFlight; });
        require(app.historyRows.size() == 357 && app.historyRows[app.scroll].id == topHistoryId,
                "background history refresh must retain the deep browsing window");
        app.query();
        pumpUntil([&] { return !app.listInFlight; });
        require(app.historyRows.size() == 358 && app.historyRows[app.scroll].id == topHistoryId &&
                app.historyRows[app.selected].id == selectedHistoryId,
                "head insertion must preserve history viewport and selected ID");

        constexpr int fileCount = 1603;
        seedFiles(directory, fileCount);
        app.mode = 0;app.query(true);
        pumpUntil([&] { return !app.pendingQuery && !app.listInFlight; });
        require(app.fileRows.size() == 100, "first file batch must remain bounded");
        auto firstFile = app.fileRows.front();
        app.select(0);
        // A repeated row can arrive when an index changes during a keyset fetch.
        // Feed a realistic overlapping batch through the actual host receiver.
        SearchQuery next;next.after = app.fileRows.back();
        auto continuation = app.files->query(next, 100);
        while (app.files->queryPending()) continuation = app.files->query(next, 100);
        auto overlap = new Result;overlap->type = 0;overlap->generation = app.generation;
        overlap->listAction = 2;overlap->listRequest = app.listRequest;
        overlap->files.push_back(app.fileRows.back());
        overlap->files.insert(overlap->files.end(), continuation.begin(), continuation.end());
        ListPage mark;mark.number = 1;mark.count = overlap->files.size();mark.cursor.file = app.fileRows.back();
        overlap->listPages.push_back(mark);app.receive(overlap);
        require(app.fileRows.size() == 200, "overlapping continuation must be deduplicated by stable ID");
        require(app.fileRows[app.selected].id == firstFile.id && app.fileAnchor == 0,
                "appending a batch must preserve focus and Shift anchor");
        while (!app.listEnd) {
            wheel(app, 1200);
            pumpUntil([&] { return !app.listInFlight; });
            require(app.fileRows.size() <= Application::listRowLimit,
                    "long scrolling must not keep every visited file row in memory");
            std::set<int64_t> ids;
            for (size_t i = 0; i < app.fileRows.size(); ++i) {
                require(ids.insert(app.fileRows[i].id).second, "duplicate in retained file window");
                if (i) require(app.fileRows[i - 1].name < app.fileRows[i].name,
                               "file keyset continuation changed global name order");
            }
        }
        require(app.listPages.front().number > 0 && app.fileRows.back().name == L"row-01602.txt",
                "file scrolling must reach the final row while evicting the oldest pages");
        require(app.selectedFilePaths() == std::vector<std::wstring>{firstFile.path},
                "evicting the selected page must keep the selection attached to the same file ID");
        auto rowsBeforeRefresh = app.fileRows;
        auto topFileId = app.fileRows[app.scroll].id;
        app.query(false, true);
        pumpUntil([&] { return !app.listInFlight; });
        require(app.fileRows.size() == rowsBeforeRefresh.size() && app.fileRows[app.scroll].id == topFileId,
                "periodic file refresh must preserve loaded rows and scroll position");
        while (app.listPages.front().number > 0) {
            auto firstPage = app.listPages.front().number;
            wheel(app, -1200);
            pumpUntil([&] { return !app.listInFlight && app.listPages.front().number < firstPage; });
            require(app.fileRows.size() <= Application::listRowLimit,
                    "backward cache restoration exceeded the memory window");
        }
        require(app.fileRows.front().id == firstFile.id,
                "scrolling backwards must restore evicted pages from keyset checkpoints");
        require(app.selected >= 0 && app.fileRows[app.selected].id == firstFile.id,
                "backward restoration must restore focus by ID instead of selecting an unrelated row");

        // Hold a real queued continuation until after EN_CHANGE replaces the query.
        std::promise<void> started, release;
        ReleaseGuard ensureRelease{release};
        auto gate = release.get_future().share();
        app.queryTasks.add([&] { started.set_value();gate.wait(); });
        require(started.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "start queued continuation fixture");
        app.scroll = (int)app.fileRows.size() - app.visibleRows();
        app.continueList();
        require(app.listInFlight && !app.pendingQuery && !app.selectedFilePaths().empty(),
                "in-flight append must keep current file selection usable");
        int oldGeneration = app.generation, oldRequest = app.listRequest;
        app.query(false, true);
        require(app.generation == oldGeneration,
                "automatic refresh must not cancel a still-running continuation");
        SetWindowTextW(app.edit, L"no-synthetic-match");
        require(app.pendingQuery && app.selectedFilePaths().empty(), "changed input must guard stale file actions");
        app.query(true);
        int pendingGeneration = app.generation;
        app.query(false, true);
        require(app.generation == pendingGeneration && app.pendingQuery,
                "automatic timer must not cancel a slow initial query while it has no rows");
        release.set_value();
        pumpUntil([&] { return !app.pendingQuery && !app.listInFlight; });
        auto late = new Result;late->type = 0;late->generation = oldGeneration;
        late->listAction = 2;late->listRequest = oldRequest;late->files = {firstFile};
        app.receive(late);
        require(app.fileRows.empty(), "late continuation must not enter the replacement query");
        SetWindowTextW(app.edit, L"");
        pumpUntil([&] { return !app.pendingQuery && !app.listInFlight; });
        app.fileFilter = 2;app.resetFilePage();
        pumpUntil([&] { return !app.pendingQuery && !app.listInFlight; });
        require(app.fileRows.empty() && app.scroll == 0, "filter change must reset the cursor and cached rows");
        app.fileFilter = 0;app.changeFileSort(SearchSort::Name);
        pumpUntil([&] { return !app.pendingQuery && !app.listInFlight; });
        require(app.fileRows.size() == 100 && app.fileRows.front().name == L"row-01602.txt",
                "sort change must reset continuation and use the new global direction");
        app.quit();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << "\n";result = 1;
    }
    CoUninitialize();SetThreadDesktop(originalDesktop);SetProcessWindowStation(originalStation);
    CloseDesktop(privateDesktop);CloseWindowStation(privateStation);
    std::error_code error;std::filesystem::remove_all(directory, error);
    if (!result) std::cout << "PASS bounded file/history scrolling\n";
    return result;
}
