#pragma once
#include "capture.hpp"
#include <cstdint>
#include <vector>
#include <memory>
#include <new>

namespace desk {
namespace image_tools_detail {
struct MemoryReservation;
using MemoryLease = std::shared_ptr<MemoryReservation>;
constexpr std::uint64_t imageWorkingBytes = 384ULL * 1024 * 1024;
constexpr std::uint64_t hostAndEncoderReserveBytes = 128ULL * 1024 * 1024;
MemoryLease reserveMemory(std::uint64_t bytes);
std::uint64_t captureMemoryRequirement(int physicalWidth, int physicalHeight);
// One reference belongs to the HWND. Each WndProc takes another reference
// before dispatch, including across nested modal loops and DestroyWindow.
template<class T> bool attachWindowState(HWND window, LPARAM creation) {
    auto* context = reinterpret_cast<CREATESTRUCTW*>(creation);
    auto* creator = static_cast<std::shared_ptr<T>*>(context->lpCreateParams);
    auto* owner = new(std::nothrow) std::shared_ptr<T>(*creator);
    if (!owner) return false;
    SetLastError(ERROR_SUCCESS);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
    if (GetLastError() != ERROR_SUCCESS) { delete owner; return false; }
    (*owner)->window = window;
    return true;
}
template<class T> std::shared_ptr<T> windowState(HWND window) {
    auto* owner = reinterpret_cast<std::shared_ptr<T>*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return owner ? *owner : std::shared_ptr<T>{};
}
template<class T> void detachWindowState(HWND window) {
    auto* owner = reinterpret_cast<std::shared_ptr<T>*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    delete owner;
}
}
// Conservative live reservations, including transient export buffers. The
// separate 128-MiB Host/encoder allowance is not included in this value.
std::uint64_t imageToolReservedBytes();
constexpr std::uint64_t imagePixelBudget = 32ULL * 1024 * 1024;
struct PixelImage {
    int width{}, height{};
    std::vector<std::uint32_t> pixels; // Top-down, opaque BGRA.
    bool valid() const;
};
bool bitmapPixels(HBITMAP bitmap, PixelImage& output);
HBITMAP bitmapFromPixels(const PixelImage& image); // Caller owns the result.
// Local selection -> physical screen coordinates, including negative monitor
// origins. Invalid arithmetic returns an empty rectangle.
RECT physicalSelection(RECT localBounds, POINT virtualOrigin);
enum class StitchStatus { Started, Appended, NoChange, NoReliableOverlap, SizeMismatch, LimitReached, InvalidImage };
struct StitchResult { StitchStatus status{StitchStatus::InvalidImage}; int overlap{}, addedRows{}; };
class VerticalStitcher {
public:
    explicit VerticalStitcher(std::uint64_t pixelLimit = imagePixelBudget, int heightLimit = 32768);
    StitchResult append(const PixelImage& next);
    const PixelImage& image() const { return image_; }
    void reset();
private:
    PixelImage image_, previous_;
    std::uint64_t pixelLimit_;
    int heightLimit_;
};
// UI thread. Takes ownership even on failure. OCR/translate receive a separate
// owned bitmap so the pinned original remains usable.
HWND pinImage(HWND owner, HBITMAP ownedBitmap, CaptureCallback callback = {});
// One session at a time. Scroll the underlying application manually; a small
// control window stays outside the sampled region. Enter finishes to a pin.
HWND beginScrollingCapture(HWND owner, RECT physicalRegion, CaptureCallback callback = {});
bool scrollingCaptureActive();
// Host shutdown, on the UI thread, before destroying callback owners.
void shutdownImageTools();
}
