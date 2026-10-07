#include "capture.hpp"
#include "image_tools.hpp"
#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <dlgs.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
int failures{};
void check(bool success, const char* description) {
    if (!success) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
using Handler = std::function<HBITMAP(HBITMAP, std::atomic_bool&)>;
template<class Translation> constexpr bool supportsInlineTranslation = requires(HWND owner, desk::CaptureCallback callback, Translation translation) {
    desk::beginCapture(owner, callback, desk::RegionCallback{}, translation);
};
template<class Translation> void begin(HWND owner, desk::CaptureCallback callback, Translation translation) {
    if constexpr (supportsInlineTranslation<Translation>) desk::beginCapture(owner, std::move(callback), {}, std::move(translation));
    else desk::beginCapture(owner, std::move(callback));
}
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
template<class Condition> bool until(Condition condition, int milliseconds = 5000) {
    const auto deadline = GetTickCount64() + milliseconds;
    do { pump(); if (condition()) return true; Sleep(5); } while (GetTickCount64() < deadline);
    return condition();
}
void key(HWND window, WPARAM value, bool control = false, bool shift = false) {
    BYTE previous[256]{}, changed[256]{}; GetKeyboardState(previous); std::copy_n(previous, 256, changed);
    changed[VK_CONTROL] = control ? 0x80 : 0; changed[VK_SHIFT] = shift ? 0x80 : 0;
    SetKeyboardState(changed); SendMessageW(window, WM_KEYDOWN, value, 0); SetKeyboardState(previous);
}
void drag(HWND window, POINT start, POINT finish) {
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start.x, start.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(finish.x, finish.y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(finish.x, finish.y));
}
DWORD pixel(HWND window, int x, int y) {
    UpdateWindow(window); HDC dc = GetDC(window); const COLORREF color = GetPixel(dc, x, y); ReleaseDC(window, dc);
    return GetRValue(color) << 16 | GetGValue(color) << 8 | GetBValue(color);
}
HBITMAP solid(int width, int height, DWORD color) {
    desk::PixelImage pixels; pixels.width = width; pixels.height = height; pixels.pixels.assign(static_cast<size_t>(width) * height, color);
    return desk::bitmapFromPixels(pixels);
}
LRESULT CALLBACK seedProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT update{}; HDC dc = BeginPaint(window, &update); RECT bounds{}; GetClientRect(window, &bounds);
        HBRUSH paper = CreateSolidBrush(RGB(242, 242, 242)); FillRect(dc, &bounds, paper); DeleteObject(paper);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(44, 44, 44)); TextOutW(dc, 35, 35, L"Synthetic translation sample", 28);
        EndPaint(window, &update); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
const RECT selection{120, 120, 420, 290};
constexpr DWORD translatedColor = 0x002e85d4;
HWND open(desk::CaptureCallback callback, Handler handler, HWND owner = nullptr) {
    begin(owner, std::move(callback), std::move(handler));
    HWND window = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    check(window != nullptr, "capture editor opens on the private desktop");
    if (window) drag(window, {selection.left, selection.top}, {selection.right, selection.bottom});
    return window;
}
Handler immediate(std::atomic_bool& completed) {
    return [&](HBITMAP source, std::atomic_bool&) {
        BITMAP size{}; GetObjectW(source, sizeof(size), &size);
        HBITMAP result = solid(size.bmWidth, size.bmHeight, 0xff000000 | translatedColor); completed = true; return result;
    };
}
void preview(HWND window, const std::filesystem::path& path) {
    if (path.empty()) return;
    UpdateWindow(window); DwmFlush();
    RECT bounds{}; GetClientRect(window, &bounds);
    HBITMAP image = desk::captureRegion(bounds);
    check(image && desk::saveBitmapPng(image, path), "write synthetic translated editor preview");
    if (image) DeleteObject(image);
}
void translatedEditor(const std::filesystem::path& previewPath) {
    std::atomic_bool completed{}; int calls{}; HBITMAP exported{};
    HWND owner = CreateWindowW(L"STATIC", L"Synthetic host", WS_POPUP, 600, 100, 60, 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(owner, SW_SHOW);
    HWND window = open([&](HBITMAP image, const std::wstring&) { ++calls; exported = image; }, immediate(completed), owner);
    if (!window) { DestroyWindow(owner); return; }
    const DWORD original = pixel(window, 360, 250);
    key(window, '1'); drag(window, {145, 145}, {235, 210});
    const DWORD annotationBefore = pixel(window, 145, 160);
    key(window, 'T');
    check(until([&] { return completed.load() && IsWindow(window) && pixel(window, 360, 250) == translatedColor; }), "T translates pixels inside the current editor");
    check(calls == 0 && !IsWindowVisible(owner), "translation neither calls the host nor reopens its panel");
    check(pixel(window, 145, 160) == annotationBefore, "existing editable annotation stays above translated pixels");
    SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
    check(pixel(window, 360, 250) == original, "holding Space previews the preserved original");
    SendMessageW(window, WM_KEYUP, VK_SPACE, 0);
    check(pixel(window, 360, 250) == translatedColor, "releasing Space restores the translated image");
    key(window, 'Z', true);
    check(pixel(window, 145, 160) == translatedColor, "Ctrl+Z still removes an annotation without a visible undo button");
    key(window, 'Z', true, true);
    check(pixel(window, 145, 160) == annotationBefore, "Ctrl+Shift+Z restores the editable annotation");
    key(window, 'Z', true); key(window, 'Y', true);
    check(pixel(window, 145, 160) == annotationBefore, "Ctrl+Y still restores an annotation");
    key(window, 'V'); preview(window, previewPath);
    key(window, 'O');
    check(calls == 1 && exported && !IsWindow(window), "explicit OCR exports the translated selection after translation stayed inline");
    desk::PixelImage pixels;
    check(exported && desk::bitmapPixels(exported, pixels) && pixels.width == 300 && pixels.height == 170 && (pixels.pixels[130 * 300 + 240] & 0xffffff) == translatedColor,
          "export contains translated pixels at the crop coordinates");
    if (exported) DeleteObject(exported); DestroyWindow(owner);
}
void failureEditor() {
    int calls{}; std::atomic_bool completed{};
    HWND window = open([&](HBITMAP image, const std::wstring&) { ++calls; DeleteObject(image); }, [&](HBITMAP, std::atomic_bool&) -> HBITMAP {
        completed = true; throw std::runtime_error("未识别到可翻译文字；请扩大选区或检查 OCR 语言包。");
    });
    if (!window) return;
    const DWORD original = pixel(window, 360, 250); key(window, 'T');
    check(until([&] { return completed.load(); }), "failing handler runs asynchronously");
    check(IsWindow(window) && desk::captureActive() && calls == 0, "translation failure leaves the screenshot editor open");
    check(until([&] { key(window, '1'); drag(window, {145, 145}, {235, 210}); return pixel(window, 145, 160) != original; }), "editor remains usable after translation failure");
    SendMessageW(window, WM_CLOSE, 0, 0);
}
void cancelledEditor(bool verifyHandles = true) {
    pump();GdiFlush(); const auto budgetBefore = desk::imageToolReservedBytes(); const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    std::atomic_bool entered{}, cancelled{}, finished{}, release{}; HBITMAP borrowed{}; HANDLE worker{};
    HWND window = open({}, [&](HBITMAP source, std::atomic_bool& cancel) {
        worker = OpenThread(SYNCHRONIZE, FALSE, GetCurrentThreadId());
        borrowed = source; entered = true;
        while (!cancel.load() && !release.load()) Sleep(2);
        cancelled = cancel.load();
        BITMAP size{}; GetObjectW(source, sizeof(size), &size);
        HBITMAP result = solid(size.bmWidth, size.bmHeight, 0xff000000 | translatedColor); finished = true; return result;
    });
    if (!window) return;
    key(window, 'T'); check(until([&] { return entered.load(); }), "translation worker owns a source crop before cancellation");
    const auto start = GetTickCount64(); SendMessageW(window, WM_CLOSE, 0, 0);
    check(GetTickCount64() - start < 250 && !desk::captureActive(), "closing the editor cancels immediately without waiting for the worker");
    release = true;
    check(until([&] { return finished.load() && desk::imageToolReservedBytes() == budgetBefore; }), "cancelled worker releases source result and image budget");
    // The reservation may reach zero before the detached thread drains its
    // GDI batch and COM teardown. Observe that thread's actual termination.
    check(worker && WaitForSingleObject(worker, 1500) == WAIT_OBJECT_0, "cancelled worker terminates promptly");
    if (worker) CloseHandle(worker);
    check(cancelled.load(), "closing exposes cooperative cancellation to OCR and network work");
    BITMAP size{}; check(!GetObjectW(borrowed, sizeof(size), &size), "cancelled worker deletes its retained source bitmap");
    GdiFlush();const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    if (verifyHandles && gdiAfter > gdiBefore + 2) std::cerr << "GDI cancellation baseline " << gdiBefore << " after " << gdiAfter << '\n';
    if(verifyHandles)check(gdiAfter <= gdiBefore + 2, "repeated cancelled translation does not leak GDI objects");
}
void staleCompletion() {
    const auto budgetBefore = desk::imageToolReservedBytes();
    std::atomic_bool entered{}, release{}, oldFinished{}, newFinished{};
    HWND old = open({}, [&](HBITMAP source, std::atomic_bool&) {
        entered = true; while (!release.load()) Sleep(2);
        BITMAP size{}; GetObjectW(source, sizeof(size), &size);
        HBITMAP image = solid(size.bmWidth, size.bmHeight, 0xffee3377); oldFinished = true; return image;
    });
    if (!old) return;
    key(old, 'T'); check(until([&] { return entered.load(); }), "old worker begins before stale-completion test");
    SendMessageW(old, WM_CLOSE, 0, 0);
    HWND current = open({}, immediate(newFinished));
    if (current) {
        key(current, 'T'); check(until([&] { return newFinished.load() && pixel(current, 360, 250) == translatedColor; }), "new session completes while cancelled worker remains alive");
        release = true; check(until([&] { return oldFinished.load(); }), "cancelled old handler can finish after a new session");
        // A forged stale ID exercises the receiving side even if the platform
        // did not recycle the same HWND for these two particular sessions.
        SendMessageW(current, WM_APP + 0x523, 1, 0); pump();
        check(pixel(current, 360, 250) == translatedColor, "stale completion cannot replace a later session's translated pixels");
        SendMessageW(current, WM_CLOSE, 0, 0);
    } else release = true;
    check(until([&] { return desk::imageToolReservedBytes() == budgetBefore; }), "both stale and current sessions release their reservations");
}
void pinExport() {
    std::atomic_bool completed{}; HBITMAP exported{}; int calls{};
    HWND window = open([&](HBITMAP image, const std::wstring&) { exported = image; ++calls; }, immediate(completed));
    if (!window) return;
    key(window, 'T'); check(until([&] { return completed.load() && pixel(window, 360, 250) == translatedColor; }), "pin case completes inline translation");
    key(window, 'P'); HWND pin = FindWindowW(L"DeskEfficiencyPinnedImage", nullptr);
    check(pin && !IsWindow(window) && calls == 0, "P pins the translated result without calling the host");
    if (pin) {
        key(pin, 'O'); desk::PixelImage pixels;
        check(exported && desk::bitmapPixels(exported, pixels) && pixels.width == 300 && pixels.height == 170 &&
            (pixels.pixels[130 * 300 + 240] & 0xffffff) == translatedColor, "pinned result retains translated pixels for later export");
        SendMessageW(pin, WM_CLOSE, 0, 0);
    }
    if (exported) DeleteObject(exported);
}
struct SaveFixture { HWND owner{}, dialog{}; std::filesystem::path path; unsigned ticks{}; bool submitted{}; };
SaveFixture* saveFixture{};
LRESULT CALLBACK saveHook(int code, WPARAM wparam, LPARAM lparam) {
    if (saveFixture && code == HCBT_ACTIVATE) {
        HWND candidate = reinterpret_cast<HWND>(wparam); wchar_t type[32]{}; GetClassNameW(candidate, type, 32);
        if (!wcscmp(type, L"#32770") && GetWindow(candidate, GW_OWNER) == saveFixture->owner) saveFixture->dialog = candidate;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
VOID CALLBACK saveTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (!saveFixture) return;
    if (++saveFixture->ticks > 60) {
        EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM) -> BOOL {
            wchar_t type[32]{}; GetClassNameW(candidate, type, 32);
            if (!wcscmp(type, L"#32770")) { PostMessageW(candidate, WM_COMMAND, IDCANCEL, 0); PostMessageW(candidate, WM_CLOSE, 0, 0); }
            return TRUE;
        }, 0);
        return;
    }
    if (IsWindow(saveFixture->dialog) && !saveFixture->submitted) {
        HWND filename{};
        EnumChildWindows(saveFixture->dialog, [](HWND child, LPARAM data) -> BOOL {
            wchar_t type[64]{}; GetClassNameW(child, type, 64);
            if (!wcscmp(type, L"Edit") && (GetDlgCtrlID(child) == 1001 || GetDlgCtrlID(child) == edt1)) {
                *reinterpret_cast<HWND*>(data) = child; return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&filename));
        if (filename) {
            SetWindowTextW(filename, saveFixture->path.c_str());
            saveFixture->submitted = true; PostMessageW(saveFixture->dialog, WM_COMMAND, IDOK, 0);
        }
    }
}
void saveExport() {
    std::atomic_bool completed{}; int calls{};
    HWND window = open([&](HBITMAP image, const std::wstring&) { ++calls; DeleteObject(image); }, immediate(completed));
    if (!window) return;
    key(window, 'T'); check(until([&] { return completed.load() && pixel(window, 360, 250) == translatedColor; }), "save case completes inline translation");
    SaveFixture fixture; fixture.owner = window;
    fixture.path = std::filesystem::temp_directory_path() / (L"DeskInlineTranslationSave-" + std::to_wstring(GetCurrentProcessId()) + L".png");
    std::error_code ignored; std::filesystem::remove(fixture.path, ignored);
    saveFixture = &fixture; HHOOK hook = SetWindowsHookExW(WH_CBT, saveHook, nullptr, GetCurrentThreadId());
    UINT_PTR timer = SetTimer(nullptr, 0, 50, saveTimer); key(window, 'S', true);
    KillTimer(nullptr, timer); UnhookWindowsHookEx(hook); saveFixture = nullptr;
    check(until([&] { return !IsWindow(window); }), "native Save finishes and closes translated editor");
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromFilename(fixture.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    UINT width{}, height{}; if (SUCCEEDED(result)) result = frame->GetSize(&width, &height);
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    DWORD sample{}; WICRect crop{240, 130, 1, 1}; if (SUCCEEDED(result)) result = converter->CopyPixels(&crop, 4, 4, reinterpret_cast<BYTE*>(&sample));
    check(SUCCEEDED(result) && width == 300 && height == 170 && (sample & 0xffffff) == translatedColor && calls == 0,
          "native Save PNG contains translated pixels without invoking the host");
    if (FAILED(result) || width != 300 || height != 170 || calls) std::cerr << "Synthetic PNG result: " << std::hex << result << std::dec << " dimensions " << width << 'x' << height << " callbacks " << calls << '\n';
    converter.Reset(); frame.Reset(); decoder.Reset(); factory.Reset();
    std::filesystem::remove(fixture.path, ignored); if (IsWindow(window)) SendMessageW(window, WM_CLOSE, 0, 0);
}
void privateDesktop(const std::filesystem::path& previewPath) {
    HDESK previous = GetThreadDesktop(GetCurrentThreadId());
    const auto name = L"DeskInlineTranslationTests-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const bool attached = desktop && SetThreadDesktop(desktop);
    check(attached, "attach synthetic private desktop");
    if (!attached) { if (desktop) CloseDesktop(desktop); return; }
    const bool activated = SwitchDesktop(desktop) != FALSE; check(activated, "activate synthetic private desktop");
    if (activated) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        WNDCLASSEXW type{sizeof(type)}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = seedProc; type.lpszClassName = L"DeskInlineTranslationSeed"; RegisterClassExW(&type);
        HWND seed = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, type.lpszClassName, L"Synthetic source", WS_POPUP, 80, 80, 450, 300, nullptr, nullptr, type.hInstance, nullptr);
        ShowWindow(seed, SW_SHOW); UpdateWindow(seed); DwmFlush();
        translatedEditor(previewPath); failureEditor();
        // One real cancellation cycle initializes deferred system/GDI+ caches;
        // the second checks growth after those one-time resources are present.
        cancelledEditor(false);cancelledEditor(); staleCompletion(); pinExport(); saveExport(); DestroyWindow(seed);
        SwitchDesktop(previous);
        if (SUCCEEDED(com)) CoUninitialize();
    }
    SetThreadDesktop(previous); CloseDesktop(desktop);
}
void isolatedCopy() {
    // Each window station has its own clipboard. Never exercise Enter on the
    // visual private desktop in WinSta0, which shares the user's clipboard.
    HWINSTA previousStation = GetProcessWindowStation(); HDESK previousDesktop = GetThreadDesktop(GetCurrentThreadId());
    HWINSTA station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    check(station != nullptr, "create independent clipboard window station"); if (!station) return;
    if (!SetProcessWindowStation(station)) { check(false, "attach independent clipboard station"); CloseWindowStation(station); return; }
    HDESK desktop = CreateDesktopW(L"DeskInlineCopyTests", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const bool attached = desktop && SetThreadDesktop(desktop); check(attached, "attach independent clipboard desktop");
    if (attached) {
        HBITMAP displayProbe = desk::captureRegion({20, 20, 60, 60});
        if (!displayProbe) std::cout << "SKIP: noninteractive window station has no screen DC; direct Enter export requires an isolated display\n";
        else {
            DeleteObject(displayProbe); std::atomic_bool completed{}; int calls{};
            HWND window = open([&](HBITMAP bitmap, const std::wstring&) { ++calls; DeleteObject(bitmap); }, immediate(completed));
            if (window) {
                key(window, 'T');
                check(until([&] { if (completed.load()) key(window, VK_RETURN); return !IsWindow(window); }), "Enter exports inline translation in an isolated clipboard station");
                if (OpenClipboard(nullptr)) {
                    HGLOBAL data = GetClipboardData(CF_DIB); const auto* header = data ? static_cast<const BITMAPINFOHEADER*>(GlobalLock(data)) : nullptr;
                    const DWORD* pixels = header ? reinterpret_cast<const DWORD*>(header + 1) : nullptr;
                    check(header && header->biWidth == 300 && header->biHeight == 170 && pixels && (pixels[39 * 300 + 240] & 0xffffff) == translatedColor && calls == 0,
                          "Enter clipboard DIB contains translated pixels and correct crop dimensions");
                    if (header) GlobalUnlock(data); EmptyClipboard(); CloseClipboard();
                } else check(false, "read independent clipboard export");
                if (IsWindow(window)) SendMessageW(window, WM_CLOSE, 0, 0);
            }
        }
    }
    check(SetProcessWindowStation(previousStation) != FALSE, "restore original window station after copy test");
    check(SetThreadDesktop(previousDesktop) != FALSE, "restore original thread desktop after copy test");
    if (desktop) CloseDesktop(desktop); CloseWindowStation(station);
}
}
int wmain(int argc, wchar_t** argv) {
    check(supportsInlineTranslation<Handler>, "capture API accepts an injected inline translation handler");
    if (argc > 1 && std::wstring_view(argv[1]) == L"--api-contract") return failures ? 1 : 0;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--isolated-copy") {
        isolatedCopy(); std::cout << (failures ? "FAILED: " : "PASS: ") << failures << " isolated copy failures\n"; return failures ? 1 : 0;
    }
    const auto dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::filesystem::path previewPath = argc > 2 && std::wstring_view(argv[1]) == L"--preview" ? argv[2] : L"";
    privateDesktop(previewPath);
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    std::cout << (failures ? "FAILED: " : "PASS: ") << failures << " inline translation failures\n"; return failures ? 1 : 0;
}
