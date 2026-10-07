#include "image_tools.hpp"
#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <limits>
#pragma comment(lib, "dwmapi.lib")

namespace desk {
namespace image_tools_detail {
std::atomic<std::uint64_t> reservedImageBytes{};
struct MemoryReservation {
    std::uint64_t bytes{};
    explicit MemoryReservation(std::uint64_t count) : bytes(count) {}
    ~MemoryReservation() { reservedImageBytes.fetch_sub(bytes); }
};
MemoryLease reserveMemory(std::uint64_t bytes) {
    if (!bytes || bytes > imageWorkingBytes) return {};
    auto current = reservedImageBytes.load();
    do {
        if (current > imageWorkingBytes || bytes > imageWorkingBytes - current) return {};
    } while (!reservedImageBytes.compare_exchange_weak(current, current + bytes));
    try { return std::make_shared<MemoryReservation>(bytes); }
    catch (...) { reservedImageBytes.fetch_sub(bytes); return {}; }
}
std::uint64_t captureMemoryRequirement(int width, int height) {
    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > imagePixelBudget) return UINT64_MAX;
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    const auto mosaicPixels = ((static_cast<std::uint64_t>(width) + 11) / 12) * ((static_cast<std::uint64_t>(height) + 11) / 12);
    // Three interactive surfaces + cropped output + WIC/clipboard copy. This
    // reservation lasts through any detached encoder using the output bitmap.
    return pixels * 4 * 5 + mosaicPixels * 4;
}
}
std::uint64_t imageToolReservedBytes() { return image_tools_detail::reservedImageBytes.load(); }
namespace {
struct DpiScope {
    DPI_AWARENESS_CONTEXT old{SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    ~DpiScope() { if (old) SetThreadDpiAwarenessContext(old); }
};
struct Match { double error{}, ratio{}, variance{}; };
Match matchRows(const PixelImage& a, const PixelImage& b, int shift, int rows, int xSamples, int ySamples) {
    double sum{}, square{}, mean{}; unsigned count{}, close{};
    const int xs = std::max(1, a.width / xSamples), ys = std::max(1, rows / ySamples);
    for (int y = 0; y < rows; y += ys) for (int x = 0; x < a.width; x += xs) {
        const auto aa = a.pixels[static_cast<size_t>(y + shift) * a.width + x];
        const auto bb = b.pixels[static_cast<size_t>(y) * b.width + x];
        const int r = static_cast<int>((aa >> 16) & 255) - static_cast<int>((bb >> 16) & 255);
        const int g = static_cast<int>((aa >> 8) & 255) - static_cast<int>((bb >> 8) & 255);
        const int blue = static_cast<int>(aa & 255) - static_cast<int>(bb & 255);
        sum += (std::abs(r) + std::abs(g) + std::abs(blue)) / 3.0;
        close += std::abs(r) <= 8 && std::abs(g) <= 8 && std::abs(blue) <= 8;
        const double light = ((bb >> 16) & 255) * .299 + ((bb >> 8) & 255) * .587 + (bb & 255) * .114;
        mean += light; square += light * light; ++count;
        if (count >= 64 && sum / count > 12) return {sum / count, 0, 0};
    }
    if (!count) return {255, 0, 0};
    return {sum / count, static_cast<double>(close) / count, square / count - (mean / count) * (mean / count)};
}
std::atomic<std::uint64_t> pinnedPixels{};
std::atomic<unsigned> pinCount{};
std::atomic<HWND> scrollWindow{};
std::atomic<UINT_PTR> saveSerial{1};
constexpr UINT saveDone = WM_APP + 0x571;
constexpr int toolbarHeight = 72;
constexpr int copyId = 2101, saveId = 2102, ocrId = 2103, translateId = 2104;
constexpr int finishId = 2201, pauseId = 2202, cancelId = 2203;
struct AsyncSave { std::atomic<bool> cancelled{}; UINT_PTR serial{}; image_tools_detail::MemoryLease memory; };
HBITMAP cloneBitmap(HBITMAP bitmap) { return static_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION)); }
void text(HDC dc, const std::wstring& value, RECT box, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(224, 233, 246)); DrawTextW(dc, value.c_str(), -1, &box, flags);
}
HFONT uiFont(int height = 15) {
    return CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
void darkCaption(HWND window) { const BOOL dark = TRUE; DwmSetWindowAttribute(window, 20, &dark, sizeof(dark)); }

struct PinState {
    HWND window{}, owner{}, buttons[4]{};
    HBITMAP bitmap{}; HDC source{}; HGDIOBJ oldBitmap{}; HFONT font{};
    int width{}, height{}, offsetX{}, offsetY{};
    double zoom{1}; BYTE alpha{255}; float scale{1};
    bool dragging{};
    POINT dragStart{}; RECT dragWindow{};
    std::uint64_t budget{};
    image_tools_detail::MemoryLease memory;
    CaptureCallback callback; std::shared_ptr<AsyncSave> saveTask;
    ~PinState() {
        if (saveTask) saveTask->cancelled = true;
        if (source && oldBitmap) SelectObject(source, oldBitmap);
        if (source) DeleteDC(source);
        if (bitmap) DeleteObject(bitmap);
        if (font) DeleteObject(font);
        if (budget) { pinnedPixels.fetch_sub(budget); pinCount.fetch_sub(1); }
    }
    RECT imageArea() const {
        RECT r{}; GetClientRect(window, &r); r.bottom = std::max<LONG>(0, r.bottom - static_cast<LONG>(toolbarHeight * scale)); return r;
    }
    void update() {
        RECT client{}; GetClientRect(window, &client);
        const int pad = static_cast<int>(8 * scale), top = client.bottom - static_cast<int>(toolbarHeight * scale) + pad;
        const int bw = std::max<int>(30, (client.right - pad * 5) / 4), bh = static_cast<int>(28 * scale);
        for (int i = 0; i < 4; ++i) SetWindowPos(buttons[i], nullptr, pad + i * (bw + pad), top, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
        RECT area = imageArea();
        const int contentW = static_cast<int>(std::min<double>(INT_MAX / 2, width * zoom));
        const int contentH = static_cast<int>(std::min<double>(INT_MAX / 2, height * zoom));
        offsetX = std::clamp(offsetX, 0, std::max(0, contentW - static_cast<int>(area.right)));
        offsetY = std::clamp(offsetY, 0, std::max(0, contentH - static_cast<int>(area.bottom)));
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, contentW - 1), static_cast<UINT>(area.right), offsetX};
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, contentH - 1), static_cast<UINT>(area.bottom), offsetY};
        SetScrollInfo(window, SB_HORZ, &horizontal, TRUE); SetScrollInfo(window, SB_VERT, &vertical, TRUE);
        const std::wstring title = L"贴图 · " + std::to_wstring(width) + L" × " + std::to_wstring(height) + L" · " +
            std::to_wstring(static_cast<int>(zoom * 100)) + L"% · 不透明度 " + std::to_wstring(alpha * 100 / 255) + L"%";
        SetWindowTextW(window, title.c_str()); InvalidateRect(window, nullptr, FALSE);
    }
    void resizeForZoom() {
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        const int maxW = std::max(240L, monitor.rcWork.right - monitor.rcWork.left - 48);
        const int maxH = std::max(180L, monitor.rcWork.bottom - monitor.rcWork.top - 48);
        const int w = std::clamp(static_cast<int>(std::min<double>(maxW, width * zoom + 32 * scale)), std::min(maxW, static_cast<int>(440 * scale)), maxW);
        const int h = std::clamp(static_cast<int>(std::min<double>(maxH, height * zoom + 112 * scale)), std::min(maxH, static_cast<int>(220 * scale)), maxH);
        SetWindowPos(window, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); update();
    }
    void paint() {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps);
        RECT client{}; GetClientRect(window, &client); HBRUSH background = CreateSolidBrush(RGB(20, 27, 38));
        FillRect(dc, &client, background); DeleteObject(background);
        RECT area = imageArea(); const int saved = SaveDC(dc); IntersectClipRect(dc, 0, 0, area.right, area.bottom);
        const int w = static_cast<int>(width * zoom), h = static_cast<int>(height * zoom);
        const int x = w < area.right ? (area.right - w) / 2 : -offsetX, y = h < area.bottom ? (area.bottom - h) / 2 : -offsetY;
        SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, x, y, w, h, source, 0, 0, width, height, SRCCOPY); RestoreDC(dc, saved);
        HGDIOBJ oldFont = SelectObject(dc, font);
        RECT hint{static_cast<LONG>(8 * scale), client.bottom - static_cast<LONG>(30 * scale), client.right - 8, client.bottom};
        text(dc, L"拖动移动 · 滚轮缩放 · Shift+滚轮透明 · Ctrl+滚轮滚动", hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont); EndPaint(window, &ps);
    }
    void copy() {
        if (!copyBitmapToClipboard(bitmap)) MessageBoxW(window, L"剪贴板正被占用，复制失败，请重试。", L"贴图", MB_OK | MB_ICONWARNING);
    }
    void deliver(const wchar_t* action) {
        if (!callback) { MessageBoxW(window, L"当前窗口没有文字处理回调。", L"贴图", MB_OK | MB_ICONINFORMATION); return; }
        auto deliveryMemory = image_tools_detail::reserveMemory(budget * sizeof(DWORD));
        if (!deliveryMemory) { MessageBoxW(window, L"处理副本超出图像内存预算，请关闭其他大图后重试。", L"贴图", MB_OK | MB_ICONINFORMATION); return; }
        HBITMAP owned = cloneBitmap(bitmap);
        if (!owned) { MessageBoxW(window, L"无法分配图片副本，请关闭其他大图后重试。", L"贴图", MB_OK | MB_ICONWARNING); return; }
        auto completed = callback; try { completed(owned, action); } catch (...) { /* Host owns the transferred bitmap. */ }
    }
    void save() {
        if (saveTask) return;
        wchar_t filename[32768] = L"贴图.png"; OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window; dialog.lpstrFile = filename; dialog.nMaxFile = 32768;
        dialog.lpstrFilter = L"PNG 图像\0*.png\0\0"; dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        const BOOL chosen = GetSaveFileNameW(&dialog);
        if (!window || !IsWindow(window) || !chosen) return;
        auto exportMemory = image_tools_detail::reserveMemory(budget * sizeof(DWORD) * 2);
        if (!exportMemory) { MessageBoxW(window, L"保存副本超出图像内存预算，请关闭其他贴图或截图后重试。", L"贴图", MB_OK | MB_ICONWARNING); return; }
        HBITMAP owned = cloneBitmap(bitmap);
        if (!owned) { MessageBoxW(window, L"无法分配导出图片副本，请关闭其他大图后重试。", L"贴图", MB_OK | MB_ICONWARNING); return; }
        std::shared_ptr<AsyncSave> task;
        try { task = std::make_shared<AsyncSave>(); }
        catch (...) { DeleteObject(owned); MessageBoxW(window, L"保存任务内存不足，请重试。", L"贴图", MB_OK | MB_ICONWARNING); return; }
        task->serial = saveSerial.fetch_add(1); task->memory = std::move(exportMemory); saveTask = task;
        const HWND destination = window; const std::filesystem::path path(filename);
        EnableWindow(buttons[1], FALSE);
        try {
            std::thread([task, owned, path, destination] {
                auto temporary = path; temporary += L".desk-pin-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(task->serial) + L".tmp";
                bool success{};
                try {
                    success = !task->cancelled.load() && saveBitmapPng(owned, temporary);
                    if (success) success = !task->cancelled.load() && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                } catch (...) { success = false; }
                DeleteFileW(temporary.c_str()); DeleteObject(owned);
                if (!task->cancelled.load()) PostMessageW(destination, saveDone, task->serial, success ? 1 : 0);
            }).detach();
        } catch (...) {
            DeleteObject(owned); saveTask.reset(); EnableWindow(buttons[1], TRUE);
            MessageBoxW(window, L"无法启动保存任务，请重试。", L"贴图", MB_OK | MB_ICONWARNING);
        }
    }
    void scroll(int bar, WPARAM command) {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(window, bar, &info);
        int pos = info.nPos;
        switch (LOWORD(command)) {
        case SB_LINEUP: pos -= 40; break; case SB_LINEDOWN: pos += 40; break;
        case SB_PAGEUP: pos -= info.nPage; break; case SB_PAGEDOWN: pos += info.nPage; break;
        case SB_THUMBTRACK: pos = info.nTrackPos; break; case SB_TOP: pos = 0; break; case SB_BOTTOM: pos = info.nMax; break;
        default: return;
        }
        if (bar == SB_VERT) offsetY = pos; else offsetX = pos; update();
    }
};
LRESULT CALLBACK pinProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE && !image_tools_detail::attachWindowState<PinState>(window, lparam)) return FALSE;
    const auto dispatch = image_tools_detail::windowState<PinState>(window);
    auto* state = dispatch.get();
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        state->scale = GetDpiForWindow(window) / 96.0f; state->font = uiFont(static_cast<int>(13 * state->scale));
        const wchar_t* labels[]{L"复制 Enter", L"保存 Ctrl+S", L"识字 O", L"翻译 T"};
        const int ids[]{copyId, saveId, ocrId, translateId};
        for (int i = 0; i < 4; ++i) {
            state->buttons[i] = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(state->buttons[i], WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
        }
        darkCaption(window); SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA); return 0;
    }
    case WM_SIZE: state->update(); return 0;
    case WM_PAINT: state->paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) {
            state->alpha = static_cast<BYTE>(std::clamp<int>(state->alpha + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 16 : -16), 48, 255));
            SetLayeredWindowAttributes(window, 0, state->alpha, LWA_ALPHA); state->update();
        } else if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) {
            state->offsetY -= GET_WHEEL_DELTA_WPARAM(wparam); state->update();
        } else { state->zoom = std::clamp(state->zoom * (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1.1 : 1 / 1.1), .01, 8.0); state->resizeForZoom(); }
        return 0;
    case WM_LBUTTONDOWN:
        state->dragging = true; state->dragStart = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ClientToScreen(window, &state->dragStart);
        GetWindowRect(window, &state->dragWindow); SetCapture(window); return 0;
    case WM_MOUSEMOVE:
        if (state->dragging) {
            POINT now{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ClientToScreen(window, &now);
            SetWindowPos(window, nullptr, state->dragWindow.left + now.x - state->dragStart.x,
                state->dragWindow.top + now.y - state->dragStart.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER); }
        return 0;
    case WM_LBUTTONUP: state->dragging = false; if (GetCapture() == window) ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: state->dragging = false; return 0;
    case WM_LBUTTONDBLCLK: case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_HSCROLL: state->scroll(SB_HORZ, wparam); return 0;
    case WM_VSCROLL: state->scroll(SB_VERT, wparam); return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) { case copyId: state->copy(); break; case saveId: state->save(); break;
        case ocrId: state->deliver(L"ocr"); break; case translateId: state->deliver(L"translate"); break; }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        if (wparam == VK_RETURN || ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'C')) state->copy();
        else if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'S') state->save();
        else if (wparam == 'O') state->deliver(L"ocr"); else if (wparam == 'T') state->deliver(L"translate");
        else if (wparam == VK_UP) state->scroll(SB_VERT, SB_LINEUP); else if (wparam == VK_DOWN) state->scroll(SB_VERT, SB_LINEDOWN);
        else if (wparam == VK_PRIOR) state->scroll(SB_VERT, SB_PAGEUP); else if (wparam == VK_NEXT) state->scroll(SB_VERT, SB_PAGEDOWN);
        return 0;
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, copyId, L"复制图片 Enter"); AppendMenuW(menu, MF_STRING, saveId, L"保存 PNG Ctrl+S");
        AppendMenuW(menu, MF_STRING, ocrId, L"识别文字 O"); AppendMenuW(menu, MF_STRING, translateId, L"原图翻译 T");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, 2199, L"关闭贴图 Esc");
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; if (p.x == -1 && p.y == -1) GetCursorPos(&p);
        const UINT action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr); DestroyMenu(menu);
        if (action == 2199) DestroyWindow(window); else if (action) SendMessageW(window, WM_COMMAND, action, 0); return 0;
    }
    case saveDone:
        if (state->saveTask && state->saveTask->serial == wparam) {
            state->saveTask.reset(); EnableWindow(state->buttons[1], TRUE);
            if (!lparam) MessageBoxW(window, L"PNG 保存失败，请检查目录权限和剩余空间。", L"贴图", MB_OK | MB_ICONWARNING);
        } return 0;
    case WM_NCDESTROY:
        state->window = nullptr;
        if (state->saveTask) state->saveTask->cancelled = true;
        image_tools_detail::detachWindowState<PinState>(window);
        return DefWindowProcW(window, message, wparam, lparam);
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

struct ScrollState {
    HWND window{}, owner{}, buttons[3]{}; RECT region{}; HFONT font{};
    VerticalStitcher stitcher; CaptureCallback callback;
    image_tools_detail::MemoryLease memory;
    std::wstring status{L"在选区内向下滚动，每次保留至少一半画面。"};
    bool paused{}, capped{}; ULONGLONG started{GetTickCount64()}; unsigned samples{};
    ~ScrollState() { if (font) DeleteObject(font); stitcher.reset(); memory.reset(); }
    void update() {
        SetWindowTextW(buttons[1], paused ? L"继续采样" : L"暂停采样"); EnableWindow(buttons[1], !capped); InvalidateRect(window, nullptr, FALSE);
    }
    void stop(const wchar_t* reason, bool limit = false) { paused = true; capped = limit; KillTimer(window, 1); status = reason; update(); }
    void sample() {
        if (paused) return;
        if (++samples > 1800 || GetTickCount64() - started > 15 * 60 * 1000) { stop(L"已达到 15 分钟采样预算，请完成或取消。", true); return; }
        RECT controls{}, intersection{}; GetWindowRect(window, &controls);
        const bool overlaps = IntersectRect(&intersection, &region, &controls) != FALSE;
        if (overlaps) { ShowWindow(window, SW_HIDE); DwmFlush(); }
        HBITMAP bitmap = captureRegion(region); PixelImage pixels;
        const bool loaded = bitmap && bitmapPixels(bitmap, pixels); if (bitmap) DeleteObject(bitmap);
        if (overlaps) ShowWindow(window, SW_SHOWNOACTIVATE);
        if (!loaded) { stop(L"屏幕采样失败，已暂停。请检查桌面会话。"); return; }
        StitchResult result;
        try { result = stitcher.append(pixels); } catch (...) { stop(L"图片内存不足，已保留成功拼接部分。", true); return; }
        switch (result.status) {
        case StitchStatus::Started: case StitchStatus::Appended:
            status = L"已拼接 " + std::to_wstring(stitcher.image().width) + L" × " + std::to_wstring(stitcher.image().height) + L" px · 可继续向下滚动"; update(); break;
        case StitchStatus::NoChange: break;
        case StitchStatus::LimitReached: stop(L"已达到 32 MP / 32768 像素高度预算，请完成。", true); break;
        default: stop(L"没有可信重叠，已暂停。滚回一点，再继续采样。"); break;
        }
    }
    void finish() {
        HBITMAP bitmap = bitmapFromPixels(stitcher.image());
        if (!bitmap) { stop(L"无法生成长图，请关闭其他大图后重试。"); return; }
        HWND host = owner; auto completed = std::move(callback);
        stitcher.reset(); memory.reset();
        DestroyWindow(window); pinImage(host, bitmap, std::move(completed));
    }
    void toggle() {
        if (capped) return;
        paused = !paused;
        if (paused) { KillTimer(window, 1); status = L"已暂停。继续后，在选区内向下滚动。"; }
        else { SetTimer(window, 1, 650, nullptr); status = L"继续采样：每次滚动保留至少一半画面。"; }
        update();
    }
    void paint() {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps); RECT client{}; GetClientRect(window, &client);
        HBRUSH brush = CreateSolidBrush(RGB(22, 30, 43)); FillRect(dc, &client, brush); DeleteObject(brush);
        auto old = SelectObject(dc, font); RECT title{14, 8, client.right - 14, 34};
        const auto heading = L"长截图 · " + std::to_wstring(stitcher.image().width) + L" × " + std::to_wstring(stitcher.image().height) + L" px";
        text(dc, heading, title); RECT hint{14, 38, client.right - 14, 90};
        text(dc, status, hint, DT_LEFT | DT_WORDBREAK); SelectObject(dc, old); EndPaint(window, &ps);
    }
};
LRESULT CALLBACK scrollProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE && !image_tools_detail::attachWindowState<ScrollState>(window, lparam)) return FALSE;
    const auto dispatch = image_tools_detail::windowState<ScrollState>(window);
    auto* state = dispatch.get();
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        state->font = uiFont(15); const wchar_t* labels[]{L"完成并贴图 Enter", L"暂停采样", L"取消 Esc"}; const int ids[]{finishId, pauseId, cancelId};
        for (int i = 0; i < 3; ++i) {
            state->buttons[i] = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                14 + i * 134, 96, 126, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(state->buttons[i], WM_SETFONT, reinterpret_cast<WPARAM>(state->font), FALSE);
        }
        if (!RegisterHotKey(window, finishId, MOD_NOREPEAT, VK_RETURN)) SetWindowTextW(state->buttons[0], L"完成并贴图");
        if (!RegisterHotKey(window, cancelId, MOD_NOREPEAT, VK_ESCAPE)) SetWindowTextW(state->buttons[2], L"取消");
        SetTimer(window, 1, 650, nullptr); darkCaption(window); return 0;
    }
    case WM_TIMER: if (wparam == 1) state->sample(); return 0;
    case WM_PAINT: state->paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND: case WM_HOTKEY:
        if (LOWORD(wparam) == finishId) state->finish();
        else if (LOWORD(wparam) == pauseId) state->toggle();
        else if (LOWORD(wparam) == cancelId) DestroyWindow(window);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_RETURN) state->finish(); else if (wparam == VK_ESCAPE) DestroyWindow(window);
        else if (wparam == VK_SPACE) state->toggle(); return 0;
    case WM_CLOSE: case WM_DISPLAYCHANGE: DestroyWindow(window); return 0;
    case WM_NCDESTROY: {
        KillTimer(window, 1); UnregisterHotKey(window, finishId); UnregisterHotKey(window, cancelId);
        HWND expected = window; scrollWindow.compare_exchange_strong(expected, nullptr);
        state->window = nullptr;
        image_tools_detail::detachWindowState<ScrollState>(window);
        return DefWindowProcW(window, message, wparam, lparam);
    }
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}
ATOM registerClass(const wchar_t* name, WNDPROC proc, UINT style = 0) {
    WNDCLASSEXW type{sizeof(type)}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = proc; type.lpszClassName = name;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.style = style; return RegisterClassExW(&type);
}
}

bool PixelImage::valid() const {
    return width > 0 && height > 0 && static_cast<std::uint64_t>(width) * height <= imagePixelBudget &&
        pixels.size() == static_cast<size_t>(width) * height;
}
bool bitmapPixels(HBITMAP bitmap, PixelImage& output) {
    BITMAP dimensions{};
    if (!bitmap || !GetObjectW(bitmap, sizeof(dimensions), &dimensions) || dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0 ||
        static_cast<std::uint64_t>(dimensions.bmWidth) * dimensions.bmHeight > imagePixelBudget) return false;
    try {
        PixelImage data{dimensions.bmWidth, dimensions.bmHeight, std::vector<std::uint32_t>(static_cast<size_t>(dimensions.bmWidth) * dimensions.bmHeight)};
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = data.width;
        info.bmiHeader.biHeight = -data.height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        HDC dc = CreateCompatibleDC(nullptr); if (!dc) return false;
        const int rows = GetDIBits(dc, bitmap, 0, data.height, data.pixels.data(), &info, DIB_RGB_COLORS); DeleteDC(dc);
        if (rows != data.height) return false;
        for (auto& pixel : data.pixels) pixel |= 0xff000000;
        output = std::move(data); return true;
    } catch (...) { return false; }
}
HBITMAP bitmapFromPixels(const PixelImage& image) {
    if (!image.valid()) return nullptr;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = image.width;
    info.bmiHeader.biHeight = -image.height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* storage{}; HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &storage, nullptr, 0);
    if (bitmap) std::memcpy(storage, image.pixels.data(), image.pixels.size() * sizeof(std::uint32_t));
    return bitmap;
}
RECT physicalSelection(RECT local, POINT origin) {
    if (local.left > local.right) std::swap(local.left, local.right);
    if (local.top > local.bottom) std::swap(local.top, local.bottom);
    const std::int64_t left = static_cast<std::int64_t>(local.left) + origin.x, top = static_cast<std::int64_t>(local.top) + origin.y;
    const std::int64_t right = static_cast<std::int64_t>(local.right) + origin.x, bottom = static_cast<std::int64_t>(local.bottom) + origin.y;
    if (left < LONG_MIN || left > LONG_MAX || right < LONG_MIN || right > LONG_MAX ||
        top < LONG_MIN || top > LONG_MAX || bottom < LONG_MIN || bottom > LONG_MAX) return {};
    return {static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right), static_cast<LONG>(bottom)};
}
VerticalStitcher::VerticalStitcher(std::uint64_t pixels, int height)
    : pixelLimit_(std::min(pixels, imagePixelBudget)), heightLimit_(std::clamp(height, 1, 32768)) {}
StitchResult VerticalStitcher::append(const PixelImage& next) {
    if (!next.valid()) return {StitchStatus::InvalidImage};
    if (image_.pixels.empty()) {
        if (next.pixels.size() > pixelLimit_ || next.height > heightLimit_) return {StitchStatus::LimitReached};
        image_ = next; previous_ = next; return {StitchStatus::Started, 0, next.height};
    }
    if (next.width != previous_.width || next.height != previous_.height) return {StitchStatus::SizeMismatch};
    if (next.pixels == previous_.pixels) return {StitchStatus::NoChange, next.height, 0};
    Match stationary = matchRows(previous_, next, 0, next.height, 256, 128);
    if (stationary.error < .35 && stationary.ratio > .999) return {StitchStatus::NoChange, next.height, 0};
    if (matchRows(next, next, 0, next.height, 128, 64).variance < 36) return {StitchStatus::NoReliableOverlap};
    const int minimumOverlap = std::max(32, next.height / 5);
    int acceptedShift{};
    for (int shift = 1; shift <= next.height - minimumOverlap; ++shift) {
        const int overlap = next.height - shift;
        Match coarse = matchRows(previous_, next, shift, overlap, 96, 24);
        if (coarse.error > 2.5 || coarse.ratio < .985 || coarse.variance < 36) continue;
        // Sampling only proposes offsets. Verify every overlap pixel so thin
        // text columns, sticky controls and sparse changes cannot alias the grid.
        Match verified = matchRows(previous_, next, shift, overlap, next.width, overlap);
        if (verified.error > 1.75 || verified.ratio < .995 || verified.variance < 36) continue;
        if (acceptedShift) return {StitchStatus::NoReliableOverlap}; // Ambiguity is unsafe.
        acceptedShift = shift;
    }
    if (!acceptedShift) return {StitchStatus::NoReliableOverlap};
    const std::uint64_t newHeight = static_cast<std::uint64_t>(image_.height) + acceptedShift;
    if (newHeight > static_cast<unsigned>(heightLimit_) || newHeight * next.width > pixelLimit_) return {StitchStatus::LimitReached};
    const int overlap = next.height - acceptedShift;
    const size_t newSize = static_cast<size_t>(newHeight) * next.width;
    if (newSize > image_.pixels.capacity()) {
        // std::vector's normal geometric growth may exceed the accepted pixel
        // limit. Bound capacity explicitly, including the old+new overlap peak.
        std::vector<std::uint32_t> combined;
        const size_t capacity = static_cast<size_t>(std::min<std::uint64_t>(pixelLimit_, std::max<std::uint64_t>(newSize, image_.pixels.capacity() * 2)));
        combined.reserve(capacity);
        combined.insert(combined.end(), image_.pixels.begin(), image_.pixels.end());
        combined.insert(combined.end(), next.pixels.begin() + static_cast<size_t>(overlap) * next.width, next.pixels.end());
        image_.pixels.swap(combined);
    } else image_.pixels.insert(image_.pixels.end(), next.pixels.begin() + static_cast<size_t>(overlap) * next.width, next.pixels.end());
    image_.height = static_cast<int>(newHeight); previous_ = next;
    return {StitchStatus::Appended, overlap, acceptedShift};
}
void VerticalStitcher::reset() { image_ = {}; previous_ = {}; }
HWND pinImage(HWND owner, HBITMAP bitmap, CaptureCallback callback) {
    std::shared_ptr<PinState> state;
    try { state = std::make_shared<PinState>(); }
    catch (...) { if (bitmap) DeleteObject(bitmap); return nullptr; }
    state->bitmap = bitmap; state->owner = owner; state->callback = std::move(callback);
    BITMAP dimensions{};
    if (!bitmap || !GetObjectW(bitmap, sizeof(dimensions), &dimensions) || dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0) return nullptr;
    const auto count = static_cast<std::uint64_t>(dimensions.bmWidth) * dimensions.bmHeight;
    if (count > imagePixelBudget || pinnedPixels.load() + count > imagePixelBudget || pinCount.load() >= 12) {
        MessageBoxW(owner, L"贴图总像素已达 32 MP 或窗口已达 12 个，请关闭其他贴图后重试。", L"贴图", MB_OK | MB_ICONWARNING); return nullptr;
    }
    state->memory = image_tools_detail::reserveMemory(count * 4);
    if (!state->memory) { MessageBoxW(owner, L"贴图超出图像内存预算，请关闭其他截图或贴图后重试。", L"贴图", MB_OK | MB_ICONWARNING); return nullptr; }
    pinnedPixels.fetch_add(count); pinCount.fetch_add(1); state->budget = count;
    state->width = dimensions.bmWidth; state->height = dimensions.bmHeight;
    state->source = CreateCompatibleDC(nullptr); if (!state->source) return nullptr; state->oldBitmap = SelectObject(state->source, bitmap);
    DpiScope dpi; MONITORINFO monitor{sizeof(monitor)}; POINT pointer{}; GetCursorPos(&pointer);
    GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST), &monitor);
    state->zoom = std::min({1.0, (monitor.rcWork.right - monitor.rcWork.left - 80.0) / state->width,
        (monitor.rcWork.bottom - monitor.rcWork.top - 180.0) / state->height}); state->zoom = std::max(.01, state->zoom);
    static ATOM cls = registerClass(L"DeskEfficiencyPinnedImage", pinProc, CS_DBLCLKS);
    if (!cls) return nullptr;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"DeskEfficiencyPinnedImage", L"贴图", WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL,
        monitor.rcWork.left + 40, monitor.rcWork.top + 40, 520, 320, IsWindow(owner) ? owner : nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) return nullptr;
    state->resizeForZoom(); ShowWindow(window, SW_SHOW); UpdateWindow(window); return IsWindow(window) ? window : nullptr;
}
bool scrollingCaptureActive() { return scrollWindow.load() != nullptr; }
void shutdownImageTools() {
    std::vector<HWND> windows;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM value) -> BOOL {
        wchar_t name[96]{}; GetClassNameW(window, name, 96);
        if (!wcscmp(name, L"DeskEfficiencyPinnedImage") || !wcscmp(name, L"DeskEfficiencyScrollingCapture") ||
            !wcscmp(name, L"DeskEfficiencyCaptureOverlay")) reinterpret_cast<std::vector<HWND>*>(value)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    for (HWND window : windows) if (IsWindow(window)) DestroyWindow(window);
}
HWND beginScrollingCapture(HWND owner, RECT region, CaptureCallback callback) {
    if (scrollingCaptureActive()) { SetForegroundWindow(scrollWindow.load()); return scrollWindow.load(); }
    DpiScope dpi;
    if (region.left > region.right) std::swap(region.left, region.right); if (region.top > region.bottom) std::swap(region.top, region.bottom);
    const auto w = static_cast<std::int64_t>(region.right) - region.left, h = static_cast<std::int64_t>(region.bottom) - region.top;
    if (w <= 0 || h < 64 || static_cast<std::uint64_t>(w) * h > imagePixelBudget) return nullptr;
    auto state = std::make_shared<ScrollState>(); state->owner = owner; state->region = region; state->callback = std::move(callback);
    // At most two bounded stitch buffers plus previous/current viewport data.
    // This also covers viewport sampling and the final bitmap before handoff.
    state->memory = image_tools_detail::reserveMemory(2 * imagePixelBudget * 4 + 2 * static_cast<std::uint64_t>(w) * h * 4);
    if (!state->memory) { MessageBoxW(owner, L"长截图超出图像内存预算，请减小选区或关闭其他贴图。", L"长截图", MB_OK | MB_ICONWARNING); return nullptr; }
    HBITMAP first = captureRegion(region); PixelImage pixels;
    const bool loaded = first && bitmapPixels(first, pixels); if (first) DeleteObject(first);
    if (!loaded) { MessageBoxW(owner, L"无法采样选区，请检查桌面会话。", L"长截图", MB_OK | MB_ICONWARNING); return nullptr; }
    try { if (state->stitcher.append(pixels).status != StitchStatus::Started) return nullptr; } catch (...) { return nullptr; }
    static ATOM cls = registerClass(L"DeskEfficiencyScrollingCapture", scrollProc); if (!cls) return nullptr;
    MONITORINFO monitor{sizeof(monitor)}; POINT anchor{region.right, region.top}; GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    int x = monitor.rcWork.right - 448, y = monitor.rcWork.top + 20;
    if (region.right + 448 < monitor.rcWork.right) x = region.right + 12;
    else if (region.left - 448 > monitor.rcWork.left) x = region.left - 448;
    else if (region.bottom + 190 < monitor.rcWork.bottom) y = region.bottom + 12;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DeskEfficiencyScrollingCapture", L"长截图 · 手动滚动", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, 436, 176, IsWindow(owner) ? owner : nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) return nullptr;
    scrollWindow = window; ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); return IsWindow(window) ? window : nullptr;
}
}
