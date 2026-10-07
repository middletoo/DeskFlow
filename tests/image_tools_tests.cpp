#include "image_tools.hpp"
#include <algorithm>
#include <iostream>
#include <string_view>
#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <dlgs.h>
#pragma comment(lib, "dwmapi.lib")

namespace {
int failures{};
std::filesystem::path previewPath;
int docOffset{};
void check(bool ok, const char* label) { if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; } }
desk::PixelImage document(int width, int height, int startRow = 0) {
    desk::PixelImage image{width, height, std::vector<std::uint32_t>(static_cast<size_t>(width) * height)};
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const unsigned row = static_cast<unsigned>(y + startRow);
        unsigned value = row * 0x9e3779b9U + static_cast<unsigned>(x) * 0x85ebca6bU;
        value ^= value >> 16; value *= 0x7feb352dU; value ^= value >> 15;
        image.pixels[static_cast<size_t>(y) * width + x] = 0xff000000 | (value & 0xffffff);
    }
    return image;
}
void stitchTests() {
    auto first = document(96, 180);
    check(first.valid(), "valid synthetic image");
    desk::VerticalStitcher stitcher;
    check(stitcher.append(first).status == desk::StitchStatus::Started, "first frame starts the long image");
    check(stitcher.append(first).status == desk::StitchStatus::NoChange, "stationary frames are not duplicated");
    auto next = document(96, 180, 71);
    auto appended = stitcher.append(next);
    check(appended.status == desk::StitchStatus::Appended && appended.overlap == 109 && appended.addedRows == 71,
          "unique vertical overlap detects the exact displacement");
    auto expected = document(96, 251);
    check(stitcher.image().pixels == expected.pixels, "stitched pixels equal the complete source document");
    auto unrelated = document(96, 180, 500);
    auto before = stitcher.image().pixels;
    check(stitcher.append(unrelated).status == desk::StitchStatus::NoReliableOverlap && stitcher.image().pixels == before,
          "unrelated frames are rejected without changing the accepted image");
    check(stitcher.append(document(97, 180)).status == desk::StitchStatus::SizeMismatch, "capture dimensions cannot change mid-session");
    desk::VerticalStitcher limitedStitcher(96ULL * 220, 220);
    limitedStitcher.append(first);
    check(limitedStitcher.append(next).status == desk::StitchStatus::LimitReached && limitedStitcher.image().height == 180,
          "pixel and height budgets are enforced before allocation");
    desk::PixelImage repeated{96, 180, std::vector<std::uint32_t>(96 * 180)};
    for (int y = 0; y < 180; ++y) for (int x = 0; x < 96; ++x)
        repeated.pixels[y * 96 + x] = ((x / 5 + y / 8) % 2) ? 0xff112233 : 0xffeeddcc;
    auto shifted = repeated;
    for (int y = 0; y < 180; ++y) for (int x = 0; x < 96; ++x)
        shifted.pixels[y * 96 + x] = ((x / 5 + (y + 8) / 8) % 2) ? 0xff112233 : 0xffeeddcc;
    desk::VerticalStitcher ambiguous;
    ambiguous.append(repeated);
    check(ambiguous.append(shifted).status == desk::StitchStatus::NoReliableOverlap,
          "periodic content with multiple plausible overlaps is rejected");
    auto sticky = next;
    std::copy_n(first.pixels.begin(), 96 * 24, sticky.pixels.begin());
    desk::VerticalStitcher fixedHeader;
    fixedHeader.append(first);
    check(fixedHeader.append(sticky).status == desk::StitchStatus::NoReliableOverlap,
          "fixed page headers cannot be silently stitched at a wrong offset");
    desk::VerticalStitcher sparseTrap;
    auto highResolution = document(640, 600); sparseTrap.append(highResolution);
    auto sampledAlias = document(640, 600, 220);
    for (int y = 0; y < 380; ++y) for (int x = 1; x < 640; x += 2) sampledAlias.pixels[y * 640 + x] ^= 0x00ffffff;
    check(sparseTrap.append(sampledAlias).status == desk::StitchStatus::NoReliableOverlap,
          "changes between sample columns cannot fool the final overlap verification");
    check(stitcher.append({}).status == desk::StitchStatus::InvalidImage, "invalid frame is rejected");
    stitcher.reset(); check(stitcher.image().pixels.empty(), "reset releases the accepted image");
}
void bitmapTests() {
    auto image = document(63, 42);
    HBITMAP bitmap = desk::bitmapFromPixels(image);
    check(bitmap != nullptr, "synthetic bitmap conversion");
    desk::PixelImage decoded;
    check(desk::bitmapPixels(bitmap, decoded) && decoded.pixels == image.pixels,
          "bitmap round trip preserves top-down pixels");
    if (bitmap) DeleteObject(bitmap);
    check(!desk::bitmapPixels(nullptr, decoded), "null bitmap is rejected");
    const RECT negative = desk::physicalSelection({200, 240, 20, 40}, {-1920, -600});
    check(negative.left == -1900 && negative.top == -560 && negative.right == -1720 && negative.bottom == -360,
          "reversed selection converts correctly through a negative virtual desktop origin");
    const RECT overflow = desk::physicalSelection({1, 1, 200, 200}, {LONG_MAX, LONG_MAX});
    check(IsRectEmpty(&overflow), "physical coordinate overflow is rejected");
}
void memoryBudgetTests() {
    using namespace desk::image_tools_detail;
    const auto baseline = desk::imageToolReservedBytes();
    const auto dual4k = captureMemoryRequirement(7680, 2160);
    check(dual4k >= 5ULL * 7680 * 2160 * 4, "capture accounts for snapshot, dimmed, paint, output and export copy");
    auto capture = reserveMemory(dual4k);
    check(capture && desk::imageToolReservedBytes() == baseline + dual4k, "dual 4K remains supported with no existing pins");
    auto oversized = reserveMemory(captureMemoryRequirement(8192, 4096));
    check(captureMemoryRequirement(8192, 4096) > imageWorkingBytes, "full 32-MP capture is rejected even without pins");
    check(!oversized, "32-MP editor is rejected when its live surfaces exceed the aggregate allowance");
    capture.reset();
    auto pins = reserveMemory(128ULL * 1024 * 1024);
    auto contested = reserveMemory(dual4k);
    check(pins && !contested, "existing pinned-image bytes participate in capture admission");
    pins.reset();
    auto entireAllowance = reserveMemory(imageWorkingBytes);
    check(entireAllowance && !reserveMemory(1), "concurrent transient reservations cannot exceed 384 MiB");
    entireAllowance.reset();
    check(desk::imageToolReservedBytes() == baseline, "memory reservations release after their last owner");
}
void pinTests() {
    HWND pin = desk::pinImage(nullptr, desk::bitmapFromPixels(document(160, 100)));
    check(pin && IsWindow(pin), "pin entry creates an owned topmost window");
    if (!pin) return;
    check((GetWindowLongPtrW(pin, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "pin stays above ordinary windows");
    RECT before{}, after{}; GetWindowRect(pin, &before);
    SendMessageW(pin, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(100, 100));
    GetWindowRect(pin, &after);
    check(after.right - after.left >= before.right - before.left, "wheel increases the pin zoom");
    SendMessageW(pin, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, -WHEEL_DELTA), MAKELPARAM(100, 100));
    COLORREF key{}; BYTE alpha{}; DWORD flags{};
    check(GetLayeredWindowAttributes(pin, &key, &alpha, &flags) && alpha < 255, "Shift-wheel adjusts pin opacity");
    SendMessageW(pin, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!IsWindow(pin), "Escape releases the pin window and owned bitmap");
    HWND fromCallback{}; HBITMAP delivered{}; std::wstring action;
    fromCallback = desk::pinImage(nullptr, desk::bitmapFromPixels(document(120, 80)),
        [&](HBITMAP bitmap, const std::wstring& name) { delivered = bitmap; action = name; });
    if (fromCallback) SendMessageW(fromCallback, WM_KEYDOWN, 'O', 0);
    check(delivered && action == L"ocr" && IsWindow(fromCallback), "pin OCR sends an owned clone and keeps the original pin");
    if (delivered) DeleteObject(delivered);
    if (fromCallback) SendMessageW(fromCallback, WM_CLOSE, 0, 0);
    // Warm the native theme/layered-window caches on this newly activated
    // desktop before measuring sustained growth. These can initialize after
    // the first window's asynchronous nonclient paint.
    for (int i = 0; i < 8; ++i) {
        HWND item = desk::pinImage(nullptr, desk::bitmapFromPixels(document(64, 64)));
        if (item) SendMessageW(item, WM_CLOSE, 0, 0);
    }
    const DWORD baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto memoryBaseline = desk::imageToolReservedBytes();
    for (int i = 0; i < 8; ++i) {
        HBITMAP owned = desk::bitmapFromPixels(document(64, 64));
        HWND item = desk::pinImage(nullptr, owned);
        if (item) SendMessageW(item, WM_CLOSE, 0, 0);
        BITMAP dimensions{};
        check(!GetObjectW(owned, sizeof(dimensions), &dimensions), "closed pin deletes its owned bitmap");
        check(desk::imageToolReservedBytes() == memoryBaseline, "closed pin releases its image reservation");
    }
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= baseline + 1, "repeated pin closure does not leak GDI handles");
}
void key(HWND window, WPARAM value, bool control = false, bool shift = false) {
    BYTE previous[256]{}; GetKeyboardState(previous); BYTE changed[256]{};
    std::copy_n(previous, 256, changed); changed[VK_CONTROL] = control ? 0x80 : 0; changed[VK_SHIFT] = shift ? 0x80 : 0;
    SetKeyboardState(changed); SendMessageW(window, WM_KEYDOWN, value, 0); SetKeyboardState(previous);
}
struct ModalShutdownFixture {
    HWND target{}, dialog{}, owner{};
    std::filesystem::path output;
    bool triggered{}, destroyOwner{};
    unsigned ticks{};
};
ModalShutdownFixture* modalFixture{};
LRESULT CALLBACK modalCbt(int code, WPARAM wparam, LPARAM lparam) {
    if (modalFixture && code == HCBT_ACTIVATE) {
        HWND candidate = reinterpret_cast<HWND>(wparam); wchar_t type[32]{}; GetClassNameW(candidate, type, 32);
        if (!wcscmp(type, L"#32770") && GetWindow(candidate, GW_OWNER) == modalFixture->target) modalFixture->dialog = candidate;
    }
    if (modalFixture && code == HCBT_DESTROYWND && reinterpret_cast<HWND>(wparam) == modalFixture->dialog && !modalFixture->triggered) {
        modalFixture->triggered = true;
        // The save dialog has accepted the file and is being destroyed inside
        // GetSaveFileNameW. Destroy the editor while its original save dispatch
        // is still suspended on the stack.
        if (modalFixture->destroyOwner) DestroyWindow(modalFixture->owner);
        else desk::shutdownImageTools();
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
VOID CALLBACK modalTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (!modalFixture || modalFixture->triggered) return;
    if (++modalFixture->ticks > 50) {
        if (IsWindow(modalFixture->dialog)) PostMessageW(modalFixture->dialog, WM_COMMAND, IDCANCEL, 0);
        return;
    }
    if (IsWindow(modalFixture->dialog)) {
        SendMessageW(modalFixture->dialog, CDM_SETCONTROLTEXT, edt1, reinterpret_cast<LPARAM>(modalFixture->output.c_str()));
        PostMessageW(modalFixture->dialog, WM_COMMAND, IDOK, 0);
    }
}
void modalShutdownTests() {
    const auto folder = std::filesystem::temp_directory_path() / (L"DeskModalSave-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    for (int variant = 0; variant < 3; ++variant) {
        const auto reservedBefore = desk::imageToolReservedBytes();
        ModalShutdownFixture fixture;
        fixture.output = folder / (L"nested-" + std::to_wstring(variant) + L".png");
        fixture.destroyOwner = variant == 2;
        fixture.owner = CreateWindowW(L"STATIC", L"Synthetic host owner", WS_POPUP, 20, 20, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (variant == 0) {
            desk::beginCapture(fixture.owner, {});
            fixture.target = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
            if (fixture.target) {
                SendMessageW(fixture.target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(80, 80));
                SendMessageW(fixture.target, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(280, 210));
                SendMessageW(fixture.target, WM_LBUTTONUP, 0, MAKELPARAM(280, 210));
            }
        } else fixture.target = desk::pinImage(fixture.owner, desk::bitmapFromPixels(document(80, 80)));
        check(fixture.target != nullptr, "create nested-save fixture");
        modalFixture = &fixture;
        HHOOK hook = SetWindowsHookExW(WH_CBT, modalCbt, nullptr, GetCurrentThreadId());
        const UINT_PTR timer = SetTimer(nullptr, 0, 100, modalTimer);
        if (fixture.target) key(fixture.target, 'S', true);
        KillTimer(nullptr, timer); UnhookWindowsHookEx(hook); modalFixture = nullptr;
        check(fixture.triggered, "destroy image window from the accepted save dialog's nested loop");
        check(!IsWindow(fixture.target), "nested shutdown destroys the window before save resumes");
        check(!std::filesystem::exists(fixture.output), "destroyed save owner cannot begin a new file write");
        if (IsWindow(fixture.owner)) DestroyWindow(fixture.owner);
        desk::shutdownImageTools();
        check(desk::imageToolReservedBytes() == reservedBefore, "nested save shutdown releases all image reservations");
    }
    std::error_code ignored;
    for (int variant = 0; variant < 3; ++variant) std::filesystem::remove(folder / (L"nested-" + std::to_wstring(variant) + L".png"), ignored);
    std::filesystem::remove(folder, ignored);
}
void region(HWND window, POINT start, POINT finish) {
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start.x, start.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(finish.x, finish.y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(finish.x, finish.y));
}
LRESULT CALLBACK documentProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC target = BeginPaint(window, &ps); RECT client{}; GetClientRect(window, &client);
        HBITMAP bitmap = desk::bitmapFromPixels(document(client.right, client.bottom, docOffset));
        HDC source = CreateCompatibleDC(target); auto previous = SelectObject(source, bitmap);
        BitBlt(target, 0, 0, client.right, client.bottom, source, 0, 0, SRCCOPY);
        SelectObject(source, previous); DeleteDC(source); DeleteObject(bitmap); EndPaint(window, &ps); return 0;
    }
    if (message == WM_MOUSEWHEEL) { docOffset += 31; InvalidateRect(window, nullptr, FALSE); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
void editorTests() {
    HBITMAP output{}; std::wstring action;
    desk::beginCapture(nullptr, [&](HBITMAP bitmap, const std::wstring& name) { output = bitmap; action = name; });
    HWND overlay = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    check(overlay != nullptr, "private screenshot editor opens");
    if (!overlay) return;
    region(overlay, {80, 80}, {320, 240});
    key(overlay, '6'); region(overlay, {105, 100}, {190, 160}); // ellipse
    key(overlay, '7'); region(overlay, {240, 130}, {240, 130}); // number
    key(overlay, 'Z', true); key(overlay, 'Y', true); // number is restored
    key(overlay, 'Z', true); key(overlay, 'Z', true, true); // Shift+Ctrl+Z is also redo
    if (!previewPath.empty()) {
        UpdateWindow(overlay); DwmFlush();
        RECT visible{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        HBITMAP preview = desk::captureRegion(visible);
        check(preview && desk::saveBitmapPng(preview, previewPath), "save the synthetic private-desktop editor preview");
        if (preview) DeleteObject(preview);
    }
    key(overlay, 'O');
    check(output && action == L"ocr" && !desk::captureActive(), "new annotations complete through the OCR entry");
    if (output) {
        desk::PixelImage pixels; desk::bitmapPixels(output, pixels);
        auto red = [](unsigned p) { return ((p >> 16) & 255) > 180 && ((p >> 8) & 255) < 160; };
        unsigned ellipse{}, number{};
        if (pixels.width == 240 && pixels.height == 160) {
            for (int y = 15; y < 90; ++y) for (int x = 20; x < 120; ++x) ellipse += red(pixels.pixels[y * pixels.width + x]);
            for (int y = 32; y < 70; ++y) for (int x = 140; x < 180; ++x) number += red(pixels.pixels[y * pixels.width + x]);
        }
        check(ellipse > 100, "ellipse annotation is present in the physical crop");
        check(number > 100, "number annotation survives both redo shortcuts");
        DeleteObject(output);
    }
    if (desk::captureActive()) SendMessageW(overlay, WM_CLOSE, 0, 0);
    RECT received{}; std::wstring recording;
    desk::beginCapture(nullptr, {}, [&](RECT bounds, const std::wstring& format) { received = bounds; recording = format; check(!desk::captureActive(), "recording starts after the overlay closes"); });
    overlay = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    if (overlay) { region(overlay, {90, 95}, {290, 215}); key(overlay, 'G'); }
    check(recording == L"gif" && received.right - received.left == 200 && received.bottom - received.top == 120,
          "GIF entry delivers the selected physical region");
    if (desk::captureActive()) SendMessageW(overlay, WM_CLOSE, 0, 0);
    desk::beginCapture(nullptr, {});
    overlay = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    if (overlay) { region(overlay, {100, 110}, {270, 210}); key(overlay, 'P'); }
    HWND pin = FindWindowW(L"DeskEfficiencyPinnedImage", nullptr);
    check(pin && !desk::captureActive(), "P closes the editor and opens an owned pin");
    if (pin) SendMessageW(pin, WM_CLOSE, 0, 0);
    if (desk::captureActive()) SendMessageW(overlay, WM_CLOSE, 0, 0);
    desk::beginCapture(nullptr, {});
    overlay = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    if (overlay) { region(overlay, {100, 110}, {270, 210}); key(overlay, 'L'); }
    HWND scroll = FindWindowW(L"DeskEfficiencyScrollingCapture", nullptr);
    check(scroll && desk::scrollingCaptureActive(), "L creates the bounded scrolling session");
    if (scroll) { SendMessageW(scroll, WM_KEYDOWN, VK_ESCAPE, 0); check(!desk::scrollingCaptureActive(), "scrolling cancel destroys the session"); }
    if (FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr)) SendMessageW(overlay, WM_CLOSE, 0, 0);
    WNDCLASSEXW type{sizeof(type)}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = documentProc;
    type.lpszClassName = L"DeskLongCaptureSyntheticDocument"; RegisterClassExW(&type);
    HWND seed = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, type.lpszClassName, L"Synthetic scroll document", WS_POPUP,
        100, 110, 170, 100, nullptr, nullptr, type.hInstance, nullptr);
    ShowWindow(seed, SW_SHOW); UpdateWindow(seed); DwmFlush();
    recording.clear(); received = {};
    desk::beginCapture(nullptr, {}, [&](RECT bounds, const std::wstring& format) { received = bounds; recording = format; });
    overlay = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    if (overlay) {
        SendMessageW(overlay, WM_MOUSEMOVE, 0, MAKELPARAM(140, 145));
        region(overlay, {140, 145}, {140, 145}); key(overlay, 'M');
    }
    check(recording == L"mp4" && received.left == 100 && received.top == 110 && received.right == 270 && received.bottom == 210,
          "hover auto-snap selects the visible synthetic window and delivers physical MP4 bounds");
    if (FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr)) SendMessageW(overlay, WM_CLOSE, 0, 0);
    output = nullptr; action.clear();
    HWND completedScroll = desk::beginScrollingCapture(nullptr, {100, 110, 270, 210},
        [&](HBITMAP bitmap, const std::wstring& name) { output = bitmap; action = name; });
    check(completedScroll != nullptr, "synthetic scrolling session opens");
    if (completedScroll) {
        SendMessageW(seed, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0); UpdateWindow(seed); DwmFlush();
        SendMessageW(completedScroll, WM_TIMER, 1, 0); SendMessageW(completedScroll, WM_KEYDOWN, VK_RETURN, 0);
        HWND completedPin = FindWindowW(L"DeskEfficiencyPinnedImage", nullptr);
        check(completedPin && !desk::scrollingCaptureActive(), "long capture finish transfers to a pin and releases the session");
        if (completedPin) { key(completedPin, 'O'); SendMessageW(completedPin, WM_CLOSE, 0, 0); }
        desk::PixelImage stitched;
        check(output && desk::bitmapPixels(output, stitched) && stitched.width == 170 && stitched.height == 131 &&
            stitched.pixels == document(170, 131).pixels, "real private-desktop scrolling samples produce the exact source long image");
        if (output) DeleteObject(output);
    }
    if (seed) DestroyWindow(seed);
    HWND shutdownPin = desk::pinImage(nullptr, desk::bitmapFromPixels(document(80, 80)));
    desk::beginCapture(nullptr, {});
    HWND shutdownOverlay = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    desk::shutdownImageTools();
    check(!IsWindow(shutdownPin) && !IsWindow(shutdownOverlay) && !desk::captureActive(),
          "shutdown releases every image window before the host callback owner is destroyed");
}
void privateUi() {
    // These UI cases never read/write the clipboard. A synthetic desktop in
    // WinSta0 is required for real screen capture; a noninteractive station
    // cannot supply a display DC and some sanitizer runtimes already use USER32.
    HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId());
    const auto name = L"DeskImageEditorTests-" + std::to_wstring(GetCurrentProcessId());
    HDESK editorDesktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const bool attached = editorDesktop && SetThreadDesktop(editorDesktop);
    check(attached, "attach private interactive-station editor desktop");
    if (attached) {
        const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const bool activated = SwitchDesktop(editorDesktop) != FALSE;
        check(activated, "activate the synthetic private test desktop");
        if (activated) {
            HBITMAP screen = desk::captureRegion({10, 10, 40, 40});
            check(screen != nullptr, "private desktop supports screen capture");
            if (screen) { DeleteObject(screen); if (previewPath == L"modal") modalShutdownTests(); else { pinTests(); editorTests(); modalShutdownTests(); } }
            SwitchDesktop(originalDesktop);
        }
        SetThreadDesktop(originalDesktop); CloseDesktop(editorDesktop);
        if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    } else if (editorDesktop) CloseDesktop(editorDesktop);
}
}
int main(int argc, char** argv) {
    if (argc > 2) previewPath = std::filesystem::path(argv[2]);
    if (argc > 1 && std::string_view(argv[1]) == "--ui-private") privateUi();
    else { stitchTests(); bitmapTests(); memoryBudgetTests(); }
    std::cout << (failures ? "FAILED: " : "PASS: ") << failures << " image-tool failures\n";
    return failures ? 1 : 0;
}
