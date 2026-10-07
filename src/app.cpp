#include "common.hpp"
#include "clipboard.hpp"
#include "search.hpp"
#include "capture.hpp"
#include "translation.hpp"
#include "settings.hpp"
#include "github_icon.hpp"
#include "accessibility.hpp"
#include "file_actions.hpp"
#include "image_tools.hpp"
#include "recording.hpp"
#include "panel_layout.hpp"
#include "clipboard_preview.hpp"
#include "scroll_window.hpp"
#include "json.hpp"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <psapi.h>
#include <fstream>
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <algorithm>
#include <memory>
#include <sstream>
#include <appmodel.h>
#include <wincodec.h>
#include <set>
#include <map>
#include <optional>
#include <cmath>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <ctime>
using Microsoft::WRL::ComPtr;
using namespace desk;
constexpr UINT DoneMessage = WM_APP + 1, TrayMessage = WM_APP + 2;
class Tasks {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    bool stop = false;
    std::thread thread;

  public:
    Tasks()
        : thread([this] {
              for (;;) {
                  std::function<void()> job;
                  {
                      std::unique_lock l(mutex);
                      cv.wait(l, [&] { return stop || !tasks.empty(); });
                      if (stop)
                          return;
                      job = std::move(tasks.front());
                      tasks.pop_front();
                  }
                  try {
                      job();
                  } catch (...) {
                  }
              }
          }) {}
    ~Tasks() {
        shutdown();
    }
    bool add(std::function<void()> job, bool replace = false) {
        std::lock_guard l(mutex);
        if (stop)
            return false;
        if (replace)
            tasks.clear();
        if (tasks.size() >= 64)
            return false;
        tasks.push_back(std::move(job));
        cv.notify_one();
        return true;
    }
    void shutdown() {
        {
            std::lock_guard l(mutex);
            stop = true;
            tasks.clear();
        }
        cv.notify_all();
        if (thread.joinable())
            thread.join();
    }
};
struct Result {
    image_tools_detail::MemoryLease imageMemory;
    HWND pasteTarget = nullptr;
    DWORD pastePid = 0;
    int type = 0, generation = 0;
    int listAction = 0, listRequest = 0;
    bool listEnd = false;
    int64_t listTotal = 0;
    std::vector<ListPage> listPages;
    std::wstring status, text;
    std::vector<HistoryItem> history;
    std::vector<SearchItem> files;
    SearchStatus index;
    ClipPayload payload;
    HBITMAP bitmap = nullptr;
    ~Result() {
        if (bitmap)
            DeleteObject(bitmap);
    }
};
static std::wstring controlText(HWND window) {
    int n = GetWindowTextLengthW(window);
    std::wstring text(n + 1, 0);
    GetWindowTextW(window, text.data(), n + 1);
    text.resize(n);
    return text;
}
static bool currentInputDesktop() {
    auto input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    if(!input)return false;
    wchar_t active[256]{},current[256]{};DWORD bytes=0;
    bool same=GetUserObjectInformationW(input,UOI_NAME,active,sizeof(active),&bytes)&&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,current,sizeof(current),&bytes)&&wcscmp(active,current)==0;
    CloseDesktop(input);return same;
}
static std::optional<std::wstring> askFileName(HWND owner, const std::wstring& existing) {
    struct Prompt {
        HWND edit = nullptr;bool done = false;std::optional<std::wstring> result;
        static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
            auto state=(Prompt*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
            if(msg==WM_NCCREATE){state=(Prompt*)((CREATESTRUCTW*)lp)->lpCreateParams;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)state);}
            if(!state)return DefWindowProcW(hwnd,msg,wp,lp);
            if(msg==WM_COMMAND&&LOWORD(wp)==IDOK){state->result=controlText(state->edit);DestroyWindow(hwnd);return 0;}
            if(msg==WM_COMMAND&&LOWORD(wp)==IDCANCEL||msg==WM_CLOSE){DestroyWindow(hwnd);return 0;}
            if(msg==WM_DESTROY){state->done=true;return 0;}
            return DefWindowProcW(hwnd,msg,wp,lp);
        }
    } state;
    auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=Prompt::procedure;wc.lpszClassName=L"DeskFlowRename";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
    RECT r{};GetWindowRect(owner,&r);
    auto prompt=CreateWindowExW(WS_EX_DLGMODALFRAME,wc.lpszClassName,L"重命名文件",WS_POPUP|WS_CAPTION|WS_SYSMENU,
        r.left+90,r.top+100,470,170,owner,nullptr,instance,&state);
    if(!prompt)return {};
    auto label=CreateWindowW(L"STATIC",L"新名称（包含扩展名）",WS_CHILD|WS_VISIBLE,20,18,410,24,prompt,nullptr,instance,nullptr);
    state.edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",existing.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,20,46,420,28,prompt,(HMENU)1,instance,nullptr);
    auto ok=CreateWindowW(L"BUTTON",L"重命名",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,256,91,87,29,prompt,(HMENU)IDOK,instance,nullptr);
    auto cancel=CreateWindowW(L"BUTTON",L"取消",WS_CHILD|WS_VISIBLE|WS_TABSTOP,352,91,87,29,prompt,(HMENU)IDCANCEL,instance,nullptr);
    for(auto control:{label,state.edit,ok,cancel})SendMessageW(control,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);
    EnableWindow(owner,FALSE);ShowWindow(prompt,SW_SHOW);SetFocus(state.edit);
    auto dot=existing.find_last_of(L'.');SendMessageW(state.edit,EM_SETSEL,0,dot==std::wstring::npos?-1:(LPARAM)dot);
    MSG message{};int quitCode=0;bool quit=false;
    while(!state.done){
        if(!GetMessageW(&message,nullptr,0,0)){quit=true;quitCode=(int)message.wParam;break;}
        if(message.message==WM_KEYDOWN&&message.wParam==VK_RETURN){SendMessageW(prompt,WM_COMMAND,IDOK,0);continue;}
        if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){SendMessageW(prompt,WM_COMMAND,IDCANCEL,0);continue;}
        if(!IsDialogMessageW(prompt,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    if(IsWindow(prompt))DestroyWindow(prompt);EnableWindow(owner,TRUE);SetForegroundWindow(owner);
    if(quit)PostQuitMessage(quitCode);
    return state.result;
}
static std::wstring historyTime(int64_t milliseconds) {
    time_t seconds = milliseconds / 1000;
    tm local{};
    if (localtime_s(&local, &seconds))
        return L"";
    wchar_t value[32];
    wcsftime(value, 32, L"%Y-%m-%d %H:%M", &local);
    return value;
}
static std::filesystem::path executableDirectory() {
    wchar_t path[32768];
    GetModuleFileNameW(nullptr, path, 32768);
    return std::filesystem::path(path).parent_path();
}
static std::wstring folderDialog(HWND owner) {
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return {};
    DWORD flags;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (FAILED(dialog->Show(owner)))
        return {};
    ComPtr<IShellItem> item;
    dialog->GetResult(&item);
    PWSTR path = nullptr;
    item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    std::wstring out = path;
    CoTaskMemFree(path);
    return out;
}
static HBITMAP historyBitmap(const ClipPayload &payload,int maximumDimension=0) {
    for (auto &f : payload.formats)
        if ((f.format == CF_DIB || f.format == CF_DIBV5) &&
            f.data.size() >= sizeof(BITMAPINFOHEADER)) {
            BITMAPINFOHEADER header{};
            memcpy(&header, f.data.data(), sizeof(header));
            if (header.biSize < 40 || header.biSize > f.data.size() || header.biWidth <= 0 ||
                header.biWidth > 16000 || header.biHeight == 0 ||
                abs((int64_t)header.biHeight) > 16000 || header.biPlanes != 1 ||
                (header.biBitCount != 24 && header.biBitCount != 32) ||
                (header.biCompression != BI_RGB &&
                 !(header.biCompression == BI_BITFIELDS && header.biBitCount == 32)) ||
                header.biClrUsed > 256)
                continue;
            size_t offset = header.biSize,
                   stride = ((size_t)header.biWidth * header.biBitCount + 31) / 32 * 4;
            if (header.biCompression == BI_BITFIELDS && header.biSize == 40)
                offset += 12;
            offset += (size_t)header.biClrUsed * 4;
            if (offset + stride * abs((int64_t)header.biHeight) > f.data.size())
                continue;
            HDC dc = GetDC(nullptr);
            if(maximumDimension>0&&std::max<int64_t>(header.biWidth,abs((int64_t)header.biHeight))>maximumDimension){
                double ratio=(double)maximumDimension/std::max<int64_t>(header.biWidth,abs((int64_t)header.biHeight));
                int width=std::max(1,(int)(header.biWidth*ratio)),height=std::max(1,(int)(abs((int64_t)header.biHeight)*ratio));
                BITMAPINFO thumbnailInfo{};thumbnailInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);thumbnailInfo.bmiHeader.biWidth=width;thumbnailInfo.bmiHeader.biHeight=-height;thumbnailInfo.bmiHeader.biPlanes=1;thumbnailInfo.bmiHeader.biBitCount=32;
                void* pixels{};auto bitmap=CreateDIBSection(dc,&thumbnailInfo,DIB_RGB_COLORS,&pixels,nullptr,0);auto target=CreateCompatibleDC(dc);
                if(bitmap&&target){auto old=SelectObject(target,bitmap);SetStretchBltMode(target,HALFTONE);SetBrushOrgEx(target,0,0,nullptr);
                    auto copied=StretchDIBits(target,0,0,width,height,0,0,header.biWidth,(int)abs((int64_t)header.biHeight),f.data.data()+offset,(BITMAPINFO*)f.data.data(),DIB_RGB_COLORS,SRCCOPY);
                    SelectObject(target,old);DeleteDC(target);ReleaseDC(nullptr,dc);if(copied!=GDI_ERROR&&copied!=0)return bitmap;DeleteObject(bitmap);
                }else{if(bitmap)DeleteObject(bitmap);if(target)DeleteDC(target);ReleaseDC(nullptr,dc);}continue;
            }
            auto bitmap =
                CreateDIBitmap(dc, (const BITMAPINFOHEADER *)f.data.data(), CBM_INIT,
                               f.data.data() + offset, (BITMAPINFO *)f.data.data(), DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            return bitmap;
        }
    return nullptr;
}
class Application {
  public:
    HINSTANCE instance;
    HWND window = nullptr, edit = nullptr, resultEdit = nullptr, settingsWindow = nullptr;
    HWND menuTooltip=nullptr;
    std::array<std::wstring,4> menuHints;
    float scale = 1;
    int mode = 0, selected = 0, scroll = 0, wheelRemainder = 0;
    bool paused = false, smoke = false;
    UINT testDurationMs = 4000;
    std::atomic_bool closing = false, cancelTranslation = false;
    std::atomic_int generation = 0;
    std::atomic_bool clipboardQueued = false, ocrBusy = false;
    std::atomic_int previewGeneration = 0, captureGeneration = 0;
    bool pendingQuery = false, favoritesOnly = false;
    SearchQuery fileQuery;
    static constexpr size_t listPageSize = 100, listRowLimit = 1000;
    ListCheckpoints listCheckpoints;
    std::deque<ListPage> listPages;
    bool listInFlight = false, listEnd = false, listHeadDirty = false;
    int listRequest = 0;
    int64_t listTotal = 0;
    std::string listIdentity;
    SearchQuery activeFileQuery;
    std::wstring activeHistoryInput, activeHistoryKind;
    bool activeFavorites = false;
    int64_t listFocusedId = 0, fileAnchorId = 0;
    std::set<int> fileSelection;
    std::map<int64_t, SearchItem> fileSelectedItems;
    int fileAnchor = 0, fileFilter = 0, historyFilter = 0;
    bool filePreviewVisible = false, fileRegex = false, draggingFiles = false;
    bool fileDetailsVisible = false, historyPreviewRequested = false;
    FloatingClipboardPreview floatingPreview;
    bool collapseFileSelectionOnMouseUp = false;
    POINT fileMouseDown{};
    std::shared_ptr<std::atomic_bool> imageCancellation = std::make_shared<std::atomic_bool>(false);
    std::filesystem::path data;
    Settings config;
    std::unique_ptr<HistoryStore> history;
    std::unique_ptr<SearchStore> files;
    std::vector<HistoryItem> historyRows;
    std::vector<SearchItem> fileRows;
    SearchStatus indexStatus;
    std::wstring status = L"随时搜索，随手留存", detailText = L"";
    HWND previousWindow = nullptr;
    DWORD ignoredSequence = 0;
    HBITMAP originalImage = nullptr, resultImage = nullptr, previewImage = nullptr;
    image_tools_detail::MemoryLease originalMemory, resultMemory;
    bool showOriginal = false;
    Tasks queryTasks, previewTasks, clipboardReaderTasks, clipboardTasks, ocrTasks;
    std::atomic_size_t pendingClipboardBytes{0};
    std::wstring recordingWarning, hotkeyWarning;
    std::wstring historyFailure, searchFailure;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1PathGeometry> githubGeometry;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1DCRenderTarget> dcTarget;
    ComPtr<ID2D1SolidColorBrush> brush;
    HFONT uiFont = nullptr;
    NOTIFYICONDATAW tray{};
    HANDLE workerHandle = nullptr;
    HANDLE setupHandle = nullptr;
    std::filesystem::path setupReceipt;
    DWORD taskbarMessage = 0;
    HWINEVENTHOOK foregroundHook = nullptr;
    inline static Application *foregroundTracker = nullptr;
    ULONGLONG startTime = GetTickCount64(), firstFrameMs = 0;
    HRESULT lastDrawResult = S_OK;
    HRESULT lastTargetResult = S_OK;
    std::vector<std::pair<D2D1_RECT_F, std::function<void()>>> buttons;
    explicit Application(HINSTANCE inst, std::filesystem::path dir, bool test,
                         UINT lifetimeMs = 4000)
        : instance(inst), data(std::move(dir)), smoke(test) {
        std::filesystem::create_directories(data);
        testDurationMs = lifetimeMs;
        try {
            config = loadSettings(data / L"settings.json");
        } catch (const std::exception &e) {
            status = userError(e);
        }
        try {
            auto preferencePath = data / L"search-ui.json";
            if (std::filesystem::exists(preferencePath) && std::filesystem::file_size(preferencePath) <= 64 * 1024) {
                std::ifstream input(preferencePath);nlohmann::json prefs;input >> prefs;
                int sort = prefs.value("sort", (int)SearchSort::Name);
                if (sort >= 0 && sort <= (int)SearchSort::Modified) fileQuery.sort = (SearchSort)sort;
                fileQuery.descending = prefs.value("descending", false);
                fileQuery.matchCase = prefs.value("matchCase", false);
                fileQuery.matchPath = prefs.value("matchPath", false);
                filePreviewVisible = prefs.value("floatingPreview", false);
                fileDetailsVisible = prefs.value("details", false);
            }
        } catch (...) { status = L"搜索偏好暂未载入，仍可正常搜索"; }
        try {
            history = std::make_unique<HistoryStore>(data);
            history->setLimits((size_t)config.maximumEntryMiB * 1024 * 1024,
                               (uint64_t)config.imageQuotaGiB * 1024 * 1024 * 1024);
        } catch (const std::exception &e) {
            historyFailure =
                L"历史库无法打开，原文件已保留。请在设置中打开数据目录，从完整备份恢复。详情：" +
                userError(e);
            recordingWarning = historyFailure;
        }
        try {
            files = std::make_unique<SearchStore>(data);
        } catch (const std::exception &e) {
            searchFailure =
                L"文件索引暂时不可用，请保留历史数据并重建文件索引。详情：" + userError(e);
        }
        previousWindow = GetForegroundWindow();
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
        if(factory)makeGithubGeometry(factory.Get(),githubGeometry.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            (IUnknown **)writeFactory.GetAddressOf());
    }
    ~Application() {
        if (setupHandle)
            CloseHandle(setupHandle);
        closing = true;
        if (foregroundHook)
            UnhookWinEvent(foregroundHook);
        if (foregroundTracker == this)
            foregroundTracker = nullptr;
        if (IsWindow(window)) {
            RemoveClipboardFormatListener(window);
            for (int i = 1; i <= 3; i++)
                UnregisterHotKey(window, i);
            Shell_NotifyIconW(NIM_DELETE, &tray);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            DestroyWindow(window);
        }
        cancelTranslation = true;
        *imageCancellation = true;
        stopIndexWorker(data);
        shutdownRecording();
        queryTasks.shutdown();
        previewTasks.shutdown();
        clipboardReaderTasks.shutdown();
        clipboardTasks.shutdown();
        ocrTasks.shutdown();
        if (workerHandle)
            CloseHandle(workerHandle);
        for (auto h : {originalImage, resultImage, previewImage})
            if (h)
                DeleteObject(h);
        if (uiFont)
            DeleteObject(uiFont);
    }
    bool dark() const {
        return config.dark;
    }
    D2D1_COLOR_F color(unsigned rgb, float a = 1) {
        return D2D1::ColorF(rgb, a);
    }
    void rect(D2D1_RECT_F r, unsigned rgb, float radius = 0) {
        brush->SetColor(color(rgb));
        if (radius > 0)
            target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
        else
            target->FillRectangle(r, brush.Get());
    }
    void text(const std::wstring &s, D2D1_RECT_F r, float size, unsigned rgb, bool strong = false,bool wrap=true,bool rightAligned=false) {
        ComPtr<IDWriteTextFormat> f;
        writeFactory->CreateTextFormat(
            L"Segoe UI", nullptr, strong ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &f);
        if (!f)
            return;
        f->SetWordWrapping(wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);
        if(rightAligned)f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        ComPtr<IDWriteInlineObject> ellipsis;
        writeFactory->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trimming, ellipsis.Get());
        brush->SetColor(color(rgb));
        target->DrawTextW(s.data(), (UINT)s.size(), f.Get(), r, brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void button(const std::wstring &label, D2D1_RECT_F r, std::function<void()> action,
                bool primary = false) {
        rect(r, primary ? 0x5865e9 : (dark() ? 0x282b35 : 0xeef0f6), 8);
        text(label, {r.left + 13, r.top + 8, r.right - 7, r.bottom}, 13,
             primary ? 0xffffff : (dark() ? 0xd8dbe5 : 0x424957), true);
        buttons.push_back({r, std::move(action)});
    }
    void menuButton(const std::wstring& label,D2D1_RECT_F bounds,std::function<void()> action,bool active=false){
        if(active)rect(bounds,dark()?0x303852:0xe8f1fb);
        text(label,{bounds.left+9,bounds.top+6,bounds.right-3,bounds.bottom-2},13,dark()?0xededf1:0x202735,false,false);
        buttons.push_back({bounds,std::move(action)});
    }
    void githubButton(float width){
        const D2D1_RECT_F bounds{width-38,0,width-8,30};
        if(githubGeometry){
            D2D1_MATRIX_3X2_F previous{};target->GetTransform(&previous);
            target->SetTransform(D2D1::Matrix3x2F::Scale(.75f,.75f)*
                D2D1::Matrix3x2F::Translation(bounds.left+6,6)*previous);
            brush->SetColor(color(dark()?0xededf1:0x24292f));target->FillGeometry(githubGeometry.Get(),brush.Get());
            target->SetTransform(previous);
        }
        buttons.push_back({bounds,[this]{
            if((INT_PTR)ShellExecuteW(window,L"open",githubProjectUrl,nullptr,nullptr,SW_SHOWNORMAL)<=32){
                status=L"无法打开 GitHub 项目，请检查默认浏览器";invalidate();
            }
        }});
    }
    void publish(Result *r) {
        if (closing || !PostMessageW(window, DoneMessage, 0, (LPARAM)r))
            delete r;
    }
    void error(const std::exception &e) {
        auto r = new Result;
        r->type = 2;
        r->status = userError(e);
        publish(r);
    }
    void invalidate() {
        if (IsWindowVisible(window))
            InvalidateRect(window, nullptr, FALSE);
    }
    void makeTarget() {
        if (target)
            return;
        // Present through the paint DC when the desktop's HWND presentation
        // path is unavailable; Direct2D and DirectWrite still draw the panel.
        auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        lastTargetResult = factory->CreateDCRenderTarget(&properties, &dcTarget);
        if (dcTarget)
            target = dcTarget;
        if (target) {
            target->SetDpi(96, 96);
            target->CreateSolidColorBrush(color(0), &brush);

        }
    }
    void resetTarget() {
        brush.Reset();
        target.Reset();
        dcTarget.Reset();
    }
    void paintBitmap(HBITMAP bitmap, D2D1_RECT_F bounds) {
        if (!bitmap)
            return;
        BITMAP b{};
        GetObject(bitmap, sizeof(b), &b);
        if (b.bmWidth <= 0 || b.bmHeight <= 0)
            return;
        float factor = std::min((bounds.right - bounds.left) / b.bmWidth,
                                (bounds.bottom - bounds.top) / b.bmHeight);
        int w = std::max(1, (int)(b.bmWidth * factor)), h = std::max(1, (int)(b.bmHeight * factor));
        BITMAPINFO info{};
        info.bmiHeader.biSize = 40;
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *pixels = nullptr;
        HDC screen = GetDC(nullptr), source = CreateCompatibleDC(screen),
            dest = CreateCompatibleDC(screen);
        HBITMAP thumb = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!thumb) {
            DeleteDC(source);
            DeleteDC(dest);
            ReleaseDC(nullptr, screen);
            return;
        }
        auto oldS = SelectObject(source, bitmap), oldD = SelectObject(dest, thumb);
        SetStretchBltMode(dest, HALFTONE);
        StretchBlt(dest, 0, 0, w, h, source, 0, 0, b.bmWidth, b.bmHeight, SRCCOPY);
        ComPtr<ID2D1Bitmap> image;
        target->CreateBitmap(D2D1::SizeU(w, h), pixels, w * 4,
                             D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                      D2D1_ALPHA_MODE_IGNORE)),
                             &image);
        if (image) {
            float x = bounds.left + (bounds.right - bounds.left - w) / 2;
            target->DrawBitmap(image.Get(), {x, bounds.top, x + w, bounds.top + h});
        }
        SelectObject(source, oldS);
        SelectObject(dest, oldD);
        DeleteObject(thumb);
        DeleteDC(source);
        DeleteDC(dest);
        ReleaseDC(nullptr, screen);
    }
    void paint() {
        PAINTSTRUCT ps;
        BeginPaint(window, &ps);
        makeTarget();
        if (!target) {
            EndPaint(window, &ps);
            return;
        }
        RECT client;
        GetClientRect(window, &client);
        lastDrawResult = dcTarget->BindDC(ps.hdc, &client);
        if (FAILED(lastDrawResult)) {
            resetTarget();
            EndPaint(window, &ps);
            return;
        }
        float w = client.right / scale, h = client.bottom / scale;
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        target->Clear(color(dark() ? 0x15171d : 0xf9fafc));
        buttons.clear();
        unsigned fg = dark() ? 0xf0f2f6 : 0x202735, muted = dark() ? 0x959aab : 0x7d8695,
                 card = dark() ? 0x1c1f28 : 0xffffff;
        target->Clear(color(dark()?0x1c1f28:0xffffff));
        menuButton(L"文件",{4,0,59,30},[this]{switchMode(0);},mode==0);
        menuButton(L"剪贴板",{61,0,128,30},[this]{switchMode(1);},mode==1);
        menuButton(L"截图",{130,0,185,30},[this]{capture();},mode==2);
        menuButton(mode==1?L"剪辑":L"搜索",{193,0,248,30},[this]{if(mode==1)historyActionsMenu();else searchOptionsMenu();});
        menuButton(L"工具",{250,0,305,30},[this]{toolsMenu();});
        githubButton(w);
        if(mode!=2){
            rect({8,35,w-8,63},dark()?0x343842:0xc7cbd2);
            rect({9,36,w-9,62},card);
        }
        if (mode == 2) {
            ShowWindow(edit, SW_HIDE);
            text(L"截图与识别", {44, 44, w - 50, 74}, 15, fg, true);
            float right = w - 28;
            rect({28, 88, w * .57f, h - 70}, card, 12);
            rect({w * .57f + 14, 88, right, h - 70}, card, 12);
            if (originalImage)
                paintBitmap(showOriginal ? originalImage
                                         : (resultImage ? resultImage : originalImage),
                            {44, 107, w * .57f - 16, h - 180});
            else
                text(L"框选屏幕，让文字回到你手中。", {54, 132, w * .57f - 30, 195}, 21, fg, true);
            text(L"识别 / 翻译结果", {w * .57f + 32, 108, right - 16, 138}, 16, fg, true);
            if (detailText.empty()) text(L"截图后点击 OCR 或翻译，结果会显示在这里。",
                 {w * .57f + 32, 148, right - 18, h - 139}, 14, muted);
            ShowWindow(resultEdit, detailText.empty() ? SW_HIDE : SW_SHOW);
            if (originalImage) {
                button(L"复制图片",{44,h-165,143,h-131},[this]{copyBitmapToClipboard(resultImage?resultImage:originalImage);});
                button(L"贴图",{151,h-165,215,h-131},[this]{auto bitmap=(HBITMAP)CopyImage(resultImage?resultImage:originalImage,IMAGE_BITMAP,0,0,LR_CREATEDIBSECTION);pinImage(window,bitmap,[this](HBITMAP image,const std::wstring& action){acceptCapturedImage(image,action);});});
            }
            button(L"打开图片",{330,h-118,423,h-81},[this]{openImage();});
            button(L"开始截图", {44, h - 118, 145, h - 81}, [this] { capture(); }, true);
            if (originalImage) {
                button(L"OCR", {153, h - 118, 214, h - 81}, [this] { processImage(L"ocr"); });
                button(L"原图翻译", {222, h - 118, 322, h - 81},
                       [this] { processImage(L"translate"); });
            }
            if (!detailText.empty())
                button(L"复制文字", {w * .57f + 32, h - 118, w * .57f + 135, h - 81},
                       [this] { copyText(detailText); });
            if (resultImage)
                button(L"复制译图", {w * .57f + 143, h - 118, w * .57f + 246, h - 81},
                       [this] { copyBitmapToClipboard(resultImage); });
            if (resultImage)
                button(L"保存译图", {w * .57f + 254, h - 118, w * .57f + 357, h - 81}, [this] {
                    BITMAP dimensions{};GetObjectW(resultImage,sizeof(dimensions),&dimensions);
                    auto saveMemory=image_tools_detail::reserveMemory((uint64_t)dimensions.bmWidth*dimensions.bmHeight*sizeof(DWORD)*2);
                    if(!saveMemory){status=L"保存副本超出图像预算，请缩小图片或关闭其他贴图后重试";invalidate();return;}
                    auto copy =
                        (HBITMAP)CopyImage(resultImage, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
                    auto image =
                        std::shared_ptr<std::remove_pointer_t<HBITMAP>>(copy, [](HBITMAP h) {
                            if (h)
                                DeleteObject(h);
                        });
                    ComPtr<IFileSaveDialog> dialog;
                    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(&dialog)))) {
                        status = L"保存对话框无法打开";
                        return;
                    }
                    COMDLG_FILTERSPEC filter{L"PNG 图片", L"*.png"};
                    dialog->SetFileTypes(1, &filter);
                    dialog->SetDefaultExtension(L"png");
                    dialog->SetFileName(L"译图.png");
                    if (FAILED(dialog->Show(window)))
                        return;
                    ComPtr<IShellItem> item;
                    dialog->GetResult(&item);
                    PWSTR path = nullptr;
                    item->GetDisplayName(SIGDN_FILESYSPATH, &path);
                    std::filesystem::path destination = path;
                    CoTaskMemFree(path);
                    ocrTasks.add([this, image, destination, saveMemory] {
                        auto r = new Result;
                        r->type = 2;
                        r->status = saveBitmapPng(image.get(), destination)
                                        ? L"译图已保存"
                                        : L"译图保存失败，请检查路径与存储空间";
                        publish(r);
                    });
                    status = L"正在保存译图…";
                });
        } else if (mode == 0) {
            ShowWindow(resultEdit, SW_HIDE);
            paintFiles(w, h, card, fg, muted);
        } else {
            ShowWindow(resultEdit, SW_HIDE);
            float listRight = w - 8;
            int count = (int)historyRows.size();
            int visible = visibleRows();
            scroll = std::clamp(scroll, 0, std::max(0, count - visible));
            if (count == 0) {
                text(pendingQuery?L"搜索中…":controlText(edit).empty()?L"还没有剪贴板历史":L"没有匹配的历史",{16,86,listRight-12,113},13,muted);
            }
            for (int row = scroll; row < std::min(count, scroll + visible); row++) {
                float y = panel_layout::historyRowsTop + (row - scroll) * panel_layout::historyRowHeight;
                if (row == selected)
                    rect({8,y,listRight,y+panel_layout::historyRowHeight-1},dark()?0x303852:0xe6f0fb);
                auto& item=historyRows[row];
                text(item.kind==L"图片"?L"▧":item.kind==L"文件"?L"▤":L"≡",{12,y+4,29,y+25},13,0x6784a8);
                auto title=(item.pinned?L"★ ":L"")+item.title;
                text(title,{35,y+4,listRight-8,y+25},13,fg,false,false);
            }
        }
        auto hr = target->EndDraw();
        lastDrawResult = hr;
        if (!firstFrameMs)
            firstFrameMs = GetTickCount64() - startTime;
        if (hr == D2DERR_RECREATE_TARGET) {
            resetTarget();
            invalidate();
        }
        EndPaint(window, &ps);
    }
    void layout() {
        RECT r;
        GetClientRect(window, &r);
        scale = GetDpiForWindow(window) / 96.f;
        auto newFont=CreateFontW(-(int)std::lround(14*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(newFont){SendMessageW(edit,WM_SETFONT,(WPARAM)newFont,TRUE);if(resultEdit)SendMessageW(resultEdit,WM_SETFONT,(WPARAM)newFont,TRUE);if(uiFont)DeleteObject(uiFont);uiFont=newFont;}
        MoveWindow(edit,(int)(13*scale),(int)(39*scale),std::max(60,(int)r.right-(int)(26*scale)),(int)(21*scale),TRUE);
        if(resultEdit)MoveWindow(resultEdit,(int)(r.right*.57+32*scale),(int)(148*scale),
            std::max(80,(int)(r.right*.43-79*scale)),std::max(60,(int)(r.bottom-287*scale)),TRUE);
        updateTooltipRegions();
        invalidate();
    }
    std::wstring functionHint(int index)const{if(index==3)return L"GitHub 项目";const wchar_t* names[]{L"文件搜索",L"剪贴板历史",L"截图"};return std::wstring(names[index])+L"  "+hotkeyText(config.hotkeys[index]);}
    void updateTooltipRegions(){if(!menuTooltip)return;RECT client{};GetClientRect(window,&client);float width=client.right/scale;const RECT logical[]{{4,0,59,30},{61,0,128,30},{130,0,185,30},{(LONG)(width-38),0,(LONG)(width-8),30}};for(int i=0;i<4;++i){TOOLINFOW tool{sizeof(tool)};tool.hwnd=window;tool.uId=i+1;tool.rect={(LONG)(logical[i].left*scale),0,(LONG)(logical[i].right*scale),(LONG)(30*scale)};SendMessageW(menuTooltip,TTM_NEWTOOLRECTW,0,(LPARAM)&tool);}}
    void createMenuTooltip(){
        menuTooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,window,nullptr,instance,nullptr);
        if(!menuTooltip)return;SendMessageW(menuTooltip,TTM_SETMAXTIPWIDTH,0,300);SendMessageW(menuTooltip,TTM_SETDELAYTIME,TTDT_INITIAL,350);
        for(int i=0;i<4;++i){TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_SUBCLASS;tool.hwnd=window;tool.uId=i+1;tool.lpszText=LPSTR_TEXTCALLBACKW;SendMessageW(menuTooltip,TTM_ADDTOOLW,0,(LPARAM)&tool);}updateTooltipRegions();
    }
    float fileListRight(float width) const {
        return width - panel_layout::margin;
    }
    int visibleRows() const {
        RECT client{};
        GetClientRect(window, &client);
        return panel_layout::visible(mode,client.bottom/scale);
    }
    int rowAtPoint(float x,float y) const {
        if(mode==2)return -1;
        RECT client{};GetClientRect(window,&client);
        float top=panel_layout::rowTop(mode),height=panel_layout::rowHeight(mode);
        float right=client.right/scale-panel_layout::margin;
        int count=mode==0?(int)fileRows.size():(int)historyRows.size();
        int shown=std::min(visibleRows(),std::max(0,count-scroll));
        if(x<panel_layout::margin||x>=right||y<top||y>=top+shown*height||y>=client.bottom/scale-panel_layout::margin)return -1;
        return scroll+(int)((y-top)/height);
    }
    std::vector<std::wstring> selectedFilePaths() const {
        std::vector<std::wstring> paths;
        if (pendingQuery || closing) return paths;
        std::set<int64_t> included;
        for (int row : fileSelection)
            if (row >= 0 && row < (int)fileRows.size()) {
                paths.push_back(fileRows[row].path);included.insert(fileRows[row].id);
            }
        for (auto& [id, item] : fileSelectedItems)
            if (!included.contains(id)) paths.push_back(item.path);
        return paths;
    }
    void selectLoadedFiles() {
        if (pendingQuery) return;
        fileSelection.clear();fileSelectedItems.clear();
        for (int i = 0; i < (int)fileRows.size(); ++i) {
            fileSelection.insert(i);fileSelectedItems.emplace(fileRows[i].id, fileRows[i]);
        }
        invalidate();
    }
    void fileOperationResult(HRESULT result, const std::wstring& success) {
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || result == S_FALSE)
            status = L"操作已取消";
        else if (FAILED(result)) {
            wchar_t code[24]{};
            swprintf_s(code, L"0x%08X", (unsigned)result);
            status = L"文件操作未完成 · Windows " + std::wstring(code);
        } else status = success;
        invalidate();
    }
    void saveFilePreferences() {
        nlohmann::json preferences{{"sort", (int)fileQuery.sort}, {"descending", fileQuery.descending},
            {"matchCase", fileQuery.matchCase}, {"matchPath", fileQuery.matchPath},
            {"floatingPreview", filePreviewVisible},{"details",fileDetailsVisible}};
        clipboardTasks.add([this, preferences] {
            try {
                auto temporary = data / L"search-ui.json.pending";
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << preferences.dump(2);
                output.close();
                if (!output || !MoveFileExW(temporary.c_str(), (data / L"search-ui.json").c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("搜索偏好保存失败");
            } catch (const std::exception& e) { error(e); }
        });
    }
    void searchOptionsMenu() {
        auto menu = CreatePopupMenu();
        auto filters=CreatePopupMenu();const wchar_t* types[]{L"全部",L"文件",L"文件夹",L"图片",L"文档",L"音频",L"视频",L"压缩包"};
        for(int i=0;i<8;++i)AppendMenuW(filters,MF_STRING|(i==fileFilter?MF_CHECKED:0),101+i,types[i]);
        AppendMenuW(menu,MF_POPUP,(UINT_PTR)filters,L"文件类型");
        AppendMenuW(menu,MF_STRING|(fileQuery.matchPath?MF_CHECKED:0),201,L"匹配完整路径");
        AppendMenuW(menu,MF_STRING|(fileQuery.matchCase?MF_CHECKED:0),202,L"区分大小写");
        AppendMenuW(menu,MF_STRING|(fileRegex?MF_CHECKED:0),203,L"正则表达式");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        auto sorting=CreatePopupMenu();const wchar_t* sorts[]{L"名称",L"目录",L"大小",L"类型",L"修改时间"};
        const SearchSort sortKinds[]{SearchSort::Name,SearchSort::Path,SearchSort::Size,SearchSort::Type,SearchSort::Modified};
        for(int i=0;i<5;++i)AppendMenuW(sorting,MF_STRING|(fileQuery.sort==sortKinds[i]?MF_CHECKED:0),301+i,sorts[i]);
        AppendMenuW(menu,MF_POPUP,(UINT_PTR)sorting,L"排序");
        AppendMenuW(menu,MF_STRING|(fileDetailsVisible?MF_CHECKED:0),205,L"显示类型列");
        AppendMenuW(menu,MF_STRING|(filePreviewVisible?MF_CHECKED:0),204,L"浮动文件预览\tAlt+P");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,402,L"刷新\tF5");
        AppendMenuW(menu,MF_STRING,403,L"文件操作…");AppendMenuW(menu,MF_STRING,401,L"搜索语法与快捷键");
        POINT point{};GetCursorPos(&point);
        auto action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if(action>=101&&action<=108){fileFilter=action-101;query(true);}
        else if(action==201){fileQuery.matchPath=!fileQuery.matchPath;saveFilePreferences();query(true);}
        else if(action==202){fileQuery.matchCase=!fileQuery.matchCase;saveFilePreferences();query(true);}
        else if(action==203){fileRegex=!fileRegex;query(true);}
        else if(action==204)toggleFilePreview();
        else if(action==205){fileDetailsVisible=!fileDetailsVisible;saveFilePreferences();invalidate();}
        else if(action>=301&&action<=305)changeFileSort(sortKinds[action-301]);
        else if(action==402)query();else if(action==403)fileActionsMenu();
        else if (action == 401) MessageBoxW(window,
            L"空格：多个关键词\next:pdf;docx：扩展名\ntype:file / type:folder：文件或目录\n"
            L"in:目录：限定目录\npath:关键词：搜索完整路径\n* 与 ?：通配符\nregex:表达式：正则\n\n"
            L"Ctrl+F 聚焦搜索 · ↑ ↓ 选择 · Ctrl/Shift 多选\nEnter 打开 · Ctrl+Enter 定位 · Ctrl+C 复制文件\n"
            L"Ctrl+Shift+C 复制路径 · Ctrl+X 剪切 · F2 改名\nDelete 移至回收站 · Alt+Enter 属性 · Shift+F10 系统菜单\n"
            L"双击名称：定位文件 · 双击目录：复制完整文件路径\nAlt+P 浮动预览 · F5 刷新",
            L"DeskFlow 搜索与操作", MB_OK);
    }
    void toolsMenu(){
        auto menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"设置");AppendMenuW(menu,MF_STRING,2,L"索引与运行状态");AppendMenuW(menu,MF_STRING,3,L"打开数据目录");
        POINT p{};GetCursorPos(&p);auto action=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);DestroyMenu(menu);
        if(action==1)openSettings();else if(action==3)ShellExecuteW(window,L"open",data.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        else if(action==2){auto s=files?files->status():SearchStatus{};auto info=std::to_wstring(s.total)+L" 个索引条目\r\n"+s.message+L"\r\n\r\n"+(paused?L"剪贴板记录已暂停":L"剪贴板记录开启");if(!recordingWarning.empty())info+=L"\r\n"+recordingWarning;if(!hotkeyWarning.empty())info+=L"\r\n"+hotkeyWarning;if(!status.empty())info+=L"\r\n\r\n最近操作："+status;MessageBoxW(window,info.c_str(),L"DeskFlow 状态",MB_OK);}
    }
    void historyActionsMenu(){
        auto menu=CreatePopupMenu();bool valid=!pendingQuery&&selected>=0&&selected<(int)historyRows.size();UINT available=MF_STRING|(valid?0:MF_GRAYED);
        AppendMenuW(menu,available,1,L"粘贴\tEnter");AppendMenuW(menu,available,2,L"复制原格式\tCtrl+C");AppendMenuW(menu,available,3,L"复制纯文本");AppendMenuW(menu,available,4,L"收藏 / 取消收藏\tCtrl+D");AppendMenuW(menu,available,5,L"删除\tDelete");
        if(valid&&historyRows[selected].kind==L"图片"){AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,6,L"识别文字");AppendMenuW(menu,MF_STRING,7,L"翻译图片");AppendMenuW(menu,MF_STRING,8,L"贴图");}
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,available,9,L"预览 / 详细信息");AppendMenuW(menu,MF_STRING,10,L"筛选类型…");AppendMenuW(menu,MF_STRING|(favoritesOnly?MF_CHECKED:0),11,L"只看收藏");AppendMenuW(menu,MF_STRING|(paused?MF_CHECKED:0),12,L"暂停记录");
        POINT p{};GetCursorPos(&p);int action=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);DestroyMenu(menu);
        if(action==1)activate(true);else if(action==2)activate(false);else if(action==3)copyHistoryText();else if(action==4)pin();else if(action==5)erase();else if(action==6)processHistoryImage(L"ocr");else if(action==7)processHistoryImage(L"translate");else if(action==8)processHistoryImage(L"pin");else if(action==9){historyPreviewRequested=true;preview();}else if(action==10)historyFilterMenu();else if(action==11){favoritesOnly=!favoritesOnly;query(true);}else if(action==12){paused=!paused;invalidate();}
    }
    void resetFilePage() {
        query(true);
    }
    void fileFilterMenu() {
        const wchar_t* labels[]{L"全部",L"文件",L"文件夹",L"图片",L"文档",L"音频",L"视频",L"压缩包"};
        auto menu = CreatePopupMenu();
        for (int i = 0; i < 8; ++i) AppendMenuW(menu, MF_STRING | (fileFilter == i ? MF_CHECKED : 0), i + 1, labels[i]);
        POINT p{};GetCursorPos(&p);
        int chosen = TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);DestroyMenu(menu);
        if (chosen) { fileFilter = chosen - 1; resetFilePage(); }
    }
    void changeFileSort(SearchSort sort) {
        if (fileQuery.sort == sort) fileQuery.descending = !fileQuery.descending;
        else { fileQuery.sort = sort;fileQuery.descending = false; }
        saveFilePreferences();resetFilePage();
    }
    void toggleFilePreview() {
        filePreviewVisible=!filePreviewVisible;
        if(!filePreviewVisible){++previewGeneration;floatingPreview.hide();if(previewImage)DeleteObject(previewImage);previewImage=nullptr;detailText.clear();}
        else preview();
        saveFilePreferences();invalidate();
    }
    void copyFileText(int kind) {
        std::wstring text;
        for (auto& value : selectedFilePaths()) {
            if (!text.empty()) text += L"\r\n";
            auto path = std::filesystem::path(value);
            text += kind == 1 ? path.filename().wstring() : kind == 2 ? path.parent_path().wstring() : value;
        }
        if (!text.empty()) copyText(text);
    }
    void fileDoubleClick(int row,float x){
        if(pendingQuery||row<0||row>=(int)fileRows.size())return;
        RECT client{};GetClientRect(window,&client);auto action=panel_layout::doubleClick(x,client.right/scale,fileDetailsVisible);
        if(action==panel_layout::DoubleClick::Locate)fileOperationResult(openFileLocation(window,fileRows[row].path),L"已定位文件");
        else if(action==panel_layout::DoubleClick::CopyPath)copyText(fileRows[row].path);
    }
    void renameSelectedFile() {
        renamePaths(selectedFilePaths());
    }
    void renamePaths(const std::vector<std::wstring>& paths) {
        if(paths.size()!=1){status=L"重命名请先选择一个文件";invalidate();return;}
        auto name=askFileName(window,std::filesystem::path(paths.front()).filename().wstring());
        if(name&&!closing){fileOperationResult(renameFile(window,paths.front(),*name),L"已重命名 · 索引会自动更新");query();}
    }
    void historyFilterMenu() {
        auto menu=CreatePopupMenu();const wchar_t* types[]{L"全部",L"文本",L"图片",L"文件"};
        for(int i=0;i<4;i++)AppendMenuW(menu,MF_STRING|(historyFilter==i?MF_CHECKED:0),i+1,types[i]);
        POINT p{};GetCursorPos(&p);int chosen=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);DestroyMenu(menu);
        if(chosen){historyFilter=chosen-1;query(true);}
    }
    void openImage() {
        ComPtr<IFileOpenDialog> dialog;
        if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
        COMDLG_FILTERSPEC filters[]{{L"图片",L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp"},{L"所有文件",L"*.*"}};
        dialog->SetFileTypes(2,filters);
        if(FAILED(dialog->Show(window)))return;
        ComPtr<IShellItem> item;dialog->GetResult(&item);PWSTR name=nullptr;
        if(FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&name)))return;
        std::filesystem::path path=name;CoTaskMemFree(name);int version=++captureGeneration;
        ocrTasks.add([this,path,version]{auto result=std::make_unique<Result>();result->type=22;result->generation=version;
            result->bitmap=loadImageBitmap(path);if(!result->bitmap){result->type=2;result->status=L"图片无法读取，或超过 32MP 处理预算";}
            if(version==captureGeneration)publish(result.release());},true);
    }
    void copyHistoryText() {
        if(pendingQuery||selected<0||selected>=(int)historyRows.size())return;
        auto id=historyRows[selected].id;
        clipboardTasks.add([this,id]{try{
            auto payload=history->load(id);auto result=std::make_unique<Result>();result->type=5;
            for(auto& format:payload.formats)if(format.format==CF_UNICODETEXT)result->payload.formats.push_back(std::move(format));
            if(result->payload.formats.empty()){result->type=2;result->status=L"该条目没有纯文本格式，可使用原格式复制";}
            publish(result.release());
        }catch(const std::exception& e){error(e);}});
    }
    void processHistoryImage(const std::wstring& action) {
        if(pendingQuery||selected<0||selected>=(int)historyRows.size())return;
        auto id=historyRows[selected].id;int version=++previewGeneration;
        previewTasks.add([this,id,version,action]{try{
            auto payload=history->load(id);auto result=std::make_unique<Result>();result->type=21;result->generation=version;
            result->bitmap=historyBitmap(payload);result->text=action;
            if(!result->bitmap){result->type=2;result->status=L"历史图片格式无法转换，可使用原格式复制";}
            if(version==previewGeneration)publish(result.release());
        }catch(const std::exception& e){error(e);}},true);
    }
    void pasteImage() {
        int version=++captureGeneration;
        size_t maximumBytes=(size_t)config.maximumEntryMiB*1024*1024;
        clipboardReaderTasks.add([this,version,maximumBytes]{try {
            auto payload=readClipboardPayload(window,maximumBytes);
            auto result=std::make_unique<Result>();result->type=22;result->generation=version;result->bitmap=historyBitmap(payload);
            if(!result->bitmap){result->type=2;result->status=L"剪贴板中没有可处理的图片";}
            if(version==captureGeneration)publish(result.release());
        }catch(const std::exception& e){error(e);}},true);
    }
    void deleteSelectedFiles() {
        auto paths=selectedFilePaths();if(paths.empty())return;
        fileOperationResult(recycleFiles(window,paths),L"已移至回收站 · 索引会自动更新");query();
    }
    bool handleFileKey(HWND focused, WPARAM key) {
        if(mode!=0)return false;
        bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0,alt=(GetKeyState(VK_MENU)&0x8000)!=0;
        bool editing=focused==edit;
        DWORD from=0,to=0;if(editing)SendMessageW(edit,EM_GETSEL,(WPARAM)&from,(LPARAM)&to);
        if(control&&key=='F'){SetFocus(edit);SendMessageW(edit,EM_SETSEL,0,-1);return true;}
        if(key==VK_TAB){SetFocus(editing?window:edit);return true;}
        if(key==VK_UP||key==VK_DOWN){select(selected+(key==VK_UP?-1:1),shift,control);return true;}
        if(key==VK_PRIOR||key==VK_NEXT){select(selected+(key==VK_PRIOR?-visibleRows():visibleRows()),shift,control);return true;}
        if(!editing&&(key==VK_HOME||key==VK_END)){select(key==VK_HOME?0:(int)fileRows.size()-1,shift,control);return true;}
        if(control&&key=='A'&&!editing){selectLoadedFiles();return true;}
        if(control&&(key=='C'||key=='X')&&((shift&&key=='C')||!editing||from==to)){
            if(shift&&key=='C')copyFileText(0);
            else{auto paths=selectedFilePaths();if(!paths.empty())fileOperationResult(copyFiles(paths,key=='X'),key=='X'?L"文件已剪切":L"文件已复制");}
            return true;
        }
        if(key==VK_RETURN&&alt){auto paths=selectedFilePaths();if(!paths.empty())fileOperationResult(showFileProperties(window,paths),L"已打开属性");return true;}
        if(key==VK_RETURN){activate(true);return true;}
        if(key==VK_F2){renameSelectedFile();return true;}
        if(key==VK_DELETE&&(!editing||controlText(edit).empty())){deleteSelectedFiles();return true;}
        if(key==VK_F5){query();return true;}
        if(key==VK_APPS||(key==VK_F10&&shift)){
            POINT point{(LONG)(65*scale),(LONG)((panel_layout::fileRowsTop+13+(selected-scroll)*panel_layout::fileRowHeight)*scale)};ClientToScreen(window,&point);fileActionsMenu(true,point);return true;
        }
        if(alt&&key=='P'){toggleFilePreview();return true;}
        return false;
    }
    void systemFileMenu(const std::vector<std::wstring>& paths,POINT p) {
        bool rename=false;auto result=showFileContextMenu(window,paths,p,&rename);
        if(closing)return;
        if(rename)renamePaths(paths);
        else fileOperationResult(result,L"系统文件操作已执行");
        query();
    }
    void fileActionsMenu(bool system = false, std::optional<POINT> position = {}) {
        if (pendingQuery) return;
        auto paths = selectedFilePaths();if (paths.empty()) return;
        POINT p{};if (position) p = *position;else GetCursorPos(&p);
        if (system) {systemFileMenu(paths,p);return;}
        auto menu = CreatePopupMenu();
        const wchar_t* labels[]{L"打开\tEnter",L"所在文件夹\tCtrl+Enter",L"复制文件\tCtrl+C",L"剪切文件\tCtrl+X",
            L"复制完整路径\tCtrl+Shift+C",L"复制文件名",L"复制父目录",L"系统右键菜单\tShift+F10",L"属性\tAlt+Enter",L"重命名\tF2",L"移至回收站\tDelete",L"以管理员身份运行"};
        for (int i=0;i<12;i++)AppendMenuW(menu,MF_STRING,i+1,labels[i]);
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,13,L"导出已加载结果（CSV / TXT）");
        int action=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);DestroyMenu(menu);
        if(closing)return;
        if(action==1){hide();for(auto& path:paths)ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}
        else if(action==2)fileOperationResult(openFileLocation(window,paths.front()),L"已定位文件");
        else if(action==3||action==4)fileOperationResult(copyFiles(paths,action==4),action==4?L"文件已剪切，可在资源管理器中粘贴":L"文件已复制，可在资源管理器中粘贴");
        else if(action>=5&&action<=7){std::wstring text;for(auto& path:paths){if(!text.empty())text+=L"\r\n";auto file=std::filesystem::path(path);text+=action==6?file.filename().wstring():action==7?file.parent_path().wstring():path;}copyText(text);}
        else if(action==8)systemFileMenu(paths,p);
        else if(action==9)fileOperationResult(showFileProperties(window,paths),L"已打开属性");
        else if(action==10)renamePaths(paths);
        else if(action==11){fileOperationResult(recycleFiles(window,paths),L"已移至回收站 · 索引会自动更新");query();}
        else if(action==12){auto result=(INT_PTR)ShellExecuteW(window,L"runas",paths.front().c_str(),nullptr,nullptr,SW_SHOWNORMAL);status=result>32?L"管理员运行请求已提交":L"管理员运行已取消或此文件不支持";invalidate();}
        else if(action==13)exportFilePage();
    }
    void exportFilePage() {
        if(pendingQuery||fileRows.empty())return;
        ComPtr<IFileSaveDialog> dialog;
        if(FAILED(CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
        COMDLG_FILTERSPEC formats[]{{L"CSV 表格",L"*.csv"},{L"TXT 路径列表",L"*.txt"}};
        dialog->SetFileTypes(2,formats);dialog->SetDefaultExtension(L"csv");dialog->SetFileName(L"搜索结果.csv");
        if(FAILED(dialog->Show(window)))return;
        ComPtr<IShellItem> item;dialog->GetResult(&item);PWSTR filename=nullptr;
        if(FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&filename)))return;
        std::filesystem::path destination=filename;CoTaskMemFree(filename);UINT type=1;dialog->GetFileTypeIndex(&type);
        auto rows=fileRows;
        clipboardTasks.add([this,rows,destination,type]{try{
            auto temporary=destination;temporary+=L".deskflow-"+std::to_wstring(GetCurrentProcessId())+L".pending";
            std::ofstream output(temporary,std::ios::binary|std::ios::trunc);output<<"\xEF\xBB\xBF";
            auto quote=[](const std::wstring& value){std::string out="\"";for(char ch:utf8(value)){out+=ch;if(ch=='\"')out+=ch;}return out+"\"";};
            if(type==1)output<<"Name,Path,Type,SizeBytes,ModifiedLocal\r\n";
            for(auto& row:rows){if(type==2)output<<utf8(row.path)<<"\r\n";
                else output<<quote(row.name)<<","<<quote(row.path)<<","<<quote(row.folder?L"文件夹":std::filesystem::path(row.name).extension().wstring())<<","<<(!row.folder&&row.sizeKnown?std::to_string(row.size):"")<<","<<quote(row.modified?fileModifiedLabel(row.modified):L"")<<"\r\n";}
            output.close();
            if(!output||!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
                DeleteFileW(temporary.c_str());throw std::runtime_error("已加载结果导出失败，请检查目标目录与磁盘空间");}
            auto result=new Result;result->type=2;result->status=L"已导出已加载的 "+std::to_wstring(rows.size())+L" 项结果";publish(result);
        }catch(const std::exception& e){error(e);}});
        status=L"正在导出已加载结果…";invalidate();
    }
    void paintFiles(float w,float h,unsigned card,unsigned fg,unsigned muted) {
        float right=w-8;
        float nameEnd=panel_layout::nameEnd(w,fileDetailsVisible),pathEnd=panel_layout::pathEnd(w,fileDetailsVisible);
        float typeEnd=panel_layout::typeEnd(w),sizeEnd=panel_layout::sizeEnd(w);
        auto label=[this](const wchar_t* name,SearchSort sort){return std::wstring(name)+(fileQuery.sort==sort?(fileQuery.descending?L" ↓":L" ↑"):L"");};
        menuButton(label(L"名称",SearchSort::Name),{8,70,nameEnd,95},[this]{changeFileSort(SearchSort::Name);});
        menuButton(label(L"目录",SearchSort::Path),{nameEnd,70,pathEnd,95},[this]{changeFileSort(SearchSort::Path);});
        if(fileDetailsVisible)menuButton(label(L"类型",SearchSort::Type),{pathEnd,70,typeEnd,95},[this]{changeFileSort(SearchSort::Type);});
        menuButton(label(L"大小",SearchSort::Size),{typeEnd,70,sizeEnd,95},[this]{changeFileSort(SearchSort::Size);});
        menuButton(label(L"修改时间",SearchSort::Modified),{sizeEnd,70,right,95},[this]{changeFileSort(SearchSort::Modified);});
        rect({8,95,right,96},dark()?0x343842:0xe4e7ec);
        scroll=std::clamp(scroll,0,std::max(0,(int)fileRows.size()-visibleRows()));
        for(int i=scroll;i<std::min((int)fileRows.size(),scroll+visibleRows());i++) {
            float y=panel_layout::fileRowsTop+(i-scroll)*panel_layout::fileRowHeight;
            if(fileSelection.contains(i)||i==selected)rect({8,y,right,y+25},dark()?0x283751:0xe6f0fb);
            auto& item=fileRows[i];
            text(item.folder?L"▣":L"▤",{12,y+4,29,y+25},13,item.folder?0xc79a49:0x4d91cd);
            text(item.name,{35,y+4,nameEnd-8,y+25},13,fg,false,false);
            text(std::filesystem::path(item.path).parent_path().wstring(),{nameEnd+10,y+4,pathEnd-8,y+25},13,fg,false,false);
            auto type=item.folder?L"文件夹":std::filesystem::path(item.name).extension().wstring();
            if(type.empty())type=L"文件";
            if(fileDetailsVisible)text(type,{pathEnd+8,y+4,typeEnd-8,y+25},12,muted,false,false);
            text(item.folder?L"":!item.sizeKnown?L"—":fileSizeLabel(item.size),{typeEnd+6,y+4,sizeEnd-10,y+25},12,fg,false,false,true);
            text(fileModifiedLabel(item.modified),{sizeEnd+8,y+4,right-8,y+25},12,fg,false,false);
        }
        if(fileRows.empty()) {
            text(pendingQuery||listInFlight?L"搜索中…":L"没有匹配文件",{16,112,right-12,142},13,muted);
        }
    }
    void show(int requested) {
        HWND foreground = GetForegroundWindow();
        if (externalWindow(foreground))
            previousWindow = foreground;
        if (requested != mode) {
            ++captureGeneration;
            cancelTranslation = true;
            *imageCancellation = true;
            if (requested != 2) {
                for (auto image : {originalImage, resultImage})
                    if (image)
                        DeleteObject(image);
                originalImage = resultImage = nullptr;
                originalMemory.reset();resultMemory.reset();
            }
        }
        mode = requested;
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)(mode == 1 ? L"搜索历史文字或文件名" : L"搜索文件名 · 空格多关键词 · ext:pdf"));
        if (requested != 2) {
            for (auto image : {originalImage, resultImage})
                if (image)
                    DeleteObject(image);
            originalImage = resultImage = nullptr;
            originalMemory.reset();resultMemory.reset();
        }
        bool interactive = currentInputDesktop();
        ShowWindow(window, interactive ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_SHOWWINDOW|(interactive?0:SWP_NOACTIVATE));
        if (interactive) SetForegroundWindow(window);
        ShowWindow(edit, mode == 2 ? SW_HIDE : SW_SHOW);
        if (interactive) SetFocus(edit);
        layout();
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SetTimer(window, 2, 1800, nullptr);
        query(true);
        invalidate();
    }
    void hide() {
        floatingPreview.hide();historyPreviewRequested=false;
        cancelTranslation = true;
        *imageCancellation = true;
        ++captureGeneration;
        ++previewGeneration;
        ++generation;
        pendingQuery = true;
        queryTasks.add([]{},true);
        ShowWindow(window, SW_HIDE);
        KillTimer(window, 2);
        if (previewImage) {
            DeleteObject(previewImage);
            previewImage = nullptr;
        }
        for (auto image : {originalImage, resultImage})
            if (image)
                DeleteObject(image);
        originalImage = resultImage = nullptr;
        originalMemory.reset();resultMemory.reset();
        detailText.clear();
        if (currentInputDesktop() && previousWindow && IsWindow(previousWindow))
            SetForegroundWindow(previousWindow);
    }
    void switchMode(int requested) {
        floatingPreview.hide();historyPreviewRequested=false;
        ++captureGeneration;
        cancelTranslation = true;
        *imageCancellation = true;
        if (requested != 2) {
            for(auto image:{originalImage,resultImage})if(image)DeleteObject(image);
            originalImage=resultImage=nullptr;originalMemory.reset();resultMemory.reset();
        }
        mode = requested;
        selected = scroll = 0;
        detailText.clear();
        ShowWindow(edit, mode == 2 ? SW_HIDE : SW_SHOW);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)(mode == 1 ? L"搜索历史文字或文件名" : L"搜索文件名 · 空格多关键词 · ext:pdf"));
        layout();
        query(true);
        SetFocus(edit);
        invalidate();
    }
    void requestList(int action, int64_t page, size_t pageCount = 1) {
        if (closing || mode == 2 || listInFlight) return;
        auto cursor = ListCursor{};
        try {
            if (action == 2) {
                if (mode == 0 && !fileRows.empty()) cursor.file = fileRows.back();
                if (mode == 1 && !historyRows.empty()) cursor.history = historyRows.back().id;
            } else cursor = listCheckpoints.get(page);
        } catch (const std::exception& e) { status = userError(e);invalidate();return; }
        listInFlight = true;
        int version = generation, serial = ++listRequest, current = mode;
        auto request = activeFileQuery;
        auto input = activeHistoryInput, kind = activeHistoryKind;
        bool favorites = activeFavorites;
        bool accepted = queryTasks.add([this, version, serial, current, action, page, pageCount,
                                       cursor, request, input, kind, favorites]() mutable {
            auto r = std::make_unique<Result>();
            r->type = action == 5 ? 26 : 0;r->generation = version;
            r->listAction = action;r->listRequest = serial;
            try {
                for (size_t batch = 0; action != 5 && batch < pageCount; ++batch) {
                    if (version != generation || closing) return;
                    ListPage mark;mark.number = page + (int64_t)batch;mark.cursor = cursor;
                    if (current == 0 && files) {
                        request.after = cursor.file;
                        std::vector<SearchItem> rows;
                        do {
                            rows = files->query(request, listPageSize);
                            if (version != generation || closing) return;
                            if(files->queryPending()&&action==1&&!rows.empty()){
                                auto partial=std::make_unique<Result>();partial->type=27;partial->generation=version;partial->listRequest=serial;partial->files=rows;
                                publish(partial.release());
                            }
                            if (files->queryPending())
                                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                        } while (files->queryPending());
                        mark.count = rows.size();
                        if (!rows.empty()) cursor.file = rows.back();
                        r->files.insert(r->files.end(), std::make_move_iterator(rows.begin()),
                                        std::make_move_iterator(rows.end()));
                    } else if (current == 1 && history) {
                        auto rows = history->list(input, cursor.history, listPageSize, favorites, kind);
                        mark.count = rows.size();
                        if (!rows.empty()) cursor.history = rows.back().id;
                        r->history.insert(r->history.end(), std::make_move_iterator(rows.begin()),
                                          std::make_move_iterator(rows.end()));
                    }
                    r->listEnd = mark.count < listPageSize;
                    r->listPages.push_back(std::move(mark));
                    if (r->listEnd) break;
                }
                if (current == 0) {
                    if (files) r->index = files->status();
                    else r->index.message = searchFailure;
                    r->listTotal = r->index.total;
                    r->status = std::to_wstring(r->index.total) + L" 个索引条目 · " + r->index.message;
                } else {
                    r->listTotal = history ? history->count() : 0;
                    r->status = history ? std::to_wstring(r->listTotal) + L" 条历史 · 本地保存" : historyFailure;
                }
            } catch (const std::exception& e) { r->type = 9;r->status = userError(e); }
            if (version == generation && !closing) publish(r.release());
        }, true);
        if (!accepted) {
            listInFlight = false;pendingQuery = false;
            status = L"查询队列繁忙，请稍后重试";
        }
        invalidate();
    }
    void query(bool reset = false, bool automatic = false) {
        if (mode == 2) { ++generation;listInFlight = false;return; }
        auto input = controlText(edit);
        auto request = fileQuery;request.after.reset();
        request.text = fileRegex && !input.empty() ? L"regex:\"" + input + L"\"" : input;
        const wchar_t* filterTerms[]{L"",L"type:file",L"type:folder",L"ext:png;jpg;jpeg;gif;bmp;webp;tif;tiff;ico",L"ext:pdf;doc;docx;xls;xlsx;ppt;pptx;txt;md;csv;rtf",L"ext:mp3;wav;flac;aac;ogg;m4a;wma",L"ext:mp4;mkv;avi;mov;webm;wmv;mpeg",L"ext:zip;7z;rar;tar;gz;bz2;xz"};
        if (fileFilter) request.text = std::wstring(filterTerms[fileFilter]) + L" " + request.text;
        const wchar_t* kinds[]{L"",L"文本",L"图片",L"文件"};
        std::wstring kind = kinds[historyFilter];
        auto identity = nlohmann::json{{"mode", mode}, {"text", utf8(mode == 0 ? request.text : input)},
            {"sort", (int)request.sort}, {"descending", request.descending},
            {"case", request.matchCase}, {"path", request.matchPath},
            {"favorites", favoritesOnly}, {"kind", utf8(kind)}}.dump();
        if (automatic && listInFlight && identity == listIdentity) return;
        reset = reset || identity != listIdentity || pendingQuery || listPages.empty();
        ++generation;listInFlight = false;
        activeFileQuery = request;activeHistoryInput = input;
        activeHistoryKind = kind;activeFavorites = favoritesOnly;listIdentity = identity;
        if (reset) {
            floatingPreview.hide();historyPreviewRequested=false;
            ++previewGeneration;
            listCheckpoints.clear();listPages.clear();listEnd = listHeadDirty = false;
            historyRows.clear();fileRows.clear();fileSelection.clear();fileSelectedItems.clear();
            selected = scroll = fileAnchor = wheelRemainder = 0;listFocusedId = fileAnchorId = 0;pendingQuery = true;
            detailText.clear();if (previewImage) DeleteObject(previewImage);previewImage = nullptr;
            requestList(1, 0);
        } else if (automatic && (listPages.front().number != 0 || scroll > visibleRows())) {
            requestList(5, 0);
        } else {
            ++previewGeneration;
            requestList(4, listPages.front().number, listPages.size());
        }
    }
    void continueList(bool backwards = false) {
        if (pendingQuery || listInFlight || closing || mode == 2 || listPages.empty()) return;
        int count = mode == 0 ? (int)fileRows.size() : (int)historyRows.size();
        int threshold = std::max(visibleRows() * 2, 12);
        if (backwards && scroll <= threshold && listPages.front().number > 0)
            requestList(3, listPages.front().number - 1);
        else if (!backwards && !listEnd && scroll + visibleRows() + threshold >= count)
            requestList(2, listPages.back().number + 1);
        else if (listHeadDirty && listPages.front().number == 0 && scroll <= visibleRows())
            query(false, true);
    }
    void scrollList(int rows) {
        if (mode == 2 || pendingQuery) return;
        int count = mode == 0 ? (int)fileRows.size() : (int)historyRows.size();
        scroll = std::clamp(scroll + rows, 0, std::max(0, count - visibleRows()));
        continueList(rows < 0);invalidate();
    }
    void wheelList(int delta) {
        if (mode == 2 || pendingQuery) return;
        wheelRemainder += delta;
        int steps = wheelRemainder / WHEEL_DELTA;
        wheelRemainder %= WHEEL_DELTA;
        if (!steps) return;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        int amount = lines == WHEEL_PAGESCROLL ? visibleRows() : (int)std::min<UINT>(lines, listRowLimit);
        scrollList(-steps * amount);
    }
    RECT previewAnchor() const{
        RECT r{};GetClientRect(window,&r);float y=panel_layout::rowTop(mode)+(selected-scroll)*panel_layout::rowHeight(mode);
        y=std::clamp(y,panel_layout::rowTop(mode),std::max(panel_layout::rowTop(mode),r.bottom/scale-32));
        RECT anchor{(LONG)(8*scale),(LONG)(y*scale),r.right-(LONG)(8*scale),(LONG)((y+panel_layout::rowHeight(mode))*scale)};
        MapWindowPoints(window,nullptr,(POINT*)&anchor,2);return anchor;
    }
    void showHistoryPreview(){
        if(pendingQuery||selected<0||selected>=(int)historyRows.size())return;
        historyPreviewRequested=true;auto& item=historyRows[selected];
        floatingPreview.show(window,item,item.kind==L"文本"?item.preview:clipboardDetails(item),nullptr,previewAnchor(),item.kind==L"文本",config.dark);
        preview();
    }
    void preview() {
        if (!IsWindowVisible(window)) return;
        if (mode == 0) {
            if (!filePreviewVisible || selected < 0 || selected >= (int)fileRows.size()) return;
            auto path = std::filesystem::path(fileRows[selected].path);
            int version = ++previewGeneration;
            previewTasks.add([this, path, version] {
                auto preview = loadFilePreview(path, &previewGeneration, version);
                auto result = std::make_unique<Result>();result->type = 20;result->generation = version;
                result->text = std::move(preview.text);result->bitmap = preview.bitmap;
                if (version == previewGeneration) publish(result.release());
            }, true);
            return;
        }
        if (mode != 1 || !historyPreviewRequested || selected < 0 || selected >= (int)historyRows.size())
            return;
        auto item=historyRows[selected];auto id=item.id;
        int version = ++previewGeneration;
        previewTasks.add(
            [this, id, version, item] {
                auto r = std::make_unique<Result>();
                r->type = 3;
                r->generation = version;
                try {
                    auto payload = history->load(id);
                    r->bitmap = historyBitmap(payload,700);
                    auto content=clipboardPreviewText(payload);r->text=std::move(content.text);
                    r->listAction=content.supported||r->bitmap?1:0;
                    if(!r->listAction)r->text=clipboardDetails(item,&payload);
                    if (version == previewGeneration)
                        publish(r.release());
                } catch (const std::exception &e) {
                    error(e);
                }
            },
            true);
    }
    void select(int row, bool extend = false, bool toggle = false) {
        if (pendingQuery) return;
        bool backwards = row < selected;
        int count = mode == 0 ? (int)fileRows.size() : (int)historyRows.size();
        if (count == 0)
            return;
        selected = std::clamp(row, 0, count - 1);
        listFocusedId = mode == 0 ? fileRows[selected].id : historyRows[selected].id;
        if (mode == 0) {
            auto add = [&](int i) {
                if (fileSelectedItems.size() < listRowLimit || fileSelectedItems.contains(fileRows[i].id)) {
                    fileSelection.insert(i);fileSelectedItems[fileRows[i].id] = fileRows[i];
                } else status = L"最多同时选择 1000 项文件";
            };
            if (extend) {
                if (!toggle) { fileSelection.clear();fileSelectedItems.clear(); }
                for (int i = std::min(fileAnchor, selected); i <= std::max(fileAnchor, selected); ++i) add(i);
            } else if (toggle) {
                if (fileSelectedItems.contains(fileRows[selected].id) || fileSelection.contains(selected)) {
                    fileSelection.erase(selected);fileSelectedItems.erase(fileRows[selected].id);
                } else add(selected);
                fileAnchor = selected;
                fileAnchorId = fileRows[selected].id;
            } else { fileSelection.clear();fileSelectedItems.clear();add(selected);fileAnchor = selected;fileAnchorId = fileRows[selected].id; }
        }
        if (selected < scroll)
            scroll = selected;
        RECT r;
        GetClientRect(window, &r);
        int visible = visibleRows();
        if (selected >= scroll + visible)
            scroll = selected - visible + 1;
        preview();
        continueList(backwards);
        NotifyWinEvent(EVENT_OBJECT_FOCUS, window, OBJID_CLIENT, selected + 1);
        invalidate();
    }
    void copyText(const std::wstring &s) {
        ClipFormat f;
        f.format = CF_UNICODETEXT;
        f.data.resize((s.size() + 1) * 2);
        memcpy(f.data.data(), s.c_str(), f.data.size());
        ClipPayload p;
        p.formats.push_back(std::move(f));
        restoreClipboardPayload(window, p);
        status = L"文字已复制";
        invalidate();
    }
    void activate(bool paste) {
        if (pendingQuery) {
            status = L"正在更新搜索结果，请稍候";
            invalidate();
            return;
        }
        if (mode == 0) {
            auto paths = selectedFilePaths();if(paths.empty())return;
            if (!paste) { fileOperationResult(copyFiles(paths), L"文件已复制 · 在资源管理器中粘贴");return; }
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                fileOperationResult(openFileLocation(window, paths.front()), L"已定位文件");return;
            }
            if (paths.size()>10 && MessageBoxW(window,L"将打开超过 10 个项目，是否继续？",L"打开多个文件",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
            hide();
            for (auto& path : paths) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (mode != 1 || selected < 0 || selected >= (int)historyRows.size())
            return;
        auto id = historyRows[selected].id;
        auto targetWindow = previousWindow;
        DWORD targetPid = 0;
        if (targetWindow)
            GetWindowThreadProcessId(targetWindow, &targetPid);
        clipboardTasks.add([this, id, paste, targetWindow, targetPid] {
            try {
                auto payload = history->load(id);
                auto r = new Result;
                r->payload = std::move(payload);
                r->type = paste ? 4 : 5; // Marshal clipboard writes to host message thread.
                r->pasteTarget = targetWindow;
                r->pastePid = targetPid;
                r->text = std::to_wstring(id);
                publish(r);
            } catch (const std::exception &e) {
                error(e);
            }
        });
    }
    void pin() {
        if (pendingQuery || mode != 1 || selected < 0 || selected >= (int)historyRows.size())
            return;
        auto item = historyRows[selected];
        clipboardTasks.add([this, item] {
            try {
                history->setPinned(item.id, !item.pinned);
                auto r = new Result;
                r->type = 6;
                publish(r);
            } catch (const std::exception &e) {
                error(e);
            }
        });
    }
    void erase() {
        if (pendingQuery || mode != 1 || selected < 0 || selected >= (int)historyRows.size())
            return;
        auto id = historyRows[selected].id;
        clipboardTasks.add([this, id] {
            try {
                history->erase(id);
                auto r = new Result;
                r->type = 6;
                publish(r);
            } catch (const std::exception &e) {
                error(e);
            }
        });
    }
    void capture() {
        if (captureActive() || recordingActive() || scrollingCaptureActive())
            return;
        ++captureGeneration;
        *imageCancellation = true;
        if (!IsWindowVisible(window))
            previousWindow = GetForegroundWindow();
        hide();
        DwmFlush();
        // Snapshots keep the translation worker independent of Host lifetime.
        auto translation = config.translation;
        auto worker = executableDirectory() / L"DeskOCR.exe";
        auto temporaryDirectory = data;
        beginCapture(window, [this](HBITMAP bitmap, const std::wstring &action) {
            acceptCapturedImage(bitmap, action);
        }, [this](RECT region, const std::wstring& action) {
            if (!closing) beginRecording(window, region, action == L"gif" ? RecordingFormat::Gif : RecordingFormat::Mp4);
        }, [translation, worker, temporaryDirectory](HBITMAP bitmap, std::atomic_bool& cancellation) {
            return translateImage(bitmap, worker, translation, cancellation, temporaryDirectory);
        });
    }
    void acceptCapturedImage(HBITMAP bitmap, const std::wstring& action) {
            if (closing) { if (bitmap) DeleteObject(bitmap);return; }
            if (originalImage)
                DeleteObject(originalImage);
            originalImage = nullptr;originalMemory.reset();
            if (resultImage) {
                DeleteObject(resultImage);
                resultImage = nullptr;
            }
            resultMemory.reset();
            BITMAP size{};
            if(!bitmap||!GetObjectW(bitmap,sizeof(size),&size)) { if(bitmap)DeleteObject(bitmap);return; }
            auto memory=image_tools_detail::reserveMemory((uint64_t)size.bmWidth*size.bmHeight*sizeof(DWORD));
            if(!memory){DeleteObject(bitmap);status=L"图像处理预算不足，请关闭其他贴图后重试";MessageBoxW(window,status.c_str(),L"DeskFlow",MB_OK|MB_ICONINFORMATION);return;}
            originalMemory=std::move(memory);originalImage=bitmap;
            mode = 2;
            detailText.clear();
            if(resultEdit)SetWindowTextW(resultEdit,L"");
            auto previous = previousWindow;
            show(2);
            previousWindow = previous;
            if (action == L"ocr" || action == L"translate") processImage(action);
            else { status = L"捕获完成 · 可识别、翻译或复制";invalidate(); }
    }
    void processImage(const std::wstring &action) {
        if (!originalImage)
            return;
        *imageCancellation = true;
        imageCancellation = std::make_shared<std::atomic_bool>(false);
        auto cancellation = imageCancellation;
        int version = ++captureGeneration;
        if(resultImage)DeleteObject(resultImage);resultImage=nullptr;resultMemory.reset();
        BITMAP dimensions{};GetObjectW(originalImage,sizeof(dimensions),&dimensions);
        auto memory=image_tools_detail::reserveMemory((uint64_t)dimensions.bmWidth*dimensions.bmHeight*sizeof(DWORD)*2);
        if(!memory){ocrBusy=false;status=detailText=L"图像处理副本超出内存预算，请关闭其他贴图或缩小图片后重试。";SetWindowTextW(resultEdit,detailText.c_str());invalidate();return;}
        ocrBusy = true;
        cancelTranslation = false;
        status = action == L"translate" ? L"正在识别并翻译选区…" : L"正在本地识别文字…";
        invalidate();
        auto bitmap = (HBITMAP)CopyImage(originalImage, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        auto owned = std::shared_ptr<std::remove_pointer_t<HBITMAP>>(bitmap, [](HBITMAP b) {
            if (b)
                DeleteObject(b);
        });
        auto translation = config.translation;
        ocrTasks.add(
            [this, owned, action, translation, cancellation, version, memory] {
                auto image = owned.get();
                if (*cancellation || version != captureGeneration)
                    return;
                auto temp = data / (L"capture-" + std::to_wstring(GetTickCount64()) + L".png");
                try {
                    if (!saveBitmapPng(image, temp))
                        throw std::runtime_error("截图临时文件保存失败");
                    auto ocr = runOcr(executableDirectory() / L"DeskOCR.exe", temp, cancellation.get());
                    std::filesystem::remove(temp);
                    if (*cancellation || version != captureGeneration)
                        return;
                    auto result = std::make_unique<Result>();
                    result->type = 7;
                    result->generation = version;
                    result->text = ocr.text;
                    if (action == L"translate") {
                        if (ocr.lines.empty())
                            throw std::runtime_error("选区中没有识别到可翻译的文字，请调整选区后重试。");
                        std::vector<std::wstring> lines;
                        for (auto &line : ocr.lines)
                            lines.push_back(line.text);
                        auto translated = translateLines(lines, translation, cancellation.get());
                        result->bitmap = renderTranslation(image, ocr, translated);
                        result->imageMemory = memory;
                        result->text.clear();
                        for (auto &line : translated)
                            result->text += line + L"\n";
                        result->status = L"原图翻译完成 · 按住 Space 查看原文";
                    } else
                        result->status = ocr.text.empty() ? L"选区中没有识别到文字"
                                                          : L"本地 OCR 完成 · 可复制文字";
                    publish(result.release());
                } catch (const std::exception &e) {
                    std::error_code ec;
                    std::filesystem::remove(temp, ec);
                    if (!*cancellation && version == captureGeneration) {
                        auto r = new Result;
                        r->type = 8;
                        r->generation = version;
                        r->status = userError(e);
                        publish(r);
                    }
                }
                if (version == captureGeneration)
                    ocrBusy = false;
            },
            true);
    }
    void receive(Result *raw) {
        std::unique_ptr<Result> r(raw);
        if(r->type==27){
            if(r->generation!=generation||r->listRequest!=listRequest||mode!=0||!listInFlight)return;
            int64_t focus=selected>=0&&selected<(int)fileRows.size()?fileRows[selected].id:0;
            fileRows=std::move(r->files);pendingQuery=false;fileSelection.clear();selected=0;
            for(int i=0;i<(int)fileRows.size();++i){if(fileRows[i].id==focus)selected=i;if(fileSelectedItems.contains(fileRows[i].id))fileSelection.insert(i);}
            if(fileSelectedItems.empty()&&!fileRows.empty()){fileSelection.insert(selected);fileSelectedItems[fileRows[selected].id]=fileRows[selected];}
            invalidate();return;
        }
        if (r->type == 0) {
            if (r->generation != generation || (r->listAction && r->listRequest != listRequest)) return;
            bool wasPending = pendingQuery;
            pendingQuery = false;listInFlight = false;
            std::set<int64_t> selectedIds, previousFileIds;
            int64_t focusId = listFocusedId, anchorId = fileAnchorId, topId = 0;
            if (mode == 0) {
                for (auto& item : fileRows) previousFileIds.insert(item.id);
                for (auto& [id, item] : fileSelectedItems) selectedIds.insert(id);
                for (int row : fileSelection)
                    if (row >= 0 && row < (int)fileRows.size()) selectedIds.insert(fileRows[row].id);
                if (!focusId && selected >= 0 && selected < (int)fileRows.size()) focusId = fileRows[selected].id;
                if (!anchorId && fileAnchor >= 0 && fileAnchor < (int)fileRows.size()) anchorId = fileRows[fileAnchor].id;
                if (scroll >= 0 && scroll < (int)fileRows.size()) topId = fileRows[scroll].id;
            } else {
                for (auto& item : historyRows) previousFileIds.insert(item.id);
                if (!focusId && selected >= 0 && selected < (int)historyRows.size()) focusId = historyRows[selected].id;
                if (scroll >= 0 && scroll < (int)historyRows.size()) topId = historyRows[scroll].id;
            }
            auto merge = [&]<typename Item>(std::vector<Item>& rows, std::vector<Item>& incoming) {
                std::set<int64_t> seen;
                if (r->listAction == 2 || r->listAction == 3)
                    for (auto& row : rows) seen.insert(row.id);
                std::vector<Item> added;added.reserve(incoming.size());
                size_t offset = 0;
                for (auto& page : r->listPages) {
                    auto count = page.count;page.count = 0;
                    for (size_t i = 0; i < count && offset < incoming.size(); ++i, ++offset)
                        if (seen.insert(incoming[offset].id).second) {
                            added.push_back(std::move(incoming[offset]));++page.count;
                        }
                }
                if (!r->listAction)
                    for (auto& row : incoming)
                        if (seen.insert(row.id).second) added.push_back(std::move(row));
                if (r->listAction == 2)
                    rows.insert(rows.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
                else if (r->listAction == 3)
                    rows.insert(rows.begin(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
                else rows = std::move(added);
            };
            if (mode == 0) { merge(fileRows, r->files);historyRows.clear(); }
            else { merge(historyRows, r->history);fileRows.clear(); }
            size_t removedFront = 0;
            if (r->listAction) {
                try {
                    for (auto& page : r->listPages) listCheckpoints.put(page.number, page.cursor);
                    if (r->listAction == 2) {
                        for (auto& page : r->listPages) if (page.count) listPages.push_back(page);
                        listEnd = r->listEnd || r->listPages.empty() || !r->listPages.back().count;
                    } else if (r->listAction == 3) {
                        for (auto it = r->listPages.rbegin(); it != r->listPages.rend(); ++it)
                            listPages.push_front(*it);
                    } else {
                        listPages.assign(r->listPages.begin(), r->listPages.end());
                        listEnd = r->listEnd;listHeadDirty = false;
                        if (!listPages.empty()) listCheckpoints.eraseAfter(listPages.back().number);
                    }
                    auto trim = [&]<typename Item>(std::vector<Item>& rows) {
                        while (rows.size() > listRowLimit && listPages.size() > 1) {
                            if (r->listAction == 3) {
                                size_t count = listPages.back().count;listPages.pop_back();
                                rows.erase(rows.end() - count, rows.end());listEnd = false;
                            } else {
                                size_t count = listPages.front().count;listPages.pop_front();
                                rows.erase(rows.begin(), rows.begin() + count);removedFront += count;
                            }
                        }
                    };
                    if (mode == 0) trim(fileRows);else trim(historyRows);
                } catch (const std::exception& e) { status = userError(e);listEnd = true; }
                listTotal = r->listTotal;
            }
            indexStatus = r->index;status = r->status;
            int count = mode == 0 ? (int)fileRows.size() : (int)historyRows.size();
            selected = std::clamp(selected - (int)removedFront, 0, std::max(0, count - 1));
            scroll = std::max(0, scroll - (int)removedFront);
            int mappedAnchor = -1, mappedFocus = -1;
            if (mode == 0) {
                fileSelection.clear();
                if (r->listAction == 4 || !r->listAction) {
                    std::set<int64_t> present;
                    for (auto& item : fileRows) present.insert(item.id);
                    for (auto id : previousFileIds)
                        if (!present.contains(id)) { fileSelectedItems.erase(id);selectedIds.erase(id); }
                }
                for (int i = 0; i < count; ++i) {
                    if (selectedIds.contains(fileRows[i].id)) {
                        fileSelection.insert(i);fileSelectedItems[fileRows[i].id] = fileRows[i];
                    }
                    if (fileRows[i].id == focusId) selected = mappedFocus = i;
                    if (fileRows[i].id == anchorId) mappedAnchor = i;
                    if (fileRows[i].id == topId) scroll = i;
                }
                fileAnchor = mappedAnchor >= 0 ? mappedAnchor : std::max(selected, 0);
                if (fileSelectedItems.empty() && fileSelection.empty() && count && (wasPending || r->listAction == 0 || r->listAction == 4)) {
                    fileSelection.insert(selected);fileSelectedItems.emplace(fileRows[selected].id, fileRows[selected]);
                }
            } else {
                for (int i = 0; i < count; ++i) {
                    if (historyRows[i].id == focusId) selected = mappedFocus = i;
                    if (historyRows[i].id == topId) scroll = i;
                }
            }
            if (focusId && mappedFocus < 0 && (r->listAction == 2 || r->listAction == 3 ||
                (r->listAction == 4 && !previousFileIds.contains(focusId)))) selected = -1;
            else if (count && selected >= 0) {
                listFocusedId = mode == 0 ? fileRows[selected].id : historyRows[selected].id;
                if (mode == 0 && (!anchorId || (mappedAnchor < 0 && previousFileIds.contains(anchorId) &&
                    (r->listAction == 0 || r->listAction == 4)))) fileAnchorId = fileRows[fileAnchor].id;
            }
            scroll = std::clamp(scroll, 0, std::max(0, count - visibleRows()));
            if (!count) {
                detailText.clear();if (previewImage) DeleteObject(previewImage);previewImage = nullptr;
            } else if (r->listAction != 2 && r->listAction != 3) preview();
        } else if (r->type == 26) {
            if (r->generation != generation || r->listRequest != listRequest) return;
            listInFlight = false;
            if (r->listTotal != listTotal) {
                listHeadDirty = true;
                if (mode == 0) listEnd = false;
            }
            listTotal = r->listTotal;indexStatus = r->index;status = r->status;
        } else if (r->type == 2)
            status = r->status;
        else if (r->type == 9) {
            if (r->generation != generation)
                return;
            if (r->listAction && r->listRequest != listRequest) return;
            listInFlight = false;status = r->status;
            if (pendingQuery || !r->listAction) {
                historyRows.clear();fileRows.clear();listPages.clear();fileSelection.clear();fileSelectedItems.clear();
                detailText.clear();if (previewImage) DeleteObject(previewImage);previewImage = nullptr;
            }
            pendingQuery = false;
        } else if (r->type == 3) {
            if (r->generation != previewGeneration || mode != 1 || !historyPreviewRequested || !IsWindowVisible(window) || selected<0 || selected>=(int)historyRows.size())
                return;
            detailText = r->text;
            auto& item=historyRows[selected];
            floatingPreview.show(window,item,detailText,r->bitmap,previewAnchor(),r->listAction!=0,config.dark);
            r->bitmap = nullptr;
        } else if (r->type == 20) {
            if (r->generation != previewGeneration || mode != 0 || !filePreviewVisible || !IsWindowVisible(window)) return;
            detailText = std::move(r->text);
            HistoryItem item;item.kind=L"文件";item.title=selected>=0&&selected<(int)fileRows.size()?fileRows[selected].name:L"文件";
            floatingPreview.show(window,item,detailText,r->bitmap,previewAnchor(),true,config.dark);r->bitmap = nullptr;
        } else if (r->type == 21) {
            if(r->generation!=previewGeneration||mode!=1)return;
            auto bitmap=r->bitmap;r->bitmap=nullptr;
            if(r->text==L"pin")pinImage(window,bitmap,[this](HBITMAP image,const std::wstring& action){acceptCapturedImage(image,action);});
            else acceptCapturedImage(bitmap,r->text);
        } else if (r->type == 22) {
            if(r->generation!=captureGeneration)return;
            auto bitmap=r->bitmap;r->bitmap=nullptr;acceptCapturedImage(bitmap,L"image");
        } else if (r->type == 4 || r->type == 5) {
            try {
                if (!restoreClipboardPayload(window, r->payload))
                    throw std::runtime_error("其他应用正在占用剪贴板，请重试");
                ignoredSequence = GetClipboardSequenceNumber();
                status = L"已复制到剪贴板";
                if (r->type == 4) {
                    DWORD currentPid = 0;
                    if (r->pasteTarget)
                        GetWindowThreadProcessId(r->pasteTarget, &currentPid);
                    bool valid =
                        r->pasteTarget && IsWindow(r->pasteTarget) && currentPid == r->pastePid;
                    bool active = GetForegroundWindow() == window;
                    if (valid && active) {
                        previousWindow = r->pasteTarget;
                        hide();
                        SetForegroundWindow(r->pasteTarget);
                    }
                    if (valid && active && GetForegroundWindow() == r->pasteTarget) {
                        INPUT inputs[4]{};
                        inputs[0].type = inputs[1].type = inputs[2].type = inputs[3].type =
                            INPUT_KEYBOARD;
                        inputs[0].ki.wVk = VK_CONTROL;
                        inputs[1].ki.wVk = 'V';
                        inputs[2].ki.wVk = 'V';
                        inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
                        inputs[3].ki.wVk = VK_CONTROL;
                        inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
                        if (SendInput(4, inputs, sizeof(INPUT)) != 4)
                            status = L"已复制；目标窗口未接受自动粘贴";
                    } else
                        status = L"已复制，未粘贴：原目标已关闭、焦点已改变或 Windows 不允许切换";
                }
            } catch (const std::exception &e) {
                status = userError(e);
            }
        } else if (r->type == 6) {
            if (IsWindowVisible(window))
                query(false, true);
        } else if (r->type == 7) {
            if (cancelTranslation || r->generation != captureGeneration || mode != 2)
                return;
            detailText = r->text;
            status = r->status;
            if (resultImage)
                DeleteObject(resultImage);
            resultImage = r->bitmap;
            resultMemory = std::move(r->imageMemory);
            r->bitmap = nullptr;
            invalidate();
        } else if (r->type == 8) {
            if (r->generation != captureGeneration || mode != 2)
                return;
            status = r->status;
            detailText =
                L"识别或翻译未完成。\n\n" + r->status +
                L"\n\n可以重新框选后重试；翻译服务的连接与 API Key 可在设置中测试。";
        } else if (r->type == 12) {
            bool changed = recordingWarning != r->status;
            recordingWarning = r->status;
            if (changed && !smoke) {
                auto notification = tray;
                notification.uFlags = NIF_INFO;
                wcscpy_s(notification.szInfoTitle, L"DeskFlow 历史记录受限");
                wcsncpy_s(notification.szInfo, r->status.c_str(), _TRUNCATE);
                notification.dwInfoFlags = NIIF_WARNING;
                Shell_NotifyIconW(NIM_MODIFY, &notification);
            }
        } else if (r->type == 13) {
            recordingWarning.clear();
            if (IsWindowVisible(window) && mode == 1) {
                listHeadDirty = true;query(false, true);
            }
        } else if (r->type == 11) {
            auto executable = executableDirectory() / L"DeskIndex.exe";
            auto parameters = L"--data \"" + data.wstring() + L"\" --parent " +
                              std::to_wstring(GetCurrentProcessId());
            SHELLEXECUTEINFOW execute{sizeof(execute)};
            execute.fMask = SEE_MASK_NOCLOSEPROCESS;
            execute.hwnd = settingsWindow ? settingsWindow : window;
            execute.lpVerb = L"runas";
            execute.lpFile = executable.c_str();
            execute.lpParameters = parameters.c_str();
            execute.nShow = SW_HIDE;
            if (ShellExecuteExW(&execute)) {
                workerHandle = execute.hProcess;
                config.elevatedIndex = true;
                try {
                    saveSettings(data / L"settings.json", config);
                } catch (const std::exception &e) {
                    status = L"管理员索引已启动，但偏好未保存：" + userError(e);
                    invalidate();
                    return;
                }
                status = L"已启动索引辅助程序，当前覆盖范围会在文件页显示";
            } else {
                status = L"未启用管理员索引，继续使用普通目录模式";
                launchWorker();
            }
        }
        if(resultEdit&&mode==2&&(r->type==7||r->type==8||r->type==21||r->type==22))SetWindowTextW(resultEdit,detailText.c_str());
        invalidate();
    }
    void clipboardChanged() {
        DWORD sequence = GetClipboardSequenceNumber();
        if (smoke || paused || !history || sequence == ignoredSequence ||
            clipboardQueued.exchange(true))
            return;
        auto excludedApps = config.excludedApps;
        auto maximumBytes = (size_t)config.maximumEntryMiB * 1024 * 1024;
        bool queued = clipboardReaderTasks.add([this, excludedApps, sequence, maximumBytes] {
            try {
                auto p = readClipboardPayload(window, maximumBytes);
                auto lower = [](std::wstring value) {
                    std::transform(value.begin(), value.end(), value.begin(), towlower);
                    return value;
                };
                auto source = lower(p.source);
                auto rules = lower(excludedApps);
                bool excluded = false;
                size_t begin = 0;
                while (begin < rules.size()) {
                    auto end = rules.find_first_of(L";,\n", begin);
                    auto rule = rules.substr(begin, end == std::wstring::npos ? end : end - begin);
                    auto left = rule.find_first_not_of(L" \t");
                    auto right = rule.find_last_not_of(L" \t");
                    if (left != std::wstring::npos &&
                        rule.substr(left, right - left + 1) == source && !source.empty())
                        excluded = true;
                    if (end == std::wstring::npos)
                        break;
                    begin = end + 1;
                }
                if (!excluded && !p.formats.empty()) {
                    size_t bytes = 0;
                    for (auto &f : p.formats)
                        bytes += f.data.size();
                    auto existing = pendingClipboardBytes.fetch_add(bytes);
                    if (existing && existing + bytes > 16 * 1024 * 1024) {
                        pendingClipboardBytes.fetch_sub(bytes);
                        throw std::runtime_error("历史写入缓冲已满，这次内容未记录，请稍后重试");
                    }
                    auto payload = std::shared_ptr<ClipPayload>(
                        new ClipPayload(std::move(p)), [this, bytes](ClipPayload *value) {
                            delete value;
                            pendingClipboardBytes.fetch_sub(bytes);
                        });
                    bool accepted = clipboardTasks.add([this, payload] {
                        try {
                            history->append(*payload);
                            auto r = new Result;
                            r->type = 13;
                            publish(r);
                        } catch (const std::exception &e) {
                            auto r = new Result;
                            r->type = 12;
                            r->status = userError(e);
                            publish(r);
                        }
                    });
                    if (!accepted)
                        throw std::runtime_error("历史写入任务拥堵，这次内容未记录，请稍后重试");
                }
            } catch (const std::exception &e) {
                auto r = new Result;
                r->type = 12;
                r->status = userError(e);
                publish(r);
            }
            clipboardQueued = false;
            if (!closing && GetClipboardSequenceNumber() != sequence)
                PostMessageW(window, WM_CLIPBOARDUPDATE, 0, 0);
        });
        if (!queued) {
            clipboardQueued = false;
            recordingWarning = L"历史读取任务拥堵，这次内容未记录，请稍后重试";
            invalidate();
        }
    }
    bool registerKeys(const Settings &desired, bool initial = false) {
        if (initial) {
            hotkeyWarning.clear();
            bool all = true;
            for (int i = 0; i < 3; i++) {
                auto key = desired.hotkeys[i];
                if (!RegisterHotKey(window, i + 1, key.modifiers | MOD_NOREPEAT, key.key)) {
                    all = false;
                    hotkeyWarning += hotkeyText(key) + L" 已被占用；";
                }
            }
            if (!all) {
                hotkeyWarning += L"请在设置中更换";
                status = hotkeyWarning;
            }
            return all;
        }
        if (!initial)
            for (int i = 0; i < 3; i++)
                UnregisterHotKey(window, i + 1);
        int registered = 0;
        for (int i = 0; i < 3; i++) {
            auto key = desired.hotkeys[i];
            if (!RegisterHotKey(window, i + 1, key.modifiers | MOD_NOREPEAT, key.key)) {
                for (int j = 0; j < registered; j++)
                    UnregisterHotKey(window, j + 1);
                if (!initial)
                    for (int j = 0; j < 3; j++)
                        RegisterHotKey(window, j + 1, config.hotkeys[j].modifiers | MOD_NOREPEAT,
                                       config.hotkeys[j].key);
                hotkeyWarning = status = L"快捷键被占用：" + hotkeyText(key) + L"；请在设置里更换";
                return false;
            }
            registered++;
        }
        hotkeyWarning.clear();
        return true;
    }
    void updateAutoStart() {
        UINT32 packageNameLength = 0;
        if (GetCurrentPackageFullName(&packageNameLength, nullptr) != APPMODEL_ERROR_NO_PACKAGE) {
            bool enable = config.autoStart;
            clipboardTasks.add([this, enable] {
                try {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    struct ApartmentGuard {
                        ~ApartmentGuard() {
                            winrt::uninit_apartment();
                        }
                    } guard;
                    auto task =
                        winrt::Windows::ApplicationModel::StartupTask::GetAsync(L"DeskFlowStartup")
                            .get();
                    if (enable) {
                        auto state = task.RequestEnableAsync().get();
                        using State = winrt::Windows::ApplicationModel::StartupTaskState;
                        if (state != State::Enabled && state != State::EnabledByPolicy)
                            throw std::runtime_error(
                                "Windows 未允许自动启动，请在系统的启动应用设置中检查 DeskFlow");
                    } else
                        task.Disable();
                } catch (const std::exception &e) {
                    error(e);
                } catch (...) {
                    auto r = new Result;
                    r->type = 2;
                    r->status = L"Windows 启动设置未更新，请在系统设置中检查";
                    publish(r);
                }
            });
            HKEY oldKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                              KEY_SET_VALUE, &oldKey) == ERROR_SUCCESS) {
                RegDeleteValueW(oldKey, L"DeskFlow");
                RegCloseKey(oldKey);
            }
            return;
        }
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                            0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return;
        if (config.autoStart) {
            auto exe = executableDirectory() / L"DeskFlow.exe";
            auto line = L"\"" + exe.wstring() + L"\" --tray";
            RegSetValueExW(key, L"DeskFlow", 0, REG_SZ, (BYTE *)line.c_str(),
                           (DWORD)((line.size() + 1) * 2));
        } else
            RegDeleteValueW(key, L"DeskFlow");
        RegCloseKey(key);
    }
    void addTray() {
        tray.cbSize = sizeof(tray);
        tray.hWnd = window;
        tray.uID = 1;
        tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray.uCallbackMessage = TrayMessage;
        tray.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
        if (!tray.hIcon) tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray.szTip, L"DeskFlow · 文件 / 剪贴板 / 截图");
        Shell_NotifyIconW(NIM_ADD, &tray);
        tray.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray);
    }
    void trayMenu() {
        HWND foreground = GetForegroundWindow();
        if (externalWindow(foreground))
            previousWindow = foreground;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 10, L"文件搜索");
        AppendMenuW(menu, MF_STRING, 11, L"剪贴板历史");
        AppendMenuW(menu, MF_STRING, 12, L"截图");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 13, paused ? L"继续记录剪贴板" : L"暂停记录剪贴板");
        AppendMenuW(menu, MF_STRING, 14, L"设置");
        AppendMenuW(menu, MF_STRING, 15, L"退出");
        POINT point;
        GetCursorPos(&point);
        SetForegroundWindow(window);
        int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0,
                                    window, nullptr);
        DestroyMenu(menu);
        if (action == 10)
            show(0);
        if (action == 11)
            show(1);
        if (action == 12)
            capture();
        if (action == 13) {
            paused = !paused;
            invalidate();
        }
        if (action == 14)
            openSettings();
        if (action == 15)
            quit();
    }
    void launchWorker() {
        if (smoke)
            return;
        auto exe = executableDirectory() / L"DeskIndex.exe";
        auto command = L"\"" + exe.wstring() + L"\" --data \"" + data.wstring() + L"\" --parent " +
                       std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process)) {
            workerHandle = process.hProcess;
            CloseHandle(process.hThread);
        } else
            status = L"索引进程未启动，请确认 DeskIndex.exe 位于程序目录";
    }
    void elevateIndex() {
        if (smoke) {
            status = L"验证模式不请求管理员权限";
            return;
        }
        auto process = workerHandle;
        workerHandle = nullptr;
        clipboardTasks.add([this, process] {
            stopIndexWorker(data);
            if (process) {
                WaitForSingleObject(process, 5000);
                CloseHandle(process);
            }
            auto r = new Result;
            r->type = 11;
            publish(r);
        });
        status = L"正在切换索引模式…";
        invalidate();
    }
    void enableLocalOcr() {
        if (setupHandle) {
            status = L"安装正在进行，请完成 Windows 管理员确认";
            invalidate();
            return;
        }
        UINT32 size = 0;
        if (GetCurrentPackageFullName(&size, nullptr) != APPMODEL_ERROR_NO_PACKAGE) {
            MessageBoxW(settingsWindow,
                        L"当前运行的是安装版。框选屏幕后按 O 即可测试本地 "
                        L"OCR。若缺少识别语言，工具会提示。",
                        L"DeskFlow", MB_OK);
            return;
        }
        auto directory = executableDirectory();
        auto bundle = directory / L"Setup";
        if (!std::filesystem::exists(bundle / L"Install-DeskFlow.ps1"))
            bundle = directory.parent_path().parent_path() / L"artifacts" / L"installer";
        auto installer = bundle / L"Install-DeskFlow.ps1";
        if (!std::filesystem::exists(installer))
            throw std::runtime_error(
                "自动安装文件缺失，请使用随软件附带的 Install-DeskFlow.cmd 安装入口");
        wchar_t system[32768];
        GetSystemDirectoryW(system, 32768);
        auto engine =
            std::filesystem::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
        setupReceipt = data / (L"setup-result-" + std::to_wstring(GetTickCount64()) + L".json");
        auto command = L"\"" + engine.wstring() +
                       L"\" -NoProfile -ExecutionPolicy Bypass -File \"" + installer.wstring() +
                       L"\" -BundleDirectory \"" + bundle.wstring() + L"\" -ParentProcessId " +
                       std::to_wstring(GetCurrentProcessId()) + L" -ParentWindowHandle " +
                       std::to_wstring((uintptr_t)window) + L" -ReceiptPath \"" +
                       setupReceipt.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(engine.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            throw std::runtime_error("自动安装程序无法启动");
        CloseHandle(process.hThread);
        setupHandle = process.hProcess;
        SetTimer(window, 17, 250, nullptr);
        status = L"自动安装已开始：请在 Windows 管理员确认中选择“是”，完成后安装版会重新打开。";
        invalidate();
    }
    static bool externalWindow(HWND window) {
        if (!window || !IsWindow(window))
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId())
            return false;
        wchar_t name[128]{};
        GetClassNameW(window, name, 128);
        return wcscmp(name, L"Shell_TrayWnd") && wcscmp(name, L"Shell_SecondaryTrayWnd") &&
               wcscmp(name, L"Progman") && wcscmp(name, L"WorkerW");
    }
    static void CALLBACK foregroundEvent(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD,
                                         DWORD) {
        auto app = foregroundTracker;
        if (app && !app->closing && externalWindow(window))
            app->previousWindow = window;
    }
    void quit() {
        closing = true;
        cancelTranslation = true;
        shutdownImageTools();
        shutdownRecording();
        RemoveClipboardFormatListener(window);
        for (int i = 1; i <= 3; i++)
            UnregisterHotKey(window, i);
        Shell_NotifyIconW(NIM_DELETE, &tray);
        if (settingsWindow)
            DestroyWindow(settingsWindow);
        DestroyWindow(window);
    }
    static LRESULT CALLBACK editProcedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                          DWORD_PTR ref) {
        auto app = (Application *)ref;
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if (app->handleFileKey(hwnd, wp)) return 0;
            if (wp == VK_ESCAPE) {
                app->hide();
                return 0;
            }
            if (wp == VK_UP || wp == VK_DOWN) {
                app->select(app->selected + (wp == VK_UP ? -1 : 1));
                return 0;
            }
            if (app->mode == 1 && (wp == VK_PRIOR || wp == VK_NEXT)) {
                app->select(app->selected + (wp == VK_PRIOR ? -app->visibleRows() : app->visibleRows()));
                return 0;
            }
            if (wp == VK_RETURN) {
                app->activate(true);
                return 0;
            }
            if (wp == VK_DELETE && controlText(hwnd).empty()) {
                app->erase();
                return 0;
            }
            if (wp == 'D' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                app->pin();
                return 0;
            }
            if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                DWORD a, b;
                SendMessageW(hwnd, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
                if (a == b) {
                    app->activate(false);
                    return 0;
                }
            }
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    static LRESULT CALLBACK resultProcedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR ref) {
        auto app=(Application*)ref;
        if(msg==WM_KEYDOWN&&wp==VK_ESCAPE){app->hide();return 0;}
        if(msg==WM_KEYDOWN&&wp=='V'&&(GetKeyState(VK_CONTROL)&0x8000)){app->pasteImage();return 0;}
        if((msg==WM_KEYDOWN||msg==WM_KEYUP)&&wp==VK_SPACE){app->showOriginal=msg==WM_KEYDOWN;app->invalidate();return 0;}
        return DefSubclassProc(hwnd,msg,wp,lp);
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Application *app = (Application *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            app = (Application *)((CREATESTRUCTW *)lp)->lpCreateParams;
            app->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
        }
        if (!app)
            return DefWindowProcW(hwnd, msg, wp, lp);
        try {
            if (msg == app->taskbarMessage && msg != 0) {
                app->addTray();
                return 0;
            }
            switch (msg) {
            case WM_APP + 5:
                app->quit();
                return 0;
            case WM_GETOBJECT:
                if ((LONG)lp == OBJID_CLIENT)
                    return desk::accessibleObject(
                        hwnd, wp, lp,
                        [app] {
                            AccessibleView view;
                            view.label = app->mode == 0   ? L"文件搜索结果"
                                         : app->mode == 1 ? L"剪贴板历史"
                                                          : L"截图和识别";
                            if (app->mode == 2)
                                return view;
                            POINT origin{};
                            ClientToScreen(app->window, &origin);
                            RECT client{};
                            GetClientRect(app->window, &client);
                            float right = client.right/app->scale-8;
                            size_t count =
                                app->mode == 0 ? app->fileRows.size() : app->historyRows.size();
                            for (size_t i = 0; i < count; i++) {
                                AccessibleRow row;
                                row.name = app->mode == 0 ? app->fileRows[i].name + L"，" +
                                                                app->fileRows[i].path
                                                          : app->historyRows[i].kind + L"，" +
                                                                app->historyRows[i].title;
                                float rowHeight = panel_layout::rowHeight(app->mode);
                                float y = panel_layout::rowTop(app->mode) + (int(i) - app->scroll) * rowHeight;
                                row.bounds = {origin.x + (LONG)(8 * app->scale),
                                              origin.y + (LONG)(y * app->scale),
                                              origin.x + (LONG)(right * app->scale),
                                              origin.y + (LONG)((y + rowHeight - 2) * app->scale)};
                                row.selected = app->mode == 0 ? app->fileSelection.contains((int)i) : (int)i == app->selected;
                                view.rows.push_back(std::move(row));
                            }
                            return view;
                        },
                        [app](int row) {
                            if (!app->closing) {
                                app->select(row);
                                if(app->mode==0)app->fileDoubleClick(row,12);else app->activate(true);
                            }
                        });
                break;
            case WM_CREATE: {
                foregroundTracker = app;
                app->foregroundHook = SetWinEventHook(
                    EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundEvent, 0,
                    0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
                app->uiFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                app->edit =
                    CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 44,
                                    169, 860, 29, hwnd, (HMENU)1, app->instance, nullptr);
                app->resultEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,
                    600,283,340,240,hwnd,(HMENU)2,app->instance,nullptr);
                SendMessageW(app->resultEdit,EM_SETLIMITTEXT,4*1024*1024,0);
                SetWindowSubclass(app->resultEdit,resultProcedure,2,(DWORD_PTR)app);
                SendMessageW(app->edit, WM_SETFONT, (WPARAM)app->uiFont, TRUE);
                SendMessageW(app->edit, EM_SETCUEBANNER, TRUE,
                             (LPARAM)L"搜索文件名或路径 · 例如 项目  ext:pdf");
                SetWindowSubclass(app->edit, editProcedure, 1, (DWORD_PTR)app);
                app->layout();
                app->createMenuTooltip();
                if (!app->smoke) app->registerKeys(app->config, true);
                if (!app->smoke && app->history)
                    AddClipboardFormatListener(hwnd);
                app->taskbarMessage = RegisterWindowMessageW(L"TaskbarCreated");
                app->addTray();
                app->launchWorker();
                if (app->config.elevatedIndex && !app->smoke)
                    SetTimer(hwnd, 14, 500, nullptr);
                if (app->smoke)
                    SetTimer(hwnd, 9, app->testDurationMs, nullptr);
                return 0;
            }
            case WM_NCCALCSIZE:
                break;
            case WM_GETMINMAXINFO: {
                auto info = (MINMAXINFO *)lp;
                float dpiScale=GetDpiForWindow(hwnd)/96.f;
                info->ptMinTrackSize = {(LONG)(540*dpiScale), (LONG)(300*dpiScale)};
                return 0;
            }
            case WM_SIZE:
                app->layout();
                return 0;
            case WM_DPICHANGED: {
                auto r = (RECT *)lp;
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                app->layout();
                return 0;
            }
            case WM_PAINT:
                app->paint();
                return 0;
            case WM_NOTIFY:{auto header=(NMHDR*)lp;if(header&&header->hwndFrom==app->menuTooltip&&header->code==TTN_GETDISPINFOW){auto display=(NMTTDISPINFOW*)lp;int index=(int)header->idFrom-1;if(index>=0&&index<4){app->menuHints[index]=app->functionHint(index);display->lpszText=app->menuHints[index].data();}return 0;}break;}
            case WM_ERASEBKGND:
                return 1;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT: {
                auto dc = (HDC)wp;
                SetTextColor(dc, app->dark() ? RGB(230, 234, 241) : RGB(35, 43, 60));
                if((HWND)lp==app->resultEdit){
                    SetBkColor(dc,app->dark()?RGB(28,31,40):RGB(255,255,255));
                    static HBRUSH resultLight=CreateSolidBrush(RGB(255,255,255)),resultDark=CreateSolidBrush(RGB(28,31,40));
                    return (LRESULT)(app->dark()?resultDark:resultLight);
                }
                SetBkColor(dc, app->dark() ? RGB(36, 40, 51) : RGB(239, 242, 248));
                static HBRUSH light = CreateSolidBrush(RGB(239, 242, 248)),
                              darkBrush = CreateSolidBrush(RGB(36, 40, 51));
                return (LRESULT)(app->dark() ? darkBrush : light);
            }
            case WM_COMMAND:
                if (HIWORD(wp) == EN_CHANGE && (HWND)lp == app->edit) {
                    app->pendingQuery = true;
                    ++app->generation;
                    ++app->previewGeneration;
                    app->listInFlight = false;
                    app->fileSelection.clear();
                    SetTimer(hwnd, 1, 70, nullptr);
                }
                return 0;
            case WM_TIMER:
                if (wp == 17 && app->setupHandle &&
                    WaitForSingleObject(app->setupHandle, 0) == WAIT_OBJECT_0) {
                    DWORD result = 1;
                    GetExitCodeProcess(app->setupHandle, &result);
                    CloseHandle(app->setupHandle);
                    app->setupHandle = nullptr;
                    KillTimer(hwnd, 17);
                    app->status = result == 0 ? L"安装完成，请从开始菜单打开 DeskFlow"
                                              : L"安装未完成或管理员确认已取消";
                    if (result != 0 && std::filesystem::exists(app->setupReceipt)) {
                        try {
                            std::ifstream file(app->setupReceipt);
                            nlohmann::json receipt;
                            file >> receipt;
                            app->status = L"安装未完成：" + wide(receipt.value("error", ""));
                        } catch (...) {
                        }
                    }
                    app->invalidate();
                    return 0;
                }
                if (wp == 14) {
                    KillTimer(hwnd, 14);
                    app->elevateIndex();
                    return 0;
                }
                if (wp == 1) {
                    KillTimer(hwnd, 1);
                    app->query(true);
                }
                if (wp == 2 && IsWindowVisible(hwnd) && app->mode == 0)
                    app->query(false, true);
                if (wp == 9) {
                    PROCESS_MEMORY_COUNTERS_EX memory{};
                    memory.cb = sizeof(memory);
                    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory,
                                         sizeof(memory));
                    nlohmann::json report = {
                        {"privateBytes", memory.PrivateUsage},
                        {"workingSet", memory.WorkingSetSize},
                        {"firstInteractiveFrameMs", app->firstFrameMs},
                        {"gdiObjects", GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)},
                        {"userObjects", GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)},
                        {"windowCreated", true}};
                    std::ofstream output(app->data / L"smoke.json");
                    output << report.dump(2);
                    output.close();
                    app->quit();
                }
                return 0;
            case WM_HOTKEY:
                if (!app->settingsWindow) {
                    if (wp == 1)
                        app->show(0);
                    if (wp == 2)
                        app->show(1);
                    if (wp == 3)
                        app->capture();
                }
                return 0;
            case WM_CLIPBOARDUPDATE:
                app->clipboardChanged();
                return 0;
            case DoneMessage:
                app->receive((Result *)lp);
                return 0;
            case TrayMessage:
                if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_SELECT)
                    app->show(0);
                if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
                    app->trayMenu();
                return 0;
            case WM_LBUTTONDOWN: {
                float x = (short)LOWORD(lp) / app->scale, y = (short)HIWORD(lp) / app->scale;
                for (auto b : app->buttons)
                    if (x >= b.first.left && x <= b.first.right && y >= b.first.top &&
                        y <= b.first.bottom) {
                        b.second();
                        return 0;
                    }
                int row=app->rowAtPoint(x,y);
                if(row>=0) {
                        app->collapseFileSelectionOnMouseUp=false;
                        if(app->mode==0&&!(wp&(MK_CONTROL|MK_SHIFT))&&app->fileSelection.size()>1&&app->fileSelection.contains(row)) {
                            app->selected=row;app->preview();app->invalidate();
                            app->collapseFileSelectionOnMouseUp=true;
                        }else app->select(row,(wp&MK_SHIFT)!=0,(wp&MK_CONTROL)!=0);
                        if(app->mode==1)app->showHistoryPreview();
                        SetFocus(app->mode==0?hwnd:app->edit);
                        if(app->mode==0){app->fileMouseDown={(short)LOWORD(lp),(short)HIWORD(lp)};app->draggingFiles=true;SetCapture(hwnd);}
                }
                return 0;
            }
            case WM_DISPLAYCHANGE:
            case WM_DWMCOMPOSITIONCHANGED:
                app->resetTarget();app->invalidate();return 0;
            case WM_LBUTTONUP: {
                bool collapse=app->collapseFileSelectionOnMouseUp;
                app->draggingFiles=false;app->collapseFileSelectionOnMouseUp=false;
                if(GetCapture()==hwnd)ReleaseCapture();
                if(collapse)app->select(app->selected);
                return 0;
            }
            case WM_CAPTURECHANGED:
                app->draggingFiles=false;app->collapseFileSelectionOnMouseUp=false;break;
            case WM_MOUSEMOVE:
                if(app->draggingFiles&&(wp&MK_LBUTTON)) {
                    POINT p{(short)LOWORD(lp),(short)HIWORD(lp)};
                    if(abs(p.x-app->fileMouseDown.x)>=GetSystemMetrics(SM_CXDRAG)||abs(p.y-app->fileMouseDown.y)>=GetSystemMetrics(SM_CYDRAG)) {
                        app->draggingFiles=false;ReleaseCapture();
                        auto paths=app->selectedFilePaths();if(!paths.empty())app->fileOperationResult(dragFiles(paths),L"文件拖动完成");
                    }
                }
                return 0;
            case WM_CONTEXTMENU: {
                if((HWND)wp==app->edit)break;
                if(app->mode==1){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(p.x!=-1||p.y!=-1){ScreenToClient(hwnd,&p);int row=app->rowAtPoint(p.x/app->scale,p.y/app->scale);if(row<0)return 0;app->select(row);}app->historyActionsMenu();return 0;}
                if(app->mode!=0)break;
                POINT screen{(short)LOWORD(lp),(short)HIWORD(lp)};
                if(screen.x==-1&&screen.y==-1){screen={(LONG)(40*app->scale),(LONG)((panel_layout::fileRowsTop+13+(app->selected-app->scroll)*panel_layout::fileRowHeight)*app->scale)};ClientToScreen(hwnd,&screen);}
                POINT local=screen;ScreenToClient(hwnd,&local);
                RECT client{};GetClientRect(hwnd,&client);
                int hitRow=app->rowAtPoint(local.x/app->scale,local.y/app->scale);
                if(hitRow>=0) {
                    int row=hitRow;
                    if(row<(int)app->fileRows.size()){
                        if(!app->fileSelection.contains(row))app->select(row);
                        app->fileActionsMenu(true,screen);
                    }
                }
                return 0;
            }
            case WM_LBUTTONDBLCLK: {
                int row=app->rowAtPoint((short)LOWORD(lp)/app->scale,(short)HIWORD(lp)/app->scale);
                if(row>=0) {
                    if(app->selected!=row)app->select(row);
                    if(app->mode==0)app->fileDoubleClick(row,(short)LOWORD(lp)/app->scale);else app->activate(true);
                }
                return 0;
            }
            case WM_MOUSEWHEEL:
                app->wheelList(GET_WHEEL_DELTA_WPARAM(wp));
                return 0;
            case WM_SYSKEYDOWN:
            case WM_KEYDOWN:
                if(app->handleFileKey(hwnd,wp))return 0;
                if(app->mode==2&&wp=='V'&&(GetKeyState(VK_CONTROL)&0x8000)){app->pasteImage();return 0;}
                if (wp == VK_ESCAPE) {
                    app->hide();
                    return 0;
                }
                if (wp == VK_SPACE && app->mode == 2) {
                    app->showOriginal = true;
                    app->invalidate();
                    return 0;
                }
                if (wp == VK_UP || wp == VK_DOWN) {
                    app->select(app->selected + (wp == VK_UP ? -1 : 1));
                    return 0;
                }
                if (app->mode == 1 && (wp == VK_PRIOR || wp == VK_NEXT)) {
                    app->select(app->selected + (wp == VK_PRIOR ? -app->visibleRows() : app->visibleRows()));
                    return 0;
                }
                if (wp == VK_RETURN) {
                    app->activate(true);
                    return 0;
                }
                break;
            case WM_KEYUP:
                if (wp == VK_SPACE) {
                    app->showOriginal = false;
                    app->invalidate();
                    return 0;
                }
                break;
            case WM_POWERBROADCAST:
                if (wp == PBT_APMRESUMEAUTOMATIC) {
                    for (int i = 1; i <= 3; i++)
                        UnregisterHotKey(hwnd, i);
                    app->registerKeys(app->config, true);
                }
                return TRUE;
            case WM_CLOSE:
                app->hide();
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
        } catch (const std::exception &e) {
            app->status = userError(e);
            app->invalidate();
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    void openSettings();
    static LRESULT CALLBACK settingsProcedure(HWND, UINT, WPARAM, LPARAM);
};
static WORD nativeHotkey(Hotkey h) {
    BYTE mods = 0;
    if (h.modifiers & MOD_CONTROL)
        mods |= HOTKEYF_CONTROL;
    if (h.modifiers & MOD_ALT)
        mods |= HOTKEYF_ALT;
    if (h.modifiers & MOD_SHIFT)
        mods |= HOTKEYF_SHIFT;
    return MAKEWORD(h.key, mods);
}
static Hotkey logicalHotkey(WORD v) {
    UINT mods = 0;
    auto bits = HIBYTE(v);
    if (bits & HOTKEYF_CONTROL)
        mods |= MOD_CONTROL;
    if (bits & HOTKEYF_ALT)
        mods |= MOD_ALT;
    if (bits & HOTKEYF_SHIFT)
        mods |= MOD_SHIFT;
    return {LOBYTE(v), mods};
}
void Application::openSettings() {
    if (settingsWindow) {
        SetForegroundWindow(settingsWindow);
        return;
    }
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = settingsProcedure;
    wc.lpszClassName = L"DeskFlowSettings";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    settingsWindow = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"DeskFlow 设置",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                     CW_USEDEFAULT, 660, 960, window, nullptr, instance, this);
    ShowWindow(settingsWindow, SW_SHOW);
    SetForegroundWindow(settingsWindow);
}
LRESULT CALLBACK Application::settingsProcedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto app = (Application *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        app = (Application *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
    }
    if (!app)
        return DefWindowProcW(hwnd, msg, wp, lp);
    auto control = [&](const wchar_t *cls, const wchar_t *name, DWORD style, int x, int y, int w,
                       int h, int id) {
        auto item = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, name,
                                    WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                                    (HMENU)(INT_PTR)id, app->instance, nullptr);
        SendMessageW(item, WM_SETFONT, (WPARAM)app->uiFont, TRUE);
        return item;
    };
    try {
        switch (msg) {
        case WM_CREATE: {
            auto label = [&](const wchar_t *s, int y) {
                control(L"STATIC", s, 0, 25, y, 180, 25, 0);
            };
            label(L"翻译引擎", 24);
            auto combo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 203,
                                 20, 406, 150, 100);
            for (auto name : {L"Google 免费（实验通道）", L"DeepL API", L"Google 官方 API",
                              L"LibreTranslate 自定义服务"})
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name);
            int provider = app->config.translation.provider == L"deepl"            ? 1
                           : app->config.translation.provider == L"google-api"     ? 2
                           : app->config.translation.provider == L"libretranslate" ? 3
                                                                                   : 0;
            SendMessageW(combo, CB_SETCURSEL, provider, 0);
            label(L"目标语言", 71);
            control(L"EDIT", app->config.translation.target.c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
                    203, 66, 406, 29, 101);
            label(L"DeepL API Key", 118);
            control(L"EDIT", app->config.translation.deeplKey.c_str(),
                    ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 203, 113, 406, 29, 102);
            auto freeBox = control(L"BUTTON", L"使用 DeepL Free 端点", BS_AUTOCHECKBOX | WS_TABSTOP,
                                   203, 151, 400, 28, 103);
            SendMessageW(freeBox, BM_SETCHECK,
                         app->config.translation.deeplFree ? BST_CHECKED : BST_UNCHECKED, 0);
            label(L"Google API Key", 193);
            control(L"EDIT", app->config.translation.googleKey.c_str(),
                    ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 203, 188, 406, 29, 104);
            label(L"LibreTranslate 地址", 240);
            control(L"EDIT", app->config.translation.libreUrl.c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
                    203, 235, 406, 29, 105);
            label(L"代理（空=系统设置）", 287);
            control(L"EDIT", app->config.translation.proxy.c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
                    203, 282, 406, 29, 106);
            label(L"文件搜索快捷键", 341);
            label(L"剪贴板快捷键", 383);
            label(L"截图快捷键", 425);
            for (int i = 0; i < 3; i++) {
                auto key =
                    control(HOTKEY_CLASSW, L"", WS_TABSTOP, 203, 336 + i * 42, 406, 29, 110 + i);
                SendMessageW(key, HKM_SETHOTKEY, nativeHotkey(app->config.hotkeys[i]), 0);
            }
            label(L"不记录的应用名", 475);
            control(L"EDIT", app->config.excludedApps.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 203,
                    470, 406, 29, 120);
            auto startup = control(L"BUTTON", L"登录 Windows 时启动", BS_AUTOCHECKBOX | WS_TABSTOP,
                                   25, 520, 270, 29, 121);
            SendMessageW(startup, BM_SETCHECK, app->config.autoStart ? BST_CHECKED : BST_UNCHECKED,
                         0);
            auto dark = control(L"BUTTON", L"深色界面", BS_AUTOCHECKBOX | WS_TABSTOP, 333, 520, 270,
                                29, 122);
            SendMessageW(dark, BM_SETCHECK, app->config.dark ? BST_CHECKED : BST_UNCHECKED, 0);
            auto administrator = control(L"BUTTON", L"启动时请求 NTFS 管理员索引（会弹出 UAC）",
                                         BS_AUTOCHECKBOX | WS_TABSTOP, 25, 550, 584, 29, 123);
            SendMessageW(administrator, BM_SETCHECK,
                         app->config.elevatedIndex ? BST_CHECKED : BST_UNCHECKED, 0);
            label(L"单条上限（MiB）", 609);
            control(L"EDIT", std::to_wstring(app->config.maximumEntryMiB).c_str(),
                    ES_NUMBER | WS_TABSTOP, 203, 604, 406, 29, 141);
            label(L"图片预算（GiB）", 652);
            control(L"EDIT", std::to_wstring(app->config.imageQuotaGiB).c_str(),
                    ES_NUMBER | WS_TABSTOP, 203, 647, 406, 29, 142);
            control(L"STATIC",
                    L"Key 使用 Windows 账户加密保存。免费通道可能限流。\nOCR "
                    L"需安装带包身份的版本；识别与图片排版在本机完成。",
                    0, 25, 692, 584, 47, 0);
            control(L"BUTTON", L"安装并启用本地 OCR（自动处理证书）", WS_TABSTOP, 25, 744, 584, 35,
                    135);
            control(L"BUTTON", L"启用 NTFS 快速索引（Windows UAC）", WS_TABSTOP, 25, 792, 584, 35,
                    133);
            control(L"BUTTON", L"打开数据目录", WS_TABSTOP, 25, 842, 146, 35, 130);
            control(L"BUTTON", L"备份全部历史", WS_TABSTOP, 183, 842, 146, 35, 131);
            control(L"BUTTON", L"连接测试", WS_TABSTOP, 339, 842, 107, 35, 134);
            control(L"BUTTON", L"保存设置", WS_TABSTOP | BS_DEFPUSHBUTTON, 458, 842, 150, 35, 132);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == 130) {
                ShellExecuteW(hwnd, L"open", app->data.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (LOWORD(wp) == 131) {
                if (!app->history)
                    throw std::runtime_error("历史库暂时不可用，原文件已保留，请先恢复完整备份");
                auto folder = folderDialog(hwnd);
                if (!folder.empty()) {
                    app->clipboardTasks.add([app, folder] {
                        try {
                            app->history->backup(
                                std::filesystem::path(folder) /
                                (L"DeskFlow-backup-" + std::to_wstring(GetTickCount64())));
                            auto r = new Result;
                            r->type = 2;
                            r->status = L"历史及附件已完整备份";
                            app->publish(r);
                        } catch (const std::exception &e) {
                            app->error(e);
                        }
                    });
                    MessageBoxW(hwnd, L"备份已在后台开始。完成状态会显示在主面板底部。",
                                L"DeskFlow", MB_OK);
                }
                return 0;
            }
            if (LOWORD(wp) == 133) {
                app->elevateIndex();
                return 0;
            }
            if (LOWORD(wp) == 135) {
                app->enableLocalOcr();
                return 0;
            }
            if (LOWORD(wp) == 134) {
                auto cfg = app->config.translation;
                int p = (int)SendDlgItemMessageW(hwnd, 100, CB_GETCURSEL, 0, 0);
                cfg.provider = p == 1   ? L"deepl"
                               : p == 2 ? L"google-api"
                               : p == 3 ? L"libretranslate"
                                        : L"google-free";
                cfg.target = controlText(GetDlgItem(hwnd, 101));
                cfg.deeplKey = controlText(GetDlgItem(hwnd, 102));
                cfg.deeplFree = SendDlgItemMessageW(hwnd, 103, BM_GETCHECK, 0, 0) == BST_CHECKED;
                cfg.googleKey = controlText(GetDlgItem(hwnd, 104));
                cfg.libreUrl = controlText(GetDlgItem(hwnd, 105));
                cfg.proxy = controlText(GetDlgItem(hwnd, 106));
                app->ocrTasks.add([app, cfg] {
                    try {
                        auto r = std::make_unique<Result>();
                        r->type = 2;
                        if (cfg.provider == L"deepl") {
                            auto usage = queryDeepLUsage(cfg, &app->closing);
                            r->status = L"DeepL 连接成功 · 已用 " + std::to_wstring(usage.used) +
                                        L" / " + std::to_wstring(usage.limit) + L" 字符";
                        } else {
                            auto result = translateLines({L"Hello"}, cfg, &app->closing);
                            r->status = L"翻译连接成功 · 合成测试：" +
                                        (result.empty() ? L"" : result.front());
                        }
                        app->publish(r.release());
                    } catch (const std::exception &e) {
                        app->error(e);
                    }
                });
                app->status = L"正在使用合成文本测试连接，结果会显示在主面板底部";
                app->invalidate();
                MessageBoxW(hwnd, app->status.c_str(), L"DeskFlow", MB_OK);
                return 0;
            }
            if (LOWORD(wp) == 132) {
                Settings desired = app->config;
                auto get = [&](int id) { return controlText(GetDlgItem(hwnd, id)); };
                int p = (int)SendDlgItemMessageW(hwnd, 100, CB_GETCURSEL, 0, 0);
                desired.translation.provider = p == 1   ? L"deepl"
                                               : p == 2 ? L"google-api"
                                               : p == 3 ? L"libretranslate"
                                                        : L"google-free";
                desired.translation.target = get(101);
                desired.translation.deeplKey = get(102);
                desired.translation.deeplFree =
                    SendDlgItemMessageW(hwnd, 103, BM_GETCHECK, 0, 0) == BST_CHECKED;
                desired.translation.googleKey = get(104);
                desired.translation.libreUrl = get(105);
                desired.translation.proxy = get(106);
                desired.excludedApps = get(120);
                desired.autoStart =
                    SendDlgItemMessageW(hwnd, 121, BM_GETCHECK, 0, 0) == BST_CHECKED;
                desired.elevatedIndex =
                    SendDlgItemMessageW(hwnd, 123, BM_GETCHECK, 0, 0) == BST_CHECKED;
                desired.dark = SendDlgItemMessageW(hwnd, 122, BM_GETCHECK, 0, 0) == BST_CHECKED;
                auto maximumEntry = std::stoul(get(141)), quota = std::stoul(get(142));
                if (maximumEntry < 1 || maximumEntry > 128 || quota < 1 || quota > 1024)
                    throw std::runtime_error("单条上限应为 1–128 MiB，图片预算应为 1–1024 GiB");
                desired.maximumEntryMiB = maximumEntry;
                desired.imageQuotaGiB = quota;
                for (int i = 0; i < 3; i++) {
                    desired.hotkeys[i] = logicalHotkey(
                        (WORD)SendDlgItemMessageW(hwnd, 110 + i, HKM_GETHOTKEY, 0, 0));
                    if (!desired.hotkeys[i].key || !desired.hotkeys[i].modifiers)
                        throw std::runtime_error("快捷键需要至少一个修饰键和一个普通键");
                }
                if (!app->registerKeys(desired)) {
                    MessageBoxW(hwnd, app->status.c_str(), L"快捷键冲突", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                try {
                    saveSettings(app->data / L"settings.json", desired);
                } catch (...) {
                    app->registerKeys(app->config);
                    throw;
                }
                app->config = desired;
                if (app->history)
                    app->history->setLimits((size_t)desired.maximumEntryMiB * 1024 * 1024,
                                            (uint64_t)desired.imageQuotaGiB * 1024 * 1024 * 1024);
                app->updateAutoStart();
                app->status = L"设置已保存";
                DestroyWindow(hwnd);
                app->invalidate();
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            app->settingsWindow = nullptr;
            return 0;
        }
    } catch (const std::exception &e) {
        MessageBoxW(hwnd, userError(e).c_str(), L"设置未保存", MB_OK | MB_ICONWARNING);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
#ifndef DESKFLOW_NO_ENTRYPOINT
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool smoke = false, trayOnly = false;
    std::filesystem::path ocrVerifyImage, ocrVerifyReport;
    UINT lifetimeMs = 4000;
    auto dir = dataDirectory();
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"--smoke-test" || arg == L"--self-test")
            smoke = true;
        if (arg == L"--tray")
            trayOnly = true;
        if (arg == L"--idle-test" && i + 1 < argc) {
            smoke = true;
            lifetimeMs = std::clamp<unsigned>(std::stoul(argv[++i]), 5, 86400) * 1000;
        }
        if (arg == L"--data" && i + 1 < argc)
            dir = argv[++i];
        if (arg == L"--ocr-verify-image" && i + 1 < argc)
            ocrVerifyImage = argv[++i];
        if (arg == L"--ocr-verify-report" && i + 1 < argc)
            ocrVerifyReport = argv[++i];
    }
    LocalFree(argv);
    if (!ocrVerifyImage.empty() && !ocrVerifyReport.empty()) {
        nlohmann::json report;
        int code = 0;
        try {
            auto recognized = runOcr(executableDirectory() / L"DeskOCR.exe", ocrVerifyImage);
            report = {{"ok", true},
                      {"text", utf8(recognized.text)},
                      {"lines", recognized.lines.size()},
                      {"width", recognized.width},
                      {"height", recognized.height}};
        } catch (const std::exception &error) {
            report = {{"ok", false}, {"error", utf8(userError(error))}};
            code = 1;
        }
        std::ofstream output(ocrVerifyReport, std::ios::binary);
        output << report.dump(2);
        output.close();
        OleUninitialize();
        return code;
    }
    HANDLE single = CreateMutexW(
        nullptr, FALSE,
        (L"Local\\DeskFlow-" + wide(sha256(utf8(dir.wstring()).data(), utf8(dir.wstring()).size())))
            .c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        auto existing = FindWindowW(L"DeskFlowPanel", nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(single);
        OleUninitialize();
        return 0;
    }
    try {
        Application app(instance, dir, smoke, lifetimeMs);
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.hInstance = instance;
        wc.lpfnWndProc = Application::procedure;
        wc.lpszClassName = L"DeskFlowPanel";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        RegisterClassW(&wc);
        float initialScale = GetDpiForSystem() / 96.f;
        int width = (int)(1000 * initialScale), height = (int)(660 * initialScale);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        width = std::min(width, (int)(work.right - work.left - 40));
        height = std::min(height, (int)(work.bottom - work.top - 40));
        int x = work.left + (work.right - work.left - width) / 2,
            y = work.top + (work.bottom - work.top - height) / 2;
        auto hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"DeskFlow",
                                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y,
                                    width, height, nullptr, nullptr, instance, &app);
        if (!hwnd)
            throw std::runtime_error("主窗口创建失败");
        DWORD rounded = 2;
        DwmSetWindowAttribute(hwnd, 33, &rounded, sizeof(rounded));
        if (!trayOnly)
            app.show(0);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (app.settingsWindow && IsDialogMessageW(app.settingsWindow, &msg))
                continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, DoneMessage, DoneMessage, PM_REMOVE))
            delete (Result *)msg.lParam;
    } catch (const std::exception &e) {
        MessageBoxW(nullptr, userError(e).c_str(), L"DeskFlow 启动失败", MB_OK | MB_ICONERROR);
        CloseHandle(single);
        OleUninitialize();
        return 1;
    }
    CloseHandle(single);
    OleUninitialize();
    return 0;
}
#endif
