#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace desk {
// Callback owns the returned bitmap and must release it with DeleteObject.
using CaptureCallback = std::function<void(HBITMAP, const std::wstring&)>;
// GIF/MP4 receive physical virtual-desktop bounds after the editor closes.
using RegionCallback = std::function<void(RECT, const std::wstring&)>;
// Runs on a worker thread. Source is borrowed and remains valid until return.
// Return a new same-size bitmap (ownership transfers to the editor), or throw.
// Cancellation becomes true immediately when the editor closes.
using InlineTranslation = std::function<HBITMAP(HBITMAP, std::atomic_bool&)>;
// UI-thread entry point. OCR invokes CaptureCallback after the editor closes.
// Translation keeps the editor open; copy/save do not invoke CaptureCallback.
void beginCapture(HWND owner, CaptureCallback callback, RegionCallback recordingCallback = {}, InlineTranslation translation = {});
bool captureActive();
// Physical virtual-desktop coordinates. Normalizes reversed corners, clips to
// the desktop and rejects empty or >32-megapixel requests. Caller owns output.
HBITMAP captureRegion(RECT physicalScreenRect);
// Atomic replacement after successful PNG encoding; source ownership is kept.
bool saveBitmapPng(HBITMAP bitmap, const std::filesystem::path& path);
bool copyBitmapToClipboard(HBITMAP bitmap);
}
