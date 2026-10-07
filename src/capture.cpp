#include "capture.hpp"
#include "image_tools.hpp"
#include <objidl.h>
#include <ole2.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace desk {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::uint64_t maxPixels = 32ULL * 1024 * 1024;
constexpr UINT saveFinished = WM_APP + 0x521;
constexpr UINT commitTextMessage = WM_APP + 0x522;
constexpr UINT translationFinished = WM_APP + 0x523;
constexpr int mosaicBlock = 12;
std::atomic<HWND> activeWindow{};
std::atomic<bool> saving{};
std::atomic<UINT_PTR> nextSaveId{1};
std::atomic<UINT_PTR> nextTranslationId{1};
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous{SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
};
struct ComScope {
    HRESULT result{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool usable() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
struct BitmapSurface {
    HBITMAP bitmap{}; HDC dc{}; HGDIOBJ previous{};
    DWORD* pixels{}; int width{}, height{};
    BitmapSurface() = default;
    BitmapSurface(const BitmapSurface&) = delete;
    BitmapSurface& operator=(const BitmapSurface&) = delete;
    ~BitmapSurface() { clear(); }
    void clear() {
        if (dc && previous) SelectObject(dc, previous);
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        bitmap = nullptr; dc = nullptr; previous = nullptr; pixels = nullptr;
    }
    bool create(int w, int h) {
        clear();
        if (w <= 0 || h <= 0 || static_cast<std::uint64_t>(w) * h > maxPixels) return false;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* storage{};
        bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &storage, nullptr, 0);
        dc = CreateCompatibleDC(nullptr);
        if (!bitmap || !dc) { clear(); return false; }
        previous = SelectObject(dc, bitmap);
        pixels = static_cast<DWORD*>(storage); width = w; height = h; return true;
    }
    bool adopt(HBITMAP owned) {
        clear(); bitmap = owned; BITMAP dimensions{};
        if (!owned || !GetObjectW(owned, sizeof(dimensions), &dimensions) || dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0 ||
            static_cast<std::uint64_t>(dimensions.bmWidth) * dimensions.bmHeight > maxPixels) { clear(); return false; }
        dc = CreateCompatibleDC(nullptr); if (!dc) { clear(); return false; }
        previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) { previous = nullptr; clear(); return false; }
        width = dimensions.bmWidth; height = dimensions.bmHeight; return true;
    }
    HBITMAP release() {
        HBITMAP result = bitmap;
        if (dc && previous) SelectObject(dc, previous);
        if (dc) DeleteDC(dc);
        bitmap = nullptr; dc = nullptr; previous = nullptr; pixels = nullptr; return result;
    }
};
RECT normalized(RECT r) {
    if (r.left > r.right) std::swap(r.left, r.right);
    if (r.top > r.bottom) std::swap(r.top, r.bottom);
    return r;
}
bool validSize(RECT r) {
    const std::int64_t w = static_cast<std::int64_t>(r.right) - r.left;
    const std::int64_t h = static_cast<std::int64_t>(r.bottom) - r.top;
    return w > 0 && h > 0 && w <= INT_MAX && h <= INT_MAX && static_cast<std::uint64_t>(w) * h <= maxPixels;
}
bool contains(RECT r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
RECT pointRect(POINT a, POINT b) { return normalized({a.x, a.y, b.x, b.y}); }
RECT desktopRect() {
    return {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}
bool writePng(HBITMAP bitmap, const std::filesystem::path& path, const std::atomic<bool>* cancelled = nullptr) {
    if (!bitmap || path.empty()) return false;
    BITMAP dimensions{};
    if (!GetObjectW(bitmap, sizeof(dimensions), &dimensions) || dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0 ||
        static_cast<std::uint64_t>(dimensions.bmWidth) * dimensions.bmHeight > maxPixels) return false;
    ComScope com; if (!com.usable()) return false;
    GUID unique{}; if (FAILED(CoCreateGuid(&unique))) return false;
    wchar_t suffix[40]{}; StringFromGUID2(unique, suffix, 40);
    auto temporary = path; temporary += std::wstring(L".") + suffix + L".tmp";
    HRESULT result{};
    {
        ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmap> source;
        ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> options;
        ComPtr<IWICFormatConverter> converter;
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result)) result = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source);
        if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
        if (SUCCEEDED(result)) result = stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, &options);
        if (SUCCEEDED(result)) result = frame->Initialize(options.Get());
        if (SUCCEEDED(result)) result = frame->SetSize(dimensions.bmWidth, dimensions.bmHeight);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
        if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result)) result = converter->Initialize(source.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(result) && cancelled && cancelled->load()) result = E_ABORT;
        if (SUCCEEDED(result)) result = frame->WriteSource(converter.Get(), nullptr);
        if (SUCCEEDED(result)) result = frame->Commit();
        if (SUCCEEDED(result)) result = encoder->Commit();
    }
    const bool success = SUCCEEDED(result) && !(cancelled && cancelled->load()) &&
        MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!success) DeleteFileW(temporary.c_str());
    return success;
}
enum class Tool { Select, Rectangle, Arrow, Pen, Text, Mosaic, Ellipse, Number };
enum class Action { Select, Rectangle, Arrow, Pen, Text, Mosaic, Ellipse, Number, Undo, Redo, Copy, Save, Ocr, Translate, Pin, Long, Gif, Mp4, Cancel };
enum class Drag { None, Select, Move, Resize, Annotate };
struct Annotation {
    Tool tool{}; std::vector<POINT> points; RECT box{}; std::wstring text;
    DWORD color{0xffff626d}; float thickness{3.0f}; int number{};
};
struct SaveTask { std::atomic<bool> cancelled{}; UINT_PTR id{}; image_tools_detail::MemoryLease memory; };
struct TranslationTask {
    std::atomic_bool cancelled{}; UINT_PTR id{};
    std::mutex mutex; HBITMAP source{}, result{};
    std::wstring error;
    // Reserve source + returned bitmap before making either allocation.
    image_tools_detail::MemoryLease memory;
    ~TranslationTask() { if (result) DeleteObject(result); if (source) DeleteObject(source); }
};
std::wstring translationError(const char* message) {
    if (!message) return L"翻译服务返回未知错误。";
    int count{}; while (count < 800 && message[count]) ++count;
    const UINT encoding = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message, count, nullptr, 0) ? CP_UTF8 : CP_ACP;
    const int length = MultiByteToWideChar(encoding, 0, message, count, nullptr, 0);
    if (length <= 0) return L"翻译服务返回未知错误。";
    std::wstring text(length, L'\0'); MultiByteToWideChar(encoding, 0, message, count, text.data(), length);
    if (text.size() > 200) text.resize(200);
    return text;
}
struct Button { RECT rect{}; Action action{}; const wchar_t* label{}; const wchar_t* title{}; };
constexpr DWORD paletteColors[]{0xffff626d, 0xffffa45f, 0xffffd461, 0xff58ddb2, 0xff79b6ff, 0xffba94ff, 0xffffffff};
constexpr float paletteWidths[]{2, 4, 7, 10};
struct Overlay {
    HWND window{}, owner{}, previousForeground{}, edit{}; WNDPROC originalEditProc{}; UINT_PTR editGeneration{};
    HFONT editFont{}; HBRUSH editBrush{};
    bool ownerWasVisible{}, selected{}, busy{}, modalDialog{}, cursorKnown{}, imeComposing{}, originalPreview{}, translationFailed{};
    bool pixelCopied=false;
    RECT screen{}, selection{}, hoverSelection{}, dragOriginal{}, toolbar{}; POINT down{}, cursor{};
    Tool tool{Tool::Select}; Drag drag{Drag::None}; int resizeEdges{}, hover{-1};
    float scale{1}; DWORD color{0xffff626d}; float thickness{3};
    BitmapSurface snapshot, dimmed, paint, mosaic, translated;
    RECT translatedSelection{};
    std::vector<Annotation> annotations, redoAnnotations; Annotation draft; CaptureCallback callback; RegionCallback recordingCallback; InlineTranslation translation;
    std::array<RECT, std::size(paletteColors)> colorRects{};
    std::array<RECT, std::size(paletteWidths)> widthRects{};
    std::vector<Button> buttons; ULONG_PTR gdiplus{}; std::shared_ptr<SaveTask> saveTask;
    std::shared_ptr<TranslationTask> translationTask;
    image_tools_detail::MemoryLease translatedMemory;
    std::wstring translationStatus;
    image_tools_detail::MemoryLease memory;
    ~Overlay() {
        if (saveTask) saveTask->cancelled = true;
        if (translationTask) translationTask->cancelled = true;
        if (editFont) DeleteObject(editFont);
        if (editBrush) DeleteObject(editBrush);
        if (gdiplus) Gdiplus::GdiplusShutdown(gdiplus);
        // Release actual pixel storage before advertising free reservation
        // space; detached encoders can retain their own reference to the lease.
        retireSurfaces();
    }
    POINT mouse() const {
        if (cursorKnown) return cursor;
        POINT p{}; GetCursorPos(&p); ScreenToClient(window, &p);
        p.x = std::clamp<LONG>(p.x, 0, snapshot.width); p.y = std::clamp<LONG>(p.y, 0, snapshot.height); return p;
    }
    POINT samplePoint()const{
        const auto point=mouse();
        return {std::clamp<LONG>(point.x,0,snapshot.width-1),std::clamp<LONG>(point.y,0,snapshot.height-1)};
    }
    DWORD sampleRgb()const{auto point=samplePoint();return snapshot.pixels[(size_t)point.y*snapshot.width+point.x]&0xffffff;}
    std::wstring sampleHex()const{wchar_t value[8]{};swprintf_s(value,L"#%06X",sampleRgb());return value;}
    void copySample(){
        const auto value=sampleHex();auto memory=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
        if(!memory)return;auto data=GlobalLock(memory);if(!data){GlobalFree(memory);return;}
        memcpy(data,value.c_str(),(value.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);
        if(!OpenClipboard(window)){GlobalFree(memory);return;}
        EmptyClipboard();const bool copied=SetClipboardData(CF_UNICODETEXT,memory)!=nullptr;CloseClipboard();
        if(!copied)GlobalFree(memory);pixelCopied=copied;invalidate();
    }
    void eventCursor(LPARAM coordinates) {
        POINT p{GET_X_LPARAM(coordinates), GET_Y_LPARAM(coordinates)};
        // Zero LPARAM also supports older callers that synthesize messages
        // after positioning the cursor. Real mouse (0,0) resolves identically.
        if (!coordinates) { POINT actual{}; if (GetCursorPos(&actual)) { ScreenToClient(window, &actual); p = actual; } }
        cursor = {std::clamp<LONG>(p.x, 0, snapshot.width), std::clamp<LONG>(p.y, 0, snapshot.height)}; cursorKnown = true;
    }
    void updateHover() {
        struct Search { POINT point{}; HWND excluded{}; RECT result{}; } search{{cursor.x + screen.left, cursor.y + screen.top}, window, {}};
        EnumWindows([](HWND candidate, LPARAM value) -> BOOL {
            auto& data = *reinterpret_cast<Search*>(value);
            if (candidate == data.excluded || !IsWindowVisible(candidate) || IsIconic(candidate) ||
                (GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_TRANSPARENT)) return TRUE;
            DWORD cloaked{}; DwmGetWindowAttribute(candidate, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)); if (cloaked) return TRUE;
            RECT bounds{}; if (FAILED(DwmGetWindowAttribute(candidate, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) GetWindowRect(candidate, &bounds);
            if (!PtInRect(&bounds, data.point)) return TRUE;
            data.result = bounds; return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        if (!validSize(search.result)) {
            MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint(search.point, MONITOR_DEFAULTTONEAREST), &monitor); search.result = monitor.rcMonitor;
        }
        RECT clipped{}; IntersectRect(&clipped, &search.result, &screen); OffsetRect(&clipped, -screen.left, -screen.top);
        if (!EqualRect(&clipped, &hoverSelection)) { hoverSelection = clipped; invalidate(); }
    }
    void addAnnotation(Annotation annotation) { if (annotations.size() < 256) { annotations.push_back(std::move(annotation)); redoAnnotations.clear(); } }
    void undo() {
        if (!annotations.empty()) { redoAnnotations.push_back(std::move(annotations.back())); annotations.pop_back(); } invalidate();
    }
    void redo() {
        if (!redoAnnotations.empty()) { annotations.push_back(std::move(redoAnnotations.back())); redoAnnotations.pop_back(); } invalidate();
    }
    void invalidate() { InvalidateRect(window, nullptr, FALSE); }
    void retireSurfaces() {
        clearTranslation();
        snapshot.clear(); dimmed.clear(); paint.clear(); mosaic.clear(); memory.reset();
    }
    void clearTranslation() {
        translated.clear(); translatedMemory.reset(); translatedSelection = {}; originalPreview = false; translationFailed = false; translationStatus.clear();
    }
    bool translatedSelected() const { return translated.bitmap && EqualRect(&selection, &translatedSelection); }
    int hitButton(POINT p) const {
        for (size_t i = 0; i < buttons.size(); ++i) if (contains(buttons[i].rect, p)) return static_cast<int>(i);
        return -1;
    }
    int hitEdge(POINT p) const {
        if (!selected) return 0;
        const int margin = static_cast<int>(7 * scale);
        if (p.x < selection.left - margin || p.x > selection.right + margin || p.y < selection.top - margin || p.y > selection.bottom + margin) return 0;
        int edges{};
        if (std::abs(p.x - selection.left) <= margin) edges |= 1;
        if (std::abs(p.x - selection.right) <= margin) edges |= 2;
        if (std::abs(p.y - selection.top) <= margin) edges |= 4;
        if (std::abs(p.y - selection.bottom) <= margin) edges |= 8;
        return edges;
    }
    void layoutToolbar() {
        buttons.clear();
        colorRects.fill({}); widthRects.fill({});
        if (!selected || drag == Drag::Select || drag == Drag::Move || drag == Drag::Resize) return;
        const int button = static_cast<int>(34 * scale), bh = static_cast<int>(36 * scale), pad = static_cast<int>(6 * scale), gap = static_cast<int>(3 * scale), groupGap = static_cast<int>(11 * scale);
        POINT anchor{screen.left + (selection.left + selection.right) / 2, screen.top + selection.bottom};
        MONITORINFO monitor{sizeof(MONITORINFO)}; GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT bounds{monitor.rcWork.left - screen.left, monitor.rcWork.top - screen.top, monitor.rcWork.right - screen.left, monitor.rcWork.bottom - screen.top};
        static constexpr std::array<Action, 17> actions{Action::Select, Action::Rectangle, Action::Ellipse, Action::Arrow, Action::Pen, Action::Text,
            Action::Mosaic, Action::Number, Action::Pin, Action::Long, Action::Gif, Action::Mp4, Action::Ocr, Action::Translate,
            Action::Save, Action::Cancel, Action::Copy};
        static constexpr const wchar_t* labels[]{L"选择", L"矩形", L"圆形", L"箭头", L"画笔", L"文字", L"马赛克", L"序号",
            L"贴图", L"长图", L"GIF", L"录屏", L"识别", L"翻译", L"保存", L"取消", L"复制"};
        static constexpr const wchar_t* titles[]{L"选择 / 移动 V", L"矩形 1", L"圆形 / 椭圆 6", L"箭头 2", L"自由画笔 3", L"文字 4 · Shift+Enter 换行",
            L"马赛克 5", L"序号标注 7", L"置顶贴图 P", L"滚动长截图 L", L"GIF 录制 G", L"MP4 录屏 M", L"识别文字 O",
            L"原图翻译 T · 按住空格看原图", L"保存 PNG Ctrl+S", L"取消 Esc", L"复制 Enter"};
        const int available = std::max<int>(button * 2 + gap, bounds.right - bounds.left - pad * 4);
        int bx = pad, by = pad, widest = pad;
        for (size_t i = 0; i < actions.size(); ++i) {
            const bool group = i == 8 || i == 12 || i == 14 || i == 15;
            const int before = bx == pad ? 0 : group ? groupGap : gap;
            const int needed = i == 15 ? button * 2 + gap : button;
            if (bx + before + needed > available + pad && bx != pad) { bx = pad; by += bh + gap; }
            else bx += before;
            buttons.push_back({{bx, by, bx + button, by + bh}, actions[i], labels[i], titles[i]});
            bx += button; widest = std::max(widest, bx);
        }
        const bool palette = tool != Tool::Select;
        const int width = std::max(widest + pad, palette ? static_cast<int>(338 * scale) : 0);
        const int terminalShift = width - pad - buttons.back().rect.right;
        OffsetRect(&buttons[buttons.size() - 2].rect, terminalShift, 0);
        OffsetRect(&buttons.back().rect, terminalShift, 0);
        const int height = by + bh + pad + (palette ? static_cast<int>(34 * scale) : 0);
        const int x = std::clamp<int>((selection.left + selection.right - width) / 2, bounds.left + pad,
            std::max<int>(bounds.left + pad, bounds.right - width - pad));
        int y = selection.bottom + static_cast<int>(14 * scale);
        if (y + height > bounds.bottom - pad) y = selection.top - height - static_cast<int>(14 * scale);
        y = std::clamp<int>(y, bounds.top + pad, std::max<int>(bounds.top + pad, bounds.bottom - height - pad));
        toolbar = {x, y, x + width, y + height};
        for (auto& item : buttons) OffsetRect(&item.rect, x, y);
        if (!palette) return;
        const int paletteY = y + height - static_cast<int>(31 * scale), sw = static_cast<int>(24 * scale);
        for (size_t i = 0; i < colorRects.size(); ++i) colorRects[i] = {x + pad + static_cast<LONG>(i * (sw + gap)), paletteY,
            x + pad + static_cast<LONG>(i * (sw + gap)) + sw, paletteY + sw};
        const int lineX = colorRects.back().right + static_cast<int>(18 * scale);
        for (size_t i = 0; i < widthRects.size(); ++i) widthRects[i] = {lineX + static_cast<LONG>(i * (sw + gap)), paletteY,
            lineX + static_cast<LONG>(i * (sw + gap)) + sw, paletteY + sw};
    }
    void close(bool restoreFocus = true) {
        const HWND handle = window, focus = previousForeground, panel = owner;
        const bool restore = ownerWasVisible;
        DestroyWindow(handle);
        if (restore && IsWindow(panel)) { ShowWindow(panel, SW_SHOWNA); if (restoreFocus) SetForegroundWindow(panel); }
        else if (restoreFocus && IsWindow(focus) && IsWindowVisible(focus)) SetForegroundWindow(focus);
    }
    bool makeMosaic() {
        if (mosaic.bitmap) return true;
        if (!mosaic.create((snapshot.width + mosaicBlock - 1) / mosaicBlock, (snapshot.height + mosaicBlock - 1) / mosaicBlock)) return false;
        GdiFlush();
        for (int y = 0; y < mosaic.height; ++y) for (int x = 0; x < mosaic.width; ++x) {
            unsigned red{}, green{}, blue{}, count{};
            for (int sy = y * mosaicBlock; sy < std::min((y + 1) * mosaicBlock, snapshot.height); sy += 3)
                for (int sx = x * mosaicBlock; sx < std::min((x + 1) * mosaicBlock, snapshot.width); sx += 3) {
                    DWORD pixel = snapshot.pixels[static_cast<size_t>(sy) * snapshot.width + sx];
                    red += (pixel >> 16) & 255; green += (pixel >> 8) & 255; blue += pixel & 255; ++count;
                }
            mosaic.pixels[static_cast<size_t>(y) * mosaic.width + x] = 0xff000000 | ((red / count) << 16) | ((green / count) << 8) | blue / count;
        }
        return true;
    }
    void drawAnnotation(HDC dc, Gdiplus::Graphics& graphics, const Annotation& annotation) {
        if (annotation.points.empty()) return;
        Gdiplus::Pen pen(Gdiplus::Color(annotation.color), annotation.thickness);
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
        const POINT a = annotation.points.front(), b = annotation.points.back(); RECT box = normalized(annotation.box);
        switch (annotation.tool) {
        case Tool::Rectangle:
            if (box.right > box.left && box.bottom > box.top) graphics.DrawRectangle(&pen, static_cast<float>(box.left), static_cast<float>(box.top),
                static_cast<float>(box.right - box.left), static_cast<float>(box.bottom - box.top)); break;
        case Tool::Ellipse:
            if (box.right > box.left && box.bottom > box.top) graphics.DrawEllipse(&pen, static_cast<float>(box.left), static_cast<float>(box.top),
                static_cast<float>(box.right - box.left), static_cast<float>(box.bottom - box.top)); break;
        case Tool::Number: {
            const float radius = 13 * scale + annotation.thickness / 2;
            Gdiplus::SolidBrush fill(Gdiplus::Color(annotation.color)), ink(Gdiplus::Color(255, 255, 255));
            graphics.FillEllipse(&fill, a.x - radius, a.y - radius, radius * 2, radius * 2);
            Gdiplus::Font font(L"Segoe UI", 16 * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat center; center.SetAlignment(Gdiplus::StringAlignmentCenter); center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            const auto number = std::to_wstring(annotation.number);
            graphics.DrawString(number.c_str(), -1, &font, Gdiplus::RectF(a.x - radius, a.y - radius, radius * 2, radius * 2), &center, &ink); break;
        }
        case Tool::Arrow: {
            graphics.DrawLine(&pen, static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(b.x), static_cast<float>(b.y));
            const double angle = std::atan2(static_cast<double>(b.y - a.y), static_cast<double>(b.x - a.x));
            const float head = std::max(12.0f, annotation.thickness * 4);
            Gdiplus::PointF triangle[3]{{static_cast<float>(b.x), static_cast<float>(b.y)},
                {b.x - head * static_cast<float>(std::cos(angle - .42)), b.y - head * static_cast<float>(std::sin(angle - .42))},
                {b.x - head * static_cast<float>(std::cos(angle + .42)), b.y - head * static_cast<float>(std::sin(angle + .42))}};
            Gdiplus::SolidBrush brush(Gdiplus::Color(annotation.color)); graphics.FillPolygon(&brush, triangle, 3); break;
        }
        case Tool::Pen:
            if (annotation.points.size() == 1) {
                Gdiplus::SolidBrush brush(Gdiplus::Color(annotation.color));
                graphics.FillEllipse(&brush, a.x - annotation.thickness / 2, a.y - annotation.thickness / 2, annotation.thickness, annotation.thickness);
            } else {
                std::vector<Gdiplus::PointF> path; path.reserve(annotation.points.size());
                for (POINT p : annotation.points) path.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
                graphics.DrawLines(&pen, path.data(), static_cast<int>(path.size()));
            } break;
        case Tool::Text: {
            Gdiplus::Font font(L"Segoe UI", 18 * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush brush(Gdiplus::Color(annotation.color)); Gdiplus::StringFormat format;
            Gdiplus::RectF textBox(static_cast<float>(a.x), static_cast<float>(a.y),
                static_cast<float>(std::max(1L, annotation.box.right - a.x)), static_cast<float>(std::max(1L, annotation.box.bottom - a.y)));
            graphics.DrawString(annotation.text.c_str(), static_cast<int>(annotation.text.size()), &font, textBox, &format, &brush); break;
        }
        case Tool::Mosaic:
            if (mosaic.bitmap && box.right > box.left && box.bottom > box.top) {
                graphics.Flush(Gdiplus::FlushIntentionSync); const int saved = SaveDC(dc);
                IntersectClipRect(dc, box.left, box.top, box.right, box.bottom);
                const int x = box.left / mosaicBlock, y = box.top / mosaicBlock;
                const int w = (box.right + mosaicBlock - 1) / mosaicBlock - x, h = (box.bottom + mosaicBlock - 1) / mosaicBlock - y;
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchBlt(dc, x * mosaicBlock, y * mosaicBlock, w * mosaicBlock, h * mosaicBlock, mosaic.dc, x, y, w, h, SRCCOPY);
                RestoreDC(dc, saved);
            } break;
        default: break;
        }
    }
    HBITMAP sourceBitmap() {
        if (!selected || !validSize(selection)) return nullptr;
        BitmapSurface output;
        if (!output.create(selection.right - selection.left, selection.bottom - selection.top) ||
            !BitBlt(output.dc, 0, 0, output.width, output.height, snapshot.dc, selection.left, selection.top, SRCCOPY)) return nullptr;
        GdiFlush();
        return output.release();
    }
    void translate() {
        if (busy || !selected || !validSize(selection)) return;
        commitText();
        if (!translation) { translationFailed = true; translationStatus = L"翻译未连接，请检查翻译设置后重试。"; invalidate(); return; }
        clearTranslation();
        std::shared_ptr<TranslationTask> task;
        try { task = std::make_shared<TranslationTask>(); } catch (...) {}
        const auto pixels = static_cast<std::uint64_t>(selection.right - selection.left) * (selection.bottom - selection.top);
        if (task) task->memory = image_tools_detail::reserveMemory(pixels * sizeof(DWORD) * 2);
        if (!task || !task->memory || !(task->source = sourceBitmap())) {
            translationFailed = true; translationStatus = L"翻译图像内存不足，请缩小选区或关闭贴图后重试。"; invalidate(); return;
        }
        task->id = nextTranslationId.fetch_add(1);
        translationTask = task; translatedSelection = selection; originalPreview = false;
        busy = true; translationStatus = L"正在识别并翻译 · Esc 取消"; invalidate();
        const HWND destination = window;
        try {
            std::thread([task, destination, handler = translation] {
                ComScope com; HBITMAP result{}; std::wstring error;
                try { if (!task->cancelled.load()) result = handler(task->source, task->cancelled); }
                catch (const std::exception& failure) { try { error = translationError(failure.what()); } catch (...) {} }
                catch (...) {}
                GdiFlush();
                // The callback borrows source and must return a distinct image.
                if (result == task->source) result = nullptr;
                BITMAP source{}, output{};
                if (result && (!GetObjectW(task->source, sizeof(source), &source) || !GetObjectW(result, sizeof(output), &output) ||
                    output.bmWidth != source.bmWidth || output.bmHeight != source.bmHeight)) { DeleteObject(result); result = nullptr; error = L"翻译结果尺寸与选区不一致。"; }
                {
                    std::lock_guard guard(task->mutex); task->result = result; task->error = std::move(error);
                }
                // Send only the session ID. An HWND reused by a later editor
                // cannot claim this task or leak an abandoned heap payload.
                if (!task->cancelled.load()) PostMessageW(destination, translationFinished, task->id, 0);
            }).detach();
        } catch (...) {
            task->cancelled = true; translationTask.reset(); busy = false;
            translationFailed = true; translationStatus = L"无法启动翻译，请重试。"; invalidate();
        }
    }
    void finishTranslation(UINT_PTR id) {
        if (!translationTask || translationTask->id != id || translationTask->cancelled.load()) return;
        auto task = std::move(translationTask); HBITMAP result{};
        { std::lock_guard guard(task->mutex); result = task->result; task->result = nullptr; }
        busy = false;
        if (result && translated.adopt(result)) {
            translatedMemory = task->memory; translationStatus = L"翻译完成 · 按住空格看原图";
        } else {
            translationFailed = true;
            translationStatus = task->error.empty() ? L"翻译失败，请检查翻译设置与网络后按 T 重试。" : task->error + L" · 按 T 重试";
        }
        invalidate();
    }
    HBITMAP resultBitmap() {
        if (!selected || !validSize(selection)) return nullptr;
        BitmapSurface output;
        if (!output.create(selection.right - selection.left, selection.bottom - selection.top)) return nullptr;
        const bool hasTranslation = translatedSelected();
        if (!BitBlt(output.dc, 0, 0, output.width, output.height, hasTranslation ? translated.dc : snapshot.dc,
            hasTranslation ? 0 : selection.left, hasTranslation ? 0 : selection.top, SRCCOPY)) return nullptr;
        SetViewportOrgEx(output.dc, -selection.left, -selection.top, nullptr);
        IntersectClipRect(output.dc, selection.left, selection.top, selection.right, selection.bottom);
        {
            Gdiplus::Graphics graphics(output.dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            for (const auto& annotation : annotations) drawAnnotation(output.dc, graphics, annotation);
            graphics.Flush(Gdiplus::FlushIntentionSync);
        }
        return output.release();
    }
    void copy() {
        commitText(); HBITMAP bitmap = resultBitmap(); const bool success = bitmap && copyBitmapToClipboard(bitmap);
        if (bitmap) DeleteObject(bitmap);
        if (success) { retireSurfaces(); close(); }
        else MessageBoxW(window, L"无法复制截图，剪贴板可能正被其他程序占用。请重试。", L"桌面提效", MB_OK | MB_ICONWARNING);
    }
    void deliver(const wchar_t* action) {
        commitText(); HBITMAP bitmap = resultBitmap();
        if (!bitmap) { MessageBoxW(window, L"无法生成截图，请缩小选区后重试。", L"桌面提效", MB_OK | MB_ICONWARNING); return; }
        auto completed = std::move(callback); retireSurfaces(); close();
        if (completed) {
            // Ownership transfers before invoking the host callback.
            try { completed(bitmap, action); } catch (...) { /* The host owns the bitmap. */ }
        } else DeleteObject(bitmap);
    }
    void pin() {
        commitText(); HBITMAP bitmap = resultBitmap();
        if (!bitmap) return;
        auto completed = std::move(callback); const HWND host = owner; retireSurfaces(); close(); pinImage(host, bitmap, std::move(completed));
    }
    void scrolling() {
        commitText();
        RECT physical = physicalSelection(selection, {screen.left, screen.top});
        auto completed = std::move(callback); const HWND host = owner; retireSurfaces(); close();
        if (!beginScrollingCapture(host, physical, std::move(completed))) MessageBoxW(host, L"无法开始长截图。选区高度至少 64 像素，请检查屏幕采样。", L"长截图", MB_OK | MB_ICONWARNING);
    }
    void record(const wchar_t* format) {
        if (!recordingCallback) { MessageBoxW(window, L"录制入口未连接，请从完整安装版重新打开截图。", L"录制", MB_OK | MB_ICONWARNING); return; }
        RECT physical = physicalSelection(selection, {screen.left, screen.top});
        auto completed = std::move(recordingCallback); retireSurfaces(); close();
        try { completed(physical, format); } catch (...) {}
    }
    void back() {
        if (edit) { cancelText(); return; }
        if (drag == Drag::Annotate) { drag = Drag::None; draft = {}; ReleaseCapture(); invalidate(); return; }
        if (tool != Tool::Select) { tool = Tool::Select; invalidate(); return; }
        if (selected) { clearTranslation(); selected = false; drag = Drag::None; annotations.clear(); redoAnnotations.clear(); buttons.clear(); updateHover(); invalidate(); return; }
        close();
    }
    void save() {
        commitText();
        if (saving.load()) { MessageBoxW(window, L"上一张截图仍在保存，请稍后重试。", L"桌面提效", MB_OK | MB_ICONINFORMATION); return; }
        wchar_t filename[32768] = L"截图.png"; OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window; dialog.lpstrFile = filename; dialog.nMaxFile = 32768;
        dialog.lpstrFilter = L"PNG 图像\0*.png\0\0"; dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        modalDialog = true;
        const BOOL chosen = GetSaveFileNameW(&dialog);
        modalDialog = false;
        if (!window || !IsWindow(window) || !chosen) return;
        HBITMAP bitmap = resultBitmap();
        if (!bitmap) { MessageBoxW(window, L"无法生成截图，请缩小选区后重试。", L"桌面提效", MB_OK | MB_ICONWARNING); return; }
        if (saving.exchange(true)) { DeleteObject(bitmap); return; }
        try { saveTask = std::make_shared<SaveTask>(); }
        catch (...) { saving = false; DeleteObject(bitmap); MessageBoxW(window, L"保存任务内存不足，请重试。", L"桌面提效", MB_OK | MB_ICONWARNING); return; }
        saveTask->id = nextSaveId.fetch_add(1); saveTask->memory = memory;
        auto task = saveTask; const HWND destination = window; const std::filesystem::path path(filename);
        busy = true; invalidate();
        try {
            std::thread([bitmap, task, destination, path] {
                bool success{};
                try { success = writePng(bitmap, path, &task->cancelled); } catch (...) {}
                DeleteObject(bitmap); saving = false;
                if (!task->cancelled.load()) PostMessageW(destination, saveFinished, task->id, success ? 1 : 0);
            }).detach();
        } catch (...) {
            saving = false; busy = false; saveTask.reset(); DeleteObject(bitmap);
            MessageBoxW(window, L"无法启动保存任务，请重试。", L"桌面提效", MB_OK | MB_ICONWARNING);
        }
    }
    void perform(Action action) {
        if (busy && action != Action::Cancel) return;
        commitText();
        if (action <= Action::Number) {
            tool = static_cast<Tool>(action); if (tool == Tool::Mosaic && !makeMosaic()) tool = Tool::Select;
            layoutToolbar();
            invalidate(); return;
        }
        switch (action) {
        case Action::Undo: undo(); break;
        case Action::Redo: redo(); break;
        case Action::Copy: copy(); break;
        case Action::Save: save(); break;
        case Action::Ocr: deliver(L"ocr"); break;
        case Action::Translate: translate(); break;
        case Action::Pin: pin(); break;
        case Action::Long: scrolling(); break;
        case Action::Gif: record(L"gif"); break;
        case Action::Mp4: record(L"mp4"); break;
        case Action::Cancel: close(); break;
        default: break;
        }
    }
    static LRESULT CALLBACK editProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam) {
        const auto dispatch = image_tools_detail::windowState<Overlay>(GetParent(control));
        auto* state = dispatch.get();
        if (!state || !state->originalEditProc) return DefWindowProcW(control, message, wparam, lparam);
        const WNDPROC original = state->originalEditProc;
        if (message == WM_IME_STARTCOMPOSITION) state->imeComposing = true;
        if (message == WM_IME_ENDCOMPOSITION) state->imeComposing = false;
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE && !state->imeComposing) { state->cancelText(); return 0; }
        if (message == WM_KEYDOWN && wparam == VK_RETURN && !state->imeComposing && !(GetKeyState(VK_SHIFT) & 0x8000)) { state->commitText(); return 0; }
        if (message == WM_KILLFOCUS) PostMessageW(state->window, commitTextMessage, state->editGeneration, 0);
        if (message == WM_GETDLGCODE) return CallWindowProcW(original, control, message, wparam, lparam) | DLGC_WANTALLKEYS;
        return CallWindowProcW(original, control, message, wparam, lparam);
    }
    void cancelText() {
        if (!edit) return;
        HWND control = edit; edit = nullptr;
        imeComposing = false;
        SetWindowLongPtrW(control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalEditProc));
        DestroyWindow(control); SetFocus(window); invalidate();
    }
    void commitText() {
        if (!edit) return;
        const int length = std::min(GetWindowTextLengthW(edit), 4096);
        if (length > 0 && annotations.size() < 256) {
            Annotation annotation; annotation.tool = Tool::Text; annotation.points.push_back(down);
            annotation.box = {down.x, down.y, selection.right, selection.bottom};
            annotation.text.resize(static_cast<size_t>(length) + 1);
            const int actual = GetWindowTextW(edit, annotation.text.data(), length + 1); annotation.text.resize(actual); annotation.color = color;
            if (actual) addAnnotation(std::move(annotation));
        }
        cancelText();
    }
    void startText(POINT point) {
        commitText(); down = point;
        const int width = std::min<int>(static_cast<int>(300 * scale), selection.right - point.x);
        const int height = std::min<int>(static_cast<int>(90 * scale), selection.bottom - point.y);
        if (width < 20 || height < 20 || annotations.size() >= 256) return;
        if (!editFont) editFont = CreateFontW(-static_cast<int>(18 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!editBrush) editBrush = CreateSolidBrush(RGB(26, 32, 43));
        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL,
            point.x, point.y, width, height, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!edit) return;
        ++editGeneration;
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(editFont), TRUE); SendMessageW(edit, EM_SETLIMITTEXT, 4096, 0);
        originalEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(editProc))); SetFocus(edit);
    }
    void pointerDown() {
        if(!selected)updateHover();
        const POINT point = mouse(); const int button = hitButton(point);
        if (busy && (button < 0 || buttons[button].action != Action::Cancel)) return;
        if (selected && !busy && !buttons.empty()) {
            for (size_t i = 0; i < colorRects.size(); ++i) if (contains(colorRects[i], point)) { commitText(); color = paletteColors[i]; invalidate(); return; }
            for (size_t i = 0; i < widthRects.size(); ++i) if (contains(widthRects[i], point)) { commitText(); thickness = paletteWidths[i]; invalidate(); return; }
        }
        if (button >= 0) { perform(buttons[button].action); return; }
        commitText(); down = point; resizeEdges = hitEdge(point);
        if (selected && resizeEdges) { clearTranslation(); drag = Drag::Resize; dragOriginal = selection; }
        else if (selected && contains(selection, point) && tool != Tool::Select) {
            if (tool == Tool::Text) { startText(point); return; }
            if (annotations.size() >= 256) return;
            draft = Annotation{}; draft.tool = tool; draft.color = color; draft.thickness = thickness * scale;
            draft.points.push_back(point); draft.box = {point.x, point.y, point.x, point.y}; drag = Drag::Annotate;
            if (tool == Tool::Number) {
                draft.number = 1; for (const auto& item : annotations) if (item.tool == Tool::Number) draft.number = std::max(draft.number, item.number + 1);
                addAnnotation(std::move(draft)); draft = {}; drag = Drag::None; invalidate(); return;
            }
        } else if (selected && contains(selection, point)) { clearTranslation(); drag = Drag::Move; dragOriginal = selection; }
        else { clearTranslation(); annotations.clear(); redoAnnotations.clear(); selected = true; selection = {point.x, point.y, point.x, point.y}; tool = Tool::Select; drag = Drag::Select; }
        SetCapture(window); hover = -1; layoutToolbar(); invalidate();
    }
    void pointerMove() {
        cursor = mouse(); if (busy) return;
        const int newHover = hitButton(cursor); if (newHover != hover) { hover = newHover; invalidate(); }
        switch (drag) {
        case Drag::Select: selection = pointRect(down, cursor); break;
        case Drag::Move: {
            const int dx = std::clamp<int>(cursor.x - down.x, -dragOriginal.left, snapshot.width - dragOriginal.right);
            const int dy = std::clamp<int>(cursor.y - down.y, -dragOriginal.top, snapshot.height - dragOriginal.bottom);
            selection = dragOriginal; OffsetRect(&selection, dx, dy); break;
        }
        case Drag::Resize:
            selection = dragOriginal;
            if (resizeEdges & 1) selection.left = cursor.x;
            if (resizeEdges & 2) selection.right = cursor.x;
            if (resizeEdges & 4) selection.top = cursor.y;
            if (resizeEdges & 8) selection.bottom = cursor.y;
            selection = normalized(selection); break;
        case Drag::Annotate: {
            POINT bounded{std::clamp<LONG>(cursor.x, selection.left, selection.right - 1), std::clamp<LONG>(cursor.y, selection.top, selection.bottom - 1)};
            if (draft.tool == Tool::Pen) {
                const POINT last = draft.points.back();
                if (draft.points.size() < 8192 && (std::abs(bounded.x - last.x) + std::abs(bounded.y - last.y) >= 2)) draft.points.push_back(bounded);
            } else { if (draft.points.size() == 1) draft.points.push_back(bounded); else draft.points.back() = bounded; }
            draft.box = pointRect(down, bounded); break;
        }
        default: if (!selected) {pixelCopied=false;updateHover();invalidate();} return;
        }
        layoutToolbar(); invalidate();
    }
    void pointerUp() {
        if (drag == Drag::None) return;
        pointerMove(); const Drag completed = drag; drag = Drag::None;
        if (GetCapture() == window) ReleaseCapture();
        if (completed == Drag::Annotate) {
            if (draft.tool == Tool::Pen || (draft.points.size() > 1 && (std::abs(draft.points.back().x - down.x) + std::abs(draft.points.back().y - down.y) >= 2)))
                addAnnotation(std::move(draft));
            draft = Annotation{};
        }
        if (completed == Drag::Select && std::abs(cursor.x - down.x) < 4 && std::abs(cursor.y - down.y) < 4 && validSize(hoverSelection)) selection = hoverSelection;
        selected = selection.right > selection.left && selection.bottom > selection.top; layoutToolbar(); invalidate();
    }
    void cursorShape() {
        const POINT point = mouse(); LPCWSTR cursorId = IDC_CROSS;
        if (hitButton(point) >= 0) cursorId = IDC_HAND;
        else if (busy) cursorId = IDC_WAIT;
        else if (const int edges = hitEdge(point)) {
            if (edges == 1 || edges == 2) cursorId = IDC_SIZEWE;
            else if (edges == 4 || edges == 8) cursorId = IDC_SIZENS;
            else if (edges == 5 || edges == 10) cursorId = IDC_SIZENWSE;
            else cursorId = IDC_SIZENESW;
        } else if (selected && contains(selection, point) && tool == Tool::Select) cursorId = IDC_SIZEALL;
        else if (tool == Tool::Text) cursorId = IDC_IBEAM;
        SetCursor(LoadCursorW(nullptr, cursorId));
    }
    void key(WPARAM value) {
        if (value == VK_ESCAPE) {
            if (edit) cancelText();
            else if (drag == Drag::Annotate) { drag = Drag::None; draft = Annotation{}; ReleaseCapture(); invalidate(); }
            else close(); return;
        }
        if (busy || drag != Drag::None) return;
        if((GetKeyState(VK_CONTROL)&0x8000)&&value=='C'){if(selected)copy();else copySample();return;}
        if (value == VK_SPACE && translatedSelected()) { originalPreview = true; invalidate(); return; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && value == 'Z') { perform((GetKeyState(VK_SHIFT) & 0x8000) ? Action::Redo : Action::Undo); return; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && value == 'Y') { perform(Action::Redo); return; }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && value == 'S') { if (selected) save(); return; }
        if (value == VK_RETURN && selected) { copy(); return; }
        if (value == 'O' && selected) { deliver(L"ocr"); return; }
        if (value == 'T' && selected) { translate(); return; }
        if (value == 'P' && selected) { pin(); return; }
        if (value == 'L' && selected) { scrolling(); return; }
        if (value == 'G' && selected) { record(L"gif"); return; }
        if (value == 'M' && selected) { record(L"mp4"); return; }
        if (value >= '1' && value <= '7' && selected) { perform(static_cast<Action>(static_cast<int>(Action::Rectangle) + value - '1')); return; }
        if (value == 'V') { tool = Tool::Select; layoutToolbar(); invalidate(); return; }
        if (value == 'C') {
            static constexpr DWORD colors[]{0xffff626d, 0xff58ddb2, 0xffffd461, 0xff79b6ff, 0xffffffff};
            auto found = std::find(std::begin(colors), std::end(colors), color);
            color = found != std::end(colors) && found + 1 < std::end(colors) ? *(found + 1) : colors[0]; invalidate(); return;
        }
        if (value == VK_OEM_4) thickness = std::max(1.0f, thickness - 1);
        if (value == VK_OEM_6) thickness = std::min(12.0f, thickness + 1);
        if (selected && (value == VK_LEFT || value == VK_RIGHT || value == VK_UP || value == VK_DOWN)) {
            clearTranslation();
            const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            int dx = value == VK_LEFT ? -step : value == VK_RIGHT ? step : 0, dy = value == VK_UP ? -step : value == VK_DOWN ? step : 0;
            dx = std::clamp(dx, -static_cast<int>(selection.left), snapshot.width - static_cast<int>(selection.right));
            dy = std::clamp(dy, -static_cast<int>(selection.top), snapshot.height - static_cast<int>(selection.bottom));
            OffsetRect(&selection, dx, dy); layoutToolbar();
        }
        invalidate();
    }
    void render();
};
void rounded(Gdiplus::Graphics& graphics, Gdiplus::Brush& brush, Gdiplus::RectF rect, float radius) {
    Gdiplus::GraphicsPath path; const float diameter = radius * 2;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90, 90);
    path.CloseFigure(); graphics.FillPath(&brush, &path);
}
void icon(Gdiplus::Graphics& g, const Button& button, bool active, float scale) {
    const float cx = (button.rect.left + button.rect.right) / 2.0f, cy = (button.rect.top + button.rect.bottom) / 2.0f, r = 9 * scale;
    Gdiplus::Color ink = button.action == Action::Cancel ? Gdiplus::Color(255, 224, 72, 76) :
        button.action == Action::Copy || active ? Gdiplus::Color(255, 20, 168, 82) : Gdiplus::Color(255, 45, 48, 52);
    Gdiplus::Pen pen(ink, 1.7f * scale); pen.SetLineJoin(Gdiplus::LineJoinRound);
    auto line = [&](float x, float y, float xx, float yy) { g.DrawLine(&pen, cx + x * scale, cy + y * scale, cx + xx * scale, cy + yy * scale); };
    switch (button.action) {
    case Action::Select: line(-7, -9, -7, 7); line(-7, -9, 7, 2); line(7, 2, 0, 3); line(0, 3, -3, 9); line(-3, 9, -7, 7); break;
    case Action::Rectangle: g.DrawRectangle(&pen, cx - r, cy - r * .7f, r * 2, r * 1.4f); break;
    case Action::Ellipse: g.DrawEllipse(&pen, cx - r, cy - r * .75f, r * 2, r * 1.5f); break;
    case Action::Number: {
        g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);
        Gdiplus::Font font(L"Segoe UI", 13 * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel); Gdiplus::SolidBrush brush(ink);
        Gdiplus::StringFormat center; center.SetAlignment(Gdiplus::StringAlignmentCenter); center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(L"1", 1, &font, Gdiplus::RectF(cx - r, cy - r, r * 2, r * 2), &center, &brush); break;
    }
    case Action::Arrow: line(-8, 8, 8, -8); line(-1, -8, 8, -8); line(8, -8, 8, 1); break;
    case Action::Pen: line(-8, 8, 6, -6); line(6, -6, 9, -3); line(9, -3, -5, 11); line(-8, 8, -9, 12); line(-9, 12, -5, 11); break;
    case Action::Text: line(-9, -8, 9, -8); line(0, -8, 0, 9); line(-5, 9, 5, 9); break;
    case Action::Mosaic:
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
            Gdiplus::SolidBrush cell(((x + y) & 1) ? ink : Gdiplus::Color(255, 151, 154, 158));
            g.FillRectangle(&cell, cx + (x * 6 - 2.5f) * scale, cy + (y * 6 - 2.5f) * scale, 5 * scale, 5 * scale);
        } break;
    case Action::Undo: g.DrawArc(&pen, cx - r, cy - r, r * 2, r * 2, 210, 290); line(-9, -7, -9, 0); line(-9, 0, -2, 0); break;
    case Action::Redo: g.DrawArc(&pen, cx - r, cy - r, r * 2, r * 2, 250, 290); line(9, -7, 9, 0); line(9, 0, 2, 0); break;
    case Action::Pin: line(-7, -8, 7, -8); line(-5, -8, -5, 1); line(5, -8, 5, 1); line(-5, 1, -9, 5);
        line(5, 1, 9, 5); line(-9, 5, 9, 5); line(0, 5, 0, 12); break;
    case Action::Long: g.DrawRectangle(&pen, cx - 8 * scale, cy - 10 * scale, 16 * scale, 20 * scale);
        line(-4, -5, 4, -5); line(-4, -1, 4, -1); line(0, 2, 0, 7); line(-3, 4, 0, 7); line(3, 4, 0, 7); break;
    case Action::Gif: case Action::Mp4: {
        g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);
        Gdiplus::SolidBrush dot(button.action == Action::Mp4 ? Gdiplus::Color(255, 255, 98, 109) : ink);
        if (button.action == Action::Mp4) g.FillEllipse(&dot, cx - 4 * scale, cy - 4 * scale, 8 * scale, 8 * scale);
        else {
            Gdiplus::Font font(L"Segoe UI", 13 * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat center; center.SetAlignment(Gdiplus::StringAlignmentCenter); center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(L"G", 1, &font, Gdiplus::RectF(cx - r, cy - r, r * 2, r * 2), &center, &dot);
        } break;
    }
    case Action::Copy:
        pen.SetWidth(2.4f * scale); line(-8, 0, -2, 6); line(-2, 6, 9, -7); break;
    case Action::Save:
        g.DrawRectangle(&pen, cx - r, cy - r, r * 2, r * 2); line(-4, -9, -4, -2); line(-4, -2, 5, -2); line(5, -2, 5, -9);
        line(-5, 9, -5, 3); line(-5, 3, 5, 3); line(5, 3, 5, 9); break;
    case Action::Ocr:
        line(-9, -3, -9, -9); line(-9, -9, -3, -9); line(3, -9, 9, -9); line(9, -9, 9, -3);
        line(-9, 3, -9, 9); line(-9, 9, -3, 9); line(3, 9, 9, 9); line(9, 9, 9, 3);
        line(-4, 4, 0, -5); line(0, -5, 4, 4); line(-2, 0, 2, 0); break;
    case Action::Translate: {
        Gdiplus::Font font(L"Segoe UI", 17 * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel); Gdiplus::SolidBrush brush(ink);
        Gdiplus::StringFormat format; format.SetAlignment(Gdiplus::StringAlignmentCenter); format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(L"译", 1, &font, Gdiplus::RectF(cx - 14 * scale, cy - 14 * scale, 28 * scale, 28 * scale), &format, &brush); break;
    }
    case Action::Cancel: line(-7, -7, 7, 7); line(-7, 7, 7, -7); break;
    }
}
void Overlay::render() {
    PAINTSTRUCT update{}; HDC target = BeginPaint(window, &update);
    if (!paint.dc) { EndPaint(window, &update); return; }
    BitBlt(paint.dc, 0, 0, snapshot.width, snapshot.height, dimmed.dc, 0, 0, SRCCOPY);
    const RECT preview = selected ? selection : hoverSelection;
    if (validSize(preview)) {
        const bool hasTranslation = selected && translatedSelected() && !originalPreview;
        BitBlt(paint.dc, preview.left, preview.top, preview.right - preview.left, preview.bottom - preview.top,
            hasTranslation ? translated.dc : snapshot.dc, hasTranslation ? 0 : preview.left, hasTranslation ? 0 : preview.top, SRCCOPY);
        const int saved = SaveDC(paint.dc); IntersectClipRect(paint.dc, preview.left, preview.top, preview.right, preview.bottom);
        {
            Gdiplus::Graphics graphics(paint.dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            if (selected) for (const auto& annotation : annotations) drawAnnotation(paint.dc, graphics, annotation);
            if (drag == Drag::Annotate) drawAnnotation(paint.dc, graphics, draft);
            graphics.Flush(Gdiplus::FlushIntentionSync);
        }
        RestoreDC(paint.dc, saved);
    }
    {
        Gdiplus::Graphics graphics(paint.dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        Gdiplus::SolidBrush panel(Gdiplus::Color(245, 22, 28, 39)), white(Gdiplus::Color(255, 232, 238, 246)), accent(Gdiplus::Color(255, 84, 222, 177));
        Gdiplus::Font font(L"Segoe UI", 12 * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat center; center.SetAlignment(Gdiplus::StringAlignmentCenter); center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        if (validSize(preview)) {
            Gdiplus::Pen border(Gdiplus::Color(255, 84, 222, 177), 1.5f * scale);
            graphics.DrawRectangle(&border, static_cast<float>(preview.left), static_cast<float>(preview.top),
                static_cast<float>(preview.right - preview.left), static_cast<float>(preview.bottom - preview.top));
            const LONG midx = (selection.left + selection.right) / 2, midy = (selection.top + selection.bottom) / 2;
            const POINT handles[]{{selection.left, selection.top}, {midx, selection.top}, {selection.right, selection.top}, {selection.left, midy},
                {selection.right, midy}, {selection.left, selection.bottom}, {midx, selection.bottom}, {selection.right, selection.bottom}};
            if (selected) for (POINT p : handles) graphics.FillRectangle(&accent, p.x - 3 * scale, p.y - 3 * scale, 6 * scale, 6 * scale);
            const std::wstring dimensions = (selected ? L"" : L"窗口 ") + std::to_wstring(preview.right - preview.left) + L" × " + std::to_wstring(preview.bottom - preview.top) + L" px";
            const float w = 178 * scale, h = 30 * scale;
            const float x = static_cast<float>(std::clamp<LONG>(preview.left, 0, std::max<LONG>(0, snapshot.width - static_cast<LONG>(w))));
            float y = preview.top - h - 8 * scale; if (y < 0) y = preview.top + 8 * scale;
            rounded(graphics, panel, {x, y, w, h}, 6 * scale);
            graphics.DrawString(dimensions.c_str(), -1, &font, Gdiplus::RectF(x, y, w, h), &center, &white);
            if (!buttons.empty()) {
                Gdiplus::SolidBrush shadow(Gdiplus::Color(55, 0, 0, 0)), toolbarPaper(Gdiplus::Color(255, 255, 255, 255));
                rounded(graphics, shadow, {static_cast<float>(toolbar.left + 2), static_cast<float>(toolbar.top + 4),
                    static_cast<float>(toolbar.right - toolbar.left), static_cast<float>(toolbar.bottom - toolbar.top)}, 5 * scale);
                rounded(graphics, toolbarPaper, {static_cast<float>(toolbar.left), static_cast<float>(toolbar.top),
                    static_cast<float>(toolbar.right - toolbar.left), static_cast<float>(toolbar.bottom - toolbar.top)}, 4 * scale);
                for (size_t i = 0; i < buttons.size(); ++i) {
                    const auto& button = buttons[i]; const bool isActive = button.action <= Action::Number && static_cast<int>(button.action) == static_cast<int>(tool);
                    if (i && buttons[i - 1].rect.top == button.rect.top &&
                        (button.action == Action::Pin || button.action == Action::Ocr || button.action == Action::Save || button.action == Action::Cancel)) {
                        Gdiplus::Pen divider(Gdiplus::Color(255, 215, 218, 220), scale);
                        const float dividerX = (buttons[i - 1].rect.right + button.rect.left) / 2.0f;
                        graphics.DrawLine(&divider, dividerX, button.rect.top + 8 * scale, dividerX, button.rect.bottom - 8 * scale);
                    }
                    if (isActive || static_cast<int>(i) == hover) {
                        Gdiplus::SolidBrush highlight(isActive ? Gdiplus::Color(255, 229, 246, 235) : Gdiplus::Color(255, 240, 242, 243));
                        rounded(graphics, highlight, {static_cast<float>(button.rect.left), static_cast<float>(button.rect.top),
                            static_cast<float>(button.rect.right - button.rect.left), static_cast<float>(button.rect.bottom - button.rect.top)}, 7 * scale);
                    }
                    icon(graphics, button, isActive, scale);
                }
                for (size_t i = 0; i < colorRects.size(); ++i) {
                    const auto& r = colorRects[i]; if (IsRectEmpty(&r)) continue;
                    Gdiplus::SolidBrush fill{Gdiplus::Color(paletteColors[i])};
                    graphics.FillEllipse(&fill, r.left + 4 * scale, r.top + 4 * scale, (r.right - r.left) - 8 * scale, (r.bottom - r.top) - 8 * scale);
                    if (paletteColors[i] == 0xffffffff) {
                        Gdiplus::Pen rim(Gdiplus::Color(255, 205, 209, 212), scale);
                        graphics.DrawEllipse(&rim, r.left + 4 * scale, r.top + 4 * scale, (r.right - r.left) - 8 * scale, (r.bottom - r.top) - 8 * scale);
                    }
                    if (color == paletteColors[i]) graphics.DrawEllipse(&border, r.left + scale, r.top + scale, (r.right - r.left) - 2 * scale, (r.bottom - r.top) - 2 * scale);
                }
                for (size_t i = 0; i < widthRects.size(); ++i) {
                    const auto& r = widthRects[i]; if (IsRectEmpty(&r)) continue;
                    if (thickness == paletteWidths[i]) {
                        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 229, 246, 235)); rounded(graphics, fill,
                            {static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top)}, 4 * scale);
                    }
                    Gdiplus::Pen sample(Gdiplus::Color(255, 50, 53, 56), paletteWidths[i] * scale); sample.SetStartCap(Gdiplus::LineCapRound); sample.SetEndCap(Gdiplus::LineCapRound);
                    graphics.DrawLine(&sample, r.left + 6 * scale, (r.top + r.bottom) / 2.0f, r.right - 6 * scale, (r.top + r.bottom) / 2.0f);
                }
                if (hover >= 0 && static_cast<size_t>(hover) < buttons.size()) {
                    const float tipWidth = 260 * scale, tipHeight = 28 * scale;
                    const float tipX = static_cast<float>(std::clamp<LONG>(buttons[hover].rect.left, 0, std::max<LONG>(0, snapshot.width - static_cast<LONG>(tipWidth))));
                    float tipY = toolbar.bottom + 7 * scale; if (tipY + tipHeight > snapshot.height) tipY = toolbar.top - tipHeight - 7 * scale;
                    rounded(graphics, panel, {tipX, tipY, tipWidth, tipHeight}, 6 * scale);
                    graphics.DrawString(buttons[hover].title, -1, &font, Gdiplus::RectF(tipX, tipY, tipWidth, tipHeight), &center, &white);
                }
            }
        }
        if (!busy && (!selected || drag == Drag::Select || drag == Drag::Resize)) {
            const POINT p = samplePoint();
            const float magW = 218 * scale, magH = 208 * scale;
            float mx = p.x + 24 * scale, my = p.y + 24 * scale;
            if (mx + magW > snapshot.width) mx = p.x - magW - 24 * scale;
            if (my + magH > snapshot.height) my = p.y - magH - 24 * scale;
            mx = std::clamp(mx, 0.0f, std::max(0.0f, snapshot.width - magW)); my = std::clamp(my, 0.0f, std::max(0.0f, snapshot.height - magH));
            Gdiplus::SolidBrush paper(Gdiplus::Color(255,255,255,255)),ink(Gdiplus::Color(255,38,43,51)),mutedInk(Gdiplus::Color(255,132,139,148));
            rounded(graphics,paper,{mx,my,magW,magH},8*scale);graphics.Flush(Gdiplus::FlushIntentionSync);
            const int sourceW = std::min(17, snapshot.width), sourceH = std::min(17, snapshot.height);
            const int sx = std::clamp<int>(p.x - sourceW / 2, 0, snapshot.width - sourceW), sy = std::clamp<int>(p.y - sourceH / 2, 0, snapshot.height - sourceH);
            const int dx = static_cast<int>(mx + 8 * scale), dy = static_cast<int>(my + 8 * scale), dw = static_cast<int>(magW - 16 * scale), dh = static_cast<int>(110 * scale);
            SetStretchBltMode(paint.dc, COLORONCOLOR); StretchBlt(paint.dc, dx, dy, dw, dh, snapshot.dc, sx, sy, sourceW, sourceH, SRCCOPY);
            const float px = dx + (std::clamp<LONG>(p.x, 0, snapshot.width - 1) - sx + .5f) * dw / sourceW;
            const float py = dy + (std::clamp<LONG>(p.y, 0, snapshot.height - 1) - sy + .5f) * dh / sourceH;
            Gdiplus::Pen cross(Gdiplus::Color(255, 84, 222, 177), scale);
            graphics.DrawLine(&cross, px, static_cast<float>(dy), px, static_cast<float>(dy + dh));
            graphics.DrawLine(&cross, static_cast<float>(dx), py, static_cast<float>(dx + dw), py);
            const auto position=L"坐标   "+std::to_wstring(p.x+screen.left)+L", "+std::to_wstring(p.y+screen.top);
            const auto value=L"色值   "+sampleHex();
            graphics.DrawString(position.c_str(),-1,&font,Gdiplus::RectF(mx+8*scale,my+121*scale,magW-16*scale,23*scale),&center,&ink);
            graphics.DrawString(value.c_str(),-1,&font,Gdiplus::RectF(mx+8*scale,my+146*scale,magW-16*scale,23*scale),&center,&ink);
            const wchar_t* hint=pixelCopied?L"色值已复制":L"Ctrl+C 复制色值";
            graphics.DrawString(hint,-1,&font,Gdiplus::RectF(mx+8*scale,my+174*scale,magW-16*scale,23*scale),&center,&mutedInk);
        }
        if (!selected || busy || !translationStatus.empty()) {
            POINT point = cursor; if (!point.x && !point.y) point = mouse();
            POINT physical{point.x + screen.left, point.y + screen.top}; MONITORINFO monitor{sizeof(MONITORINFO)};
            GetMonitorInfoW(MonitorFromPoint(physical, MONITOR_DEFAULTTONEAREST), &monitor);
            const float w = (translationFailed ? 600 : 360) * scale, h = (translationFailed ? 96 : 48) * scale;
            const float x = (monitor.rcMonitor.left + monitor.rcMonitor.right) / 2.0f - screen.left - w / 2;
            const float y = monitor.rcMonitor.top - screen.top + 34 * scale;
            rounded(graphics, panel, {x, y, w, h}, 12 * scale);
            const wchar_t* status = translationTask ? translationStatus.c_str() : busy ? L"正在保存 PNG · Esc 取消" :
                !translationStatus.empty() ? translationStatus.c_str() : L"单击选窗口 · 拖动框选 · 右键 / Esc 退出";
            graphics.DrawString(status, -1, &font, Gdiplus::RectF(x, y, w, h), &center, &white);
        }
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    BitBlt(target, update.rcPaint.left, update.rcPaint.top, update.rcPaint.right - update.rcPaint.left,
        update.rcPaint.bottom - update.rcPaint.top, paint.dc, update.rcPaint.left, update.rcPaint.top, SRCCOPY);
    EndPaint(window, &update);
}
LRESULT CALLBACK overlayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE && !image_tools_detail::attachWindowState<Overlay>(window, lparam)) return FALSE;
    const auto dispatch = image_tools_detail::windowState<Overlay>(window);
    auto* state = dispatch.get();
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: state->render(); return 0;
    case WM_LBUTTONDOWN: state->eventCursor(lparam); state->pointerDown(); return 0;
    case WM_MOUSEMOVE: state->eventCursor(lparam); state->pointerMove(); return 0;
    case WM_LBUTTONUP: state->eventCursor(lparam); state->pointerUp(); return 0;
    case WM_RBUTTONDOWN: if (!state->busy) state->back(); return 0;
    case WM_SETCURSOR: state->cursorShape(); return TRUE;
    case WM_KEYDOWN: state->key(wparam); return 0;
    case WM_KEYUP:
        if (wparam == VK_SPACE && state->originalPreview) { state->originalPreview = false; state->invalidate(); }
        return 0;
    case WM_CTLCOLOREDIT:
        SetBkColor(reinterpret_cast<HDC>(wparam), RGB(26, 32, 43));
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB((state->color >> 16) & 255, (state->color >> 8) & 255, state->color & 255));
        return reinterpret_cast<LRESULT>(state->editBrush);
    case WM_CAPTURECHANGED:
        if (state->drag != Drag::None) { state->drag = Drag::None; state->draft = Annotation{}; state->layoutToolbar(); state->invalidate(); } return 0;
    case commitTextMessage: if (wparam == state->editGeneration) state->commitText(); return 0;
    case translationFinished: state->finishTranslation(static_cast<UINT_PTR>(wparam)); return 0;
    case saveFinished:
        if (state->saveTask && wparam == state->saveTask->id) {
            state->busy = false; state->saveTask.reset();
            if (lparam) state->close();
            else { state->invalidate(); MessageBoxW(window, L"PNG 保存失败，请检查目标目录权限和可用磁盘空间。", L"桌面提效", MB_OK | MB_ICONWARNING); }
        } return 0;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED: case WM_CLOSE: state->close(); return 0;
    case WM_ACTIVATEAPP:
        if (!wparam && !state->busy && !state->modalDialog) state->close(false);
        return 0;
    case WM_NCDESTROY: {
        HWND expected = window; activeWindow.compare_exchange_strong(expected, nullptr);
        state->window = nullptr;
        if (state->saveTask) state->saveTask->cancelled = true;
        if (state->translationTask) { state->translationTask->cancelled = true; state->translationTask.reset(); }
        state->retireSurfaces();
        image_tools_detail::detachWindowState<Overlay>(window);
        return DefWindowProcW(window, message, wparam, lparam);
    }
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}
}
HBITMAP captureRegion(RECT physicalScreenRect) {
    DpiScope dpi; RECT requested = normalized(physicalScreenRect);
    if (!validSize(requested)) return nullptr;
    RECT region{}, desktop = desktopRect();
    if (!IntersectRect(&region, &requested, &desktop) || !validSize(region)) return nullptr;
    BitmapSurface output; if (!output.create(region.right - region.left, region.bottom - region.top)) return nullptr;
    HDC screen = GetDC(nullptr); if (!screen) return nullptr;
    const BOOL captured = BitBlt(output.dc, 0, 0, output.width, output.height, screen, region.left, region.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen); if (!captured) return nullptr; GdiFlush(); return output.release();
}
bool saveBitmapPng(HBITMAP bitmap, const std::filesystem::path& path) {
    try { return writePng(bitmap, path); } catch (...) { return false; }
}
bool copyBitmapToClipboard(HBITMAP bitmap) {
    if (!bitmap) return false;
    BITMAP dimensions{};
    if (!GetObjectW(bitmap, sizeof(dimensions), &dimensions) || dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0 ||
        static_cast<std::uint64_t>(dimensions.bmWidth) * dimensions.bmHeight > maxPixels) return false;
    const size_t pixels = static_cast<size_t>(dimensions.bmWidth) * dimensions.bmHeight * 4;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels); if (!memory) return false;
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory)); if (!header) { GlobalFree(memory); return false; }
    *header = {}; header->biSize = sizeof(BITMAPINFOHEADER); header->biWidth = dimensions.bmWidth; header->biHeight = dimensions.bmHeight;
    header->biPlanes = 1; header->biBitCount = 32; header->biCompression = BI_RGB; header->biSizeImage = static_cast<DWORD>(pixels);
    HDC dc = GetDC(nullptr);
    const bool converted = dc && GetDIBits(dc, bitmap, 0, dimensions.bmHeight, header + 1, reinterpret_cast<BITMAPINFO*>(header), DIB_RGB_COLORS) == dimensions.bmHeight;
    if (dc) ReleaseDC(nullptr, dc); GlobalUnlock(memory);
    if (!converted) { GlobalFree(memory); return false; }
    // EmptyClipboard with a null HWND leaves no owner, so use a caller-owned window.
    HWND clipboardOwner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!clipboardOwner) { GlobalFree(memory); return false; }
    bool success{};
    if (OpenClipboard(clipboardOwner)) {
        if (EmptyClipboard() && SetClipboardData(CF_DIB, memory)) { success = true; memory = nullptr; }
        CloseClipboard();
    }
    DestroyWindow(clipboardOwner); if (memory) GlobalFree(memory); return success;
}
bool captureActive() { return activeWindow.load() != nullptr || scrollingCaptureActive(); }
capture_detail::HoverInfo capture_detail::hoverInfo(){
    HoverInfo result;auto state=image_tools_detail::windowState<Overlay>(activeWindow.load());
    if(!state||!state->snapshot.pixels)return result;
    result.region=state->selected?state->selection:state->hoverSelection;OffsetRect(&result.region,state->screen.left,state->screen.top);
    result.pixel=state->samplePoint();result.pixel.x+=state->screen.left;result.pixel.y+=state->screen.top;
    result.rgb=state->sampleRgb();result.selected=state->selected;return result;
}
void beginCapture(HWND owner, CaptureCallback callback, RegionCallback recordingCallback, InlineTranslation translation) {
    if (scrollingCaptureActive()) { beginScrollingCapture(owner, {}, {}); return; }
    if (captureActive()) { SetForegroundWindow(activeWindow.load()); return; }
    DpiScope dpi; auto state = std::make_shared<Overlay>();
    state->owner = owner; state->ownerWasVisible = owner && IsWindowVisible(owner);
    state->previousForeground = GetForegroundWindow(); state->callback = std::move(callback); state->recordingCallback = std::move(recordingCallback);
    state->translation = std::move(translation);
    const bool ownerWasVisible = state->ownerWasVisible;
    if (ownerWasVisible) ShowWindow(owner, SW_HIDE);
    state->screen = desktopRect();
    auto recover = [&] {
        if (ownerWasVisible && IsWindow(owner)) ShowWindow(owner, SW_SHOW);
        MessageBoxW(owner, L"无法捕获桌面。请检查当前桌面会话或减少显示器总分辨率。", L"桌面提效", MB_OK | MB_ICONWARNING);
    };
    state->memory = image_tools_detail::reserveMemory(image_tools_detail::captureMemoryRequirement(state->screen.right - state->screen.left, state->screen.bottom - state->screen.top));
    if (!state->memory) {
        if (ownerWasVisible && IsWindow(owner)) ShowWindow(owner, SW_SHOW);
        MessageBoxW(owner, L"当前截图与贴图超出 512 MiB 图像预算（含主窗口及编码预留）。请关闭贴图或降低虚拟桌面总分辨率。", L"桌面提效", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!validSize(state->screen) || !state->snapshot.create(state->screen.right - state->screen.left, state->screen.bottom - state->screen.top) ||
        !state->dimmed.create(state->snapshot.width, state->snapshot.height) || !state->paint.create(state->snapshot.width, state->snapshot.height)) { recover(); return; }
    HDC screen = GetDC(nullptr);
    const bool captured = screen && BitBlt(state->snapshot.dc, 0, 0, state->snapshot.width, state->snapshot.height, screen, state->screen.left, state->screen.top, SRCCOPY | CAPTUREBLT);
    if (screen) ReleaseDC(nullptr, screen); if (!captured) { recover(); return; } GdiFlush();
    for (size_t i = 0, count = static_cast<size_t>(state->snapshot.width) * state->snapshot.height; i < count; ++i) {
        const DWORD pixel = state->snapshot.pixels[i];
        const DWORD red = ((pixel >> 16) & 255) * 42 / 100 + 4, green = ((pixel >> 8) & 255) * 42 / 100 + 6, blue = (pixel & 255) * 42 / 100 + 10;
        state->dimmed.pixels[i] = 0xff000000 | (red << 16) | (green << 8) | blue;
    }
    Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&state->gdiplus, &startup, nullptr) != Gdiplus::Ok) { recover(); return; }
    static ATOM windowClass = [] {
        WNDCLASSEXW type{sizeof(type)}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = overlayProc;
        type.lpszClassName = L"DeskEfficiencyCaptureOverlay"; type.hCursor = LoadCursorW(nullptr, IDC_CROSS); return RegisterClassExW(&type);
    }();
    if (!windowClass) { recover(); return; }
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DeskEfficiencyCaptureOverlay", L"截图 · 桌面提效", WS_POPUP,
        state->screen.left, state->screen.top, state->snapshot.width, state->snapshot.height, nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) { recover(); return; }
    state->scale = std::clamp(GetDpiForWindow(window) / 96.0f, 1.0f, 2.5f); activeWindow = window;
    state->cursor = state->mouse(); state->cursorKnown = true; state->updateHover();
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window); SetFocus(window); UpdateWindow(window);
}
}
