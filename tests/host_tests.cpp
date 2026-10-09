#define DESKFLOW_NO_ENTRYPOINT
#include "../src/app.cpp"
#include <iostream>
#include <future>
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static ClipPayload payload(const std::wstring &text) {
    ClipPayload p;
    ClipFormat f;
    f.format = CF_UNICODETEXT;
    f.data.resize((text.size() + 1) * 2);
    memcpy(f.data.data(), text.c_str(), f.data.size());
    p.formats.push_back(std::move(f));
    return p;
}
static void pumpUntil(const std::function<bool()> &done) {
    auto end = GetTickCount64() + 2500;
    while (!done() && GetTickCount64() < end) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message != WM_QUIT) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        Sleep(5);
    }
    require(done(), "host operation exceeded 2.5 seconds");
}
int main() {
    auto station = GetProcessWindowStation();
    auto desktop = GetThreadDesktop(GetCurrentThreadId());
    auto privateStation = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    if (!privateStation || !SetProcessWindowStation(privateStation)) {
        std::cerr << "FAIL cannot create isolated GUI station\n";
        return 1;
    }
    auto privateDesktop =
        CreateDesktopW(L"DeskFlowHostTests", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!privateDesktop || !SetThreadDesktop(privateDesktop)) {
        SetProcessWindowStation(station);
        CloseWindowStation(privateStation);
        std::cerr << "FAIL isolated desktop\n";
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&cc);
    auto data = std::filesystem::temp_directory_path() /
                (L"DeskFlow-host-test-" + std::to_wstring(GetCurrentProcessId()));
    int result = 0;
    try {
        auto broken=data/L"broken-module";std::filesystem::create_directories(broken);{std::ofstream bytes(broken/L"history.db",std::ios::binary);bytes<<"corrupt synthetic database";}
        {Application degraded(GetModuleHandleW(nullptr),broken,true);require(!degraded.history&&degraded.files,"a corrupt history module must preserve file search and host startup");require(std::filesystem::file_size(broken/L"history.db")>0,"corrupt original database must be preserved");}
        {
            Application app(GetModuleHandleW(nullptr), data, true);
            WNDCLASSW wc{};
            wc.hInstance = app.instance;
            wc.lpfnWndProc = Application::procedure;
            wc.lpszClassName = L"DeskFlowHostTest";
            RegisterClassW(&wc);
            auto hwnd = CreateWindowExW(0, wc.lpszClassName, L"Host tests", WS_OVERLAPPED, 0, 0,
                                        1000, 660, nullptr, nullptr, app.instance, &app);
            require(hwnd != nullptr, "host window creation");
            require(app.workerHandle==nullptr&&!app.workerRestartPending,
                    "isolated host tests must not start or elevate a real index worker");
            {
                auto failed=new Result;
                failed->type=11;failed->workerRestartFailed=true;
                failed->stoppedWorker=CreateEventW(nullptr,TRUE,FALSE,nullptr);
                require(failed->stoppedWorker!=nullptr,"worker handle fixture");
                auto retained=failed->stoppedWorker;
                app.workerRestartPending=true;
                app.receive(failed);
                require(!app.workerRestartPending&&app.workerHandle==retained&&
                        WaitForSingleObject(app.workerHandle,0)==WAIT_TIMEOUT,
                        "failed stop must retain the original worker handle without launching a second writer");
                CloseHandle(app.workerHandle);app.workerHandle=nullptr;
            }
            require(app.functionHint(0).find(L"Q")!=std::wstring::npos&&app.functionHint(1).find(L"W")!=std::wstring::npos&&app.functionHint(2).find(L"S")!=std::wstring::npos,"function hover hints must reflect current Alt Q/W/S keys");
            auto first = app.history->append(payload(L"旧历史内容"));
            app.mode = 1;
            app.historyRows = app.history->list();
            app.detailText = L"旧历史内容";
            restoreClipboardPayload(hwnd, payload(L"未修改的测试剪贴板"));
            auto before = GetClipboardSequenceNumber();
            SetWindowTextW(app.edit, L"没有匹配内容");
            app.activate(false);
            require(GetClipboardSequenceNumber() == before,
                    "new input before debounce must not activate old rows");
            app.erase();app.pin();
            std::promise<void> guardComplete;auto guardWait=guardComplete.get_future();
            app.clipboardTasks.add([&]{guardComplete.set_value();});
            require(guardWait.wait_for(std::chrono::seconds(2))==std::future_status::ready,
                    "clipboard mutation guard task must finish");
            require(app.history->count()==1&&!app.history->list().front().pinned,
                    "pending search must not delete or pin stale history rows");
            pumpUntil([&] { return !app.pendingQuery && app.historyRows.empty(); });
            require(app.detailText.empty() && app.previewImage == nullptr,
                    "empty search must clear stale preview");
            app.historyRows = app.history->list();
            app.pendingQuery = true;
            auto failure = new Result;
            failure->type = 9;
            failure->generation = app.generation;
            failure->status = L"查询失败";
            app.receive(failure);
            require(app.historyRows.empty(), "failed new query must not re-enable stale rows");
            app.recordingWarning=L"先前的存储错误";
            auto committed=new Result;committed->type=13;app.receive(committed);
            require(app.recordingWarning.empty(),"successful storage acknowledgement must clear the previous warning");
            auto paste = new Result;
            paste->type = 4;
            paste->payload = payload(L"只复制，目标已关闭");
            app.previousWindow = nullptr;
            app.receive(paste);
            require(app.status.find(L"未粘贴") != std::wstring::npos,
                    "missing paste target must produce visible feedback");
            auto captured = readClipboardPayload(hwnd);
            wchar_t exe[32768];
            GetModuleFileNameW(nullptr, exe, 32768);
            require(captured.source == std::filesystem::path(exe).filename().wstring(),
                    "clipboard source must come from owner, not foreground window");
            std::promise<void> started, release;
            auto gate = release.get_future().share();
            Tasks queue;
            queue.add([&] {
                started.set_value();
                gate.wait();
            });
            started.get_future().wait();
            bool bounded = true;
            for (int i = 0; i < 64; i++)
                bounded &= queue.add([] {});
            bool overflow = queue.add([] {});
            release.set_value();
            queue.shutdown();
            require(bounded && !overflow,
                    "bounded queue must report overflow without accepting unlimited work");
            app.mode=0;app.pendingQuery=false;app.fileRows.clear();
            for(int i=0;i<6;i++)app.fileRows.push_back({i+1,L"合成文件 "+std::to_wstring(i),L"C:\\DeskFlow-synthetic\\"+std::to_wstring(i)+L".txt",false,12});
            app.select(0);app.select(2,true);
            POINT click{(LONG)(70*app.scale),(LONG)((panel_layout::fileRowsTop+13)*app.scale)};
            SendMessageW(hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(click.x,click.y));
            require(app.fileSelection.size()==3,"mouse-down on selected group must preserve files for drag");
            SendMessageW(hwnd,WM_LBUTTONUP,0,MAKELPARAM(click.x,click.y));
            require(app.fileSelection.size()==1,"plain click release must collapse the group normally");
            app.select(2);auto refreshed=new Result;refreshed->type=0;refreshed->generation=app.generation;
            refreshed->files=app.fileRows;refreshed->files.insert(refreshed->files.begin(),{100,L"新增合成文件",L"C:\\DeskFlow-synthetic\\new.txt",false,1});
            app.receive(refreshed);
            require(app.selected==3&&app.fileAnchor==3,"refresh must remap both focus and Shift anchor by stable id");
            app.select(5,true);require(app.fileSelection.size()==3&&!app.fileSelection.contains(2),"Shift after refresh must not include unrelated preceding file");
            RECT client{};GetClientRect(hwnd,&client);
            require(app.rowAtPoint(70,client.bottom/app.scale-100)==-1,"footer action buttons must not hit file rows");
            before=GetClipboardSequenceNumber();
            SendMessageW(hwnd,WM_LBUTTONDBLCLK,0,MAKELPARAM((int)(70*app.scale),client.bottom-100));
            require(GetClipboardSequenceNumber()==before,"footer double click must not activate file selection");
            auto stale=new Result;stale->type=0;stale->generation=app.generation;stale->files={{999,L"迟到结果",L"C:\\late.txt",false,1}};
            app.hide();app.receive(stale);
            require(!app.fileRows.empty()&&app.fileRows.front().id!=999,"hidden panel must reject the old query response");
            require(app.previewImage==nullptr,"hidden panel must keep its preview released");
            BITMAPINFO imageInfo{};imageInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);imageInfo.bmiHeader.biWidth=512;imageInfo.bmiHeader.biHeight=-512;imageInfo.bmiHeader.biPlanes=1;imageInfo.bmiHeader.biBitCount=32;
            void* pixels=nullptr;auto image=CreateDIBSection(nullptr,&imageInfo,DIB_RGB_COLORS,&pixels,nullptr,0);
            require(image!=nullptr,"host image budget fixture creation");
            app.acceptCapturedImage(image,L"image");
            require(imageToolReservedBytes()==1024*1024,"Host must retain a lease for its original bitmap");
            auto competingImages=image_tools_detail::reserveMemory(image_tools_detail::imageWorkingBytes-2*1024*1024);
            require((bool)competingImages,"competing image fixture reservation");
            app.processImage(L"translate");
            require(!app.ocrBusy&&app.status.find(L"预算")!=std::wstring::npos,"Host must reject processing before allocating unbudgeted image copies");
            app.switchMode(0);competingImages.reset();
            require(imageToolReservedBytes()==0,"switching to files must release hidden capture images and leases");
            app.hide();
            require(imageToolReservedBytes()==0,"Host hiding must release all original/result image leases");
            app.quit();
        }
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << "\n";
        result = 1;
    }
    CoUninitialize();
    SetThreadDesktop(desktop);
    SetProcessWindowStation(station);
    CloseDesktop(privateDesktop);
    CloseWindowStation(privateStation);
    std::error_code ec;
    std::filesystem::remove_all(data, ec);
    if (!result)
        std::cout << "PASS native host input guard, preview clearing, clipboard owner, paste "
                     "failure and queue limits\n";
    return result;
}
