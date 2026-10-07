#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <string_view>
#include <dwmapi.h>
#include "capture.hpp"
#pragma comment(lib, "dwmapi.lib")

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
HBITMAP synthetic() {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 3;
    info.bmiHeader.biHeight = -2;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits{};
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap) {
        auto* pixels = static_cast<DWORD*>(bits);
        pixels[0] = 0x00ff0000; pixels[1] = 0x0000ff00; pixels[2] = 0x000000ff;
        pixels[3] = 0x00112233; pixels[4] = 0x00445566; pixels[5] = 0x00778899;
    }
    return bitmap;
}
void regionTests() {
    check(desk::captureRegion({0, 0, 0, 1}) == nullptr, "empty capture is rejected");
    check(desk::captureRegion({LONG_MIN, LONG_MIN, LONG_MAX, LONG_MAX}) == nullptr,
          "overflow / unbounded capture is rejected");
    RECT region{GetSystemMetrics(SM_XVIRTUALSCREEN) + 12, GetSystemMetrics(SM_YVIRTUALSCREEN) + 12,
                GetSystemMetrics(SM_XVIRTUALSCREEN) + 32, GetSystemMetrics(SM_YVIRTUALSCREEN) + 25};
    HBITMAP image = desk::captureRegion({region.right, region.bottom, region.left, region.top});
    BITMAP dimensions{};
    check(image && GetObjectW(image, sizeof(dimensions), &dimensions), "reversed physical rectangle captures");
    if (image) {
        check(dimensions.bmWidth == 20 && dimensions.bmHeight == 13, "normalized capture preserves physical dimensions");
        DeleteObject(image);
    }
}
void pngTests() {
    HBITMAP bitmap = synthetic();
    check(bitmap != nullptr, "synthetic bitmap allocation");
    const auto filename = std::filesystem::temp_directory_path() /
        (L"desk-capture-test-" + std::to_wstring(GetCurrentProcessId()) + L".png");
    check(!desk::saveBitmapPng(nullptr, filename), "null export is rejected");
    check(!desk::saveBitmapPng(bitmap, filename / L"missing.png"), "invalid output path reports failure");
    check(desk::saveBitmapPng(bitmap, filename), "PNG export succeeds");
    const auto originalSize = std::filesystem::file_size(filename);
    check(!desk::saveBitmapPng(nullptr, filename) && std::filesystem::file_size(filename) == originalSize,
          "failed save preserves an existing image");
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    auto result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromFilename(filename.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    UINT width{}, height{};
    if (SUCCEEDED(result)) result = frame->GetSize(&width, &height);
    check(SUCCEEDED(result) && width == 3 && height == 2, "PNG preserves dimensions");
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    DWORD pixels[6]{};
    if (SUCCEEDED(result)) result = converter->CopyPixels(nullptr, 12, sizeof(pixels), reinterpret_cast<BYTE*>(pixels));
    check(SUCCEEDED(result) && (pixels[0] & 0xffffff) == 0xff0000 && (pixels[3] & 0xffffff) == 0x112233,
          "PNG preserves pixel colors and top-down row order");
    converter.Reset(); frame.Reset(); decoder.Reset(); factory.Reset();
    std::error_code ignored;
    std::filesystem::remove(filename, ignored);
    if (bitmap) DeleteObject(bitmap);
}
void clipboardTests(bool allowWrite = false) {
    const DWORD before = GetClipboardSequenceNumber();
    check(!desk::copyBitmapToClipboard(nullptr), "null clipboard bitmap is rejected");
    check(GetClipboardSequenceNumber() == before, "invalid copy leaves clipboard untouched");
    if (!allowWrite) { std::cout << "SKIP: live clipboard writes; use --isolated-clipboard\n"; return; }
    // Clipboard content is user data. The synthetic round trip runs only when it
    // is empty, and restores that empty state after testing. Occupied clipboards
    // are left untouched, including private or delayed-rendered formats.
    if (!OpenClipboard(nullptr)) { std::cout << "SKIP: clipboard locked\n"; return; }
    const bool empty = CountClipboardFormats() == 0;
    CloseClipboard();
    if (!empty) { std::cout << "SKIP: clipboard contains user data\n"; return; }
    HBITMAP bitmap = synthetic();
    check(desk::copyBitmapToClipboard(bitmap), "synthetic clipboard export");
    const DWORD testSequence = GetClipboardSequenceNumber();
    check(IsClipboardFormatAvailable(CF_DIB) != FALSE, "clipboard contains interoperable DIB format");
    if (OpenClipboard(nullptr)) {
        const auto dib = static_cast<HGLOBAL>(GetClipboardData(CF_DIB));
        const auto* header = static_cast<const BITMAPINFOHEADER*>(GlobalLock(dib));
        check(header && header->biWidth == 3 && header->biHeight == 2 && header->biBitCount == 32,
              "clipboard DIB dimensions / format");
        if (header) {
            const auto* colors = reinterpret_cast<const DWORD*>(header + 1);
            check((colors[0] & 0xffffff) == 0x112233 && (colors[3] & 0xffffff) == 0xff0000,
                  "clipboard DIB pixel colors / bottom-up row order");
        }
        if (header) GlobalUnlock(dib);
        // Do not clear data another process copied while the test was running.
        if (GetClipboardSequenceNumber() == testSequence) EmptyClipboard();
        CloseClipboard();
    } else check(false, "clipboard reopens for validation / cleanup");
    if (bitmap) DeleteObject(bitmap);
}
void isolatedClipboardTests() {
    // A private noninteractive window station has its own clipboard. This
    // verifies the actual export without replacing any interactive user data.
    HWINSTA originalStation = GetProcessWindowStation();
    HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId());
    HWINSTA station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    check(station != nullptr, "create isolated test window station");
    if (!station) return;
    if (!SetProcessWindowStation(station)) {
        check(false, "attach isolated test window station"); CloseWindowStation(station); return;
    }
    HDESK desktop = CreateDesktopW(L"DeskCaptureTests", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    check(desktop != nullptr, "create isolated test desktop");
    if (desktop && SetThreadDesktop(desktop)) clipboardTests(true);
    else check(false, "attach isolated test desktop");
    check(SetProcessWindowStation(originalStation) != FALSE, "restore original window station");
    check(SetThreadDesktop(originalDesktop) != FALSE, "restore original desktop");
    if (desktop) CloseDesktop(desktop);
    CloseWindowStation(station);
}
LRESULT CALLBACK seedProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps);
        RECT client{}; GetClientRect(window, &client);
        HBRUSH paper = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(dc, &client, paper); DeleteObject(paper);
        for (int y = 0; y < 240; y += 8) for (int x = 0; x < 320; x += 8) {
            if ((x / 8 + y / 8) % 2) {
                HBRUSH cell = CreateSolidBrush(RGB(225, 235, 245));
                RECT box{x, y, x + 8, y + 8}; FillRect(dc, &box, cell); DeleteObject(cell);
            }
        }
        EndPaint(window, &ps); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void drag(HWND overlay, POINT start, POINT finish) {
    ScreenToClient(overlay, &start); ScreenToClient(overlay, &finish);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start.x, start.y));
    SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(finish.x, finish.y));
    SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(finish.x, finish.y));
}
void click(HWND overlay, POINT point) {
    ScreenToClient(overlay, &point);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
}
void hoverAt(HWND overlay,POINT point){
    ScreenToClient(overlay,&point);SendMessageW(overlay,WM_MOUSEMOVE,0,MAKELPARAM(point.x,point.y));
}
void overlayTests() {
    HWND oldFocus = GetForegroundWindow();
    WNDCLASSEXW type{sizeof(type)}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = seedProc;
    type.lpszClassName = L"DeskCaptureTestSeed"; RegisterClassExW(&type);
    HWND seed = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, type.lpszClassName, L"Capture test pattern", WS_POPUP,
        80, 80, 320, 240, nullptr, nullptr, type.hInstance, nullptr);
    check(seed != nullptr, "test pattern window creation");
    if (!seed) return;
    ShowWindow(seed, SW_SHOW); UpdateWindow(seed); DwmFlush();
    HBITMAP output{}; std::wstring action;
    desk::beginCapture(nullptr, [&](HBITMAP bitmap, const std::wstring& name) { output = bitmap; action = name; });
    HWND overlay = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    check(overlay && desk::captureActive(), "overlay opens without blocking the message thread");
    if (overlay) {
        hoverAt(overlay,{100,100});
        const auto firstHover=desk::capture_detail::hoverInfo();
        RECT expected{};GetWindowRect(seed,&expected);
        check(!firstHover.selected&&EqualRect(&firstHover.region,&expected),"before clicking the hover outline matches the application window");
        UpdateWindow(overlay);
        hoverAt(overlay,{108,100});
        const auto movedHover=desk::capture_detail::hoverInfo();
        check(movedHover.pixel.x==108&&movedHover.pixel.y==100&&movedHover.rgb!=firstHover.rgb,
              "moving within one window updates physical pixel coordinates and original RGB");
        check(GetUpdateRect(overlay,nullptr,FALSE)!=FALSE,"same-window pointer movement repaints the pixel information");
        HWND other=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,type.lpszClassName,L"Second synthetic application",WS_POPUP,480,80,300,240,nullptr,nullptr,type.hInstance,nullptr);
        ShowWindow(other,SW_SHOWNOACTIVATE);UpdateWindow(other);
        hoverAt(overlay,{500,100});
        const auto secondHover=desk::capture_detail::hoverInfo();GetWindowRect(other,&expected);
        check(!secondHover.selected&&EqualRect(&secondHover.region,&expected),"hover outline follows another application before the first click");
        DestroyWindow(other);
        desk::beginCapture(nullptr, [](HBITMAP bitmap, const std::wstring&) { DeleteObject(bitmap); });
        check(FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr) == overlay, "duplicate capture reuses the active overlay");
        drag(overlay, {100, 100}, {260, 210});
        drag(overlay, {260, 155}, {270, 155}); drag(overlay, {270, 155}, {260, 155});
        SendMessageW(overlay, WM_KEYDOWN, VK_RIGHT, 0); SendMessageW(overlay, WM_KEYDOWN, VK_LEFT, 0);
        SendMessageW(overlay, WM_KEYDOWN, '1', 0); drag(overlay, {115, 115}, {170, 155});
        SendMessageW(overlay, WM_KEYDOWN, '2', 0); drag(overlay, {200, 118}, {220, 153});
        SendMessageW(overlay, WM_KEYDOWN, '3', 0); drag(overlay, {120, 180}, {175, 190});
        SendMessageW(overlay, WM_KEYDOWN, '4', 0);
        click(overlay, {180, 155});
        HWND edit = FindWindowExW(overlay, nullptr, L"EDIT", nullptr);
        check(edit != nullptr, "text annotation uses a native editable control");
        if (edit) {
            SetWindowTextW(edit, L"测试 Test");
            SendMessageW(edit, WM_KILLFOCUS, reinterpret_cast<WPARAM>(overlay), 0);
            SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        }
        click(overlay, {110, 160});
        HWND nextEdit = FindWindowExW(overlay, nullptr, L"EDIT", nullptr);
        if (nextEdit) SetWindowTextW(nextEdit, L"第二段");
        pump();
        check(nextEdit && FindWindowExW(overlay, nullptr, L"EDIT", nullptr) == nextEdit,
              "stale focus-loss messages do not commit a later text editor");
        if (IsWindow(nextEdit)) SendMessageW(nextEdit, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(overlay, WM_KEYDOWN, '5', 0); drag(overlay, {205, 180}, {245, 201});
        SendMessageW(overlay, WM_KEYDOWN, 'O', 0);
        check(output && action == L"ocr", "OCR action transfers the annotated bitmap to the host");
        check(!desk::captureActive(), "completion destroys the overlay");
        if (desk::captureActive()) SendMessageW(overlay, WM_CLOSE, 0, 0);
    }
    if (output) {
        BITMAP dimensions{}; GetObjectW(output, sizeof(dimensions), &dimensions);
        check(dimensions.bmWidth == 160 && dimensions.bmHeight == 110, "annotated crop preserves selection dimensions");
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 160; info.bmiHeader.biHeight = -110; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        std::vector<DWORD> pixels(160 * 110); HDC dc = GetDC(nullptr);
        GetDIBits(dc, output, 0, 110, pixels.data(), &info, DIB_RGB_COLORS); ReleaseDC(nullptr, dc);
        auto reddish = [](DWORD value) { return ((value >> 16) & 255) > 200 && ((value >> 8) & 255) < 150; };
        check(reddish(pixels[15 * 160 + 15]), "rectangle annotation is translated into cropped image coordinates");
        check(reddish(pixels[90 * 160 + 75]), "freehand annotation reaches the pointer endpoint");
        unsigned textPixels{};
        for (int y = 55; y < 78; ++y) for (int x = 80; x < 158; ++x) textPixels += reddish(pixels[y * 160 + x]);
        check(textPixels > 20, "Unicode text annotation is rendered into the cropped image");
        const DWORD mosaicColor = pixels[84 * 160 + 116] & 0xffffff;
        check((pixels[84 * 160 + 117] & 0xffffff) == mosaicColor && (pixels[85 * 160 + 116] & 0xffffff) == mosaicColor,
              "mosaic produces pixelated blocks in the cropped output");
        DeleteObject(output);
    }
    // Repeated cancellation must release the full-screen bitmaps and memory DCs.
    pump();
    const DWORD handlesBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 5; ++i) {
        desk::beginCapture(nullptr, {});
        HWND active = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
        if (active) SendMessageW(active, WM_KEYDOWN, VK_ESCAPE, 0);
        pump();
    }
    check(!desk::captureActive(), "Escape cancels capture");
    desk::beginCapture(nullptr, {});
    HWND deactivated = FindWindowW(L"DeskEfficiencyCaptureOverlay", nullptr);
    if (deactivated) SendMessageW(deactivated, WM_ACTIVATEAPP, FALSE, 0);
    check(!desk::captureActive(), "changing to another application dismisses the editor");
    if (desk::captureActive()) SendMessageW(deactivated, WM_CLOSE, 0, 0);
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= handlesBefore + 2, "repeated cancellation does not leak GDI objects");
    DestroyWindow(seed);
    if (IsWindow(oldFocus)) SetForegroundWindow(oldFocus);
}
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--isolated-clipboard") {
        isolatedClipboardTests();
        std::cout << (failures ? "FAILED: " : "PASS: ") << failures << " isolated clipboard failures\n";
        return failures ? 1 : 0;
    }
    const auto dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    regionTests(); pngTests(); clipboardTests();
    if (argc > 1 && std::string_view(argv[1]) == "--overlay") overlayTests();
    if (SUCCEEDED(com)) CoUninitialize();
    if (dpi) SetThreadDpiAwarenessContext(dpi);
    std::cout << (failures ? "FAILED: " : "PASS: ") << failures << " capture failures\n";
    return failures ? 1 : 0;
}
