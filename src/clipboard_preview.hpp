#pragma once
#include "clipboard.hpp"
#include <functional>
#include <memory>
namespace desk {
struct ClipboardPreviewText {std::wstring text;bool supported=false;};
ClipboardPreviewText clipboardPreviewText(const ClipPayload& payload);
std::wstring clipboardDetails(const HistoryItem& item,const ClipPayload* payload=nullptr);
class FloatingClipboardPreview {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    FloatingClipboardPreview();
    ~FloatingClipboardPreview();
    // Takes ownership of the bitmap. UI-thread only; never activates the popup.
    void show(HWND owner,const HistoryItem& item,const std::wstring& text,HBITMAP bitmap,
              RECT anchor,bool content,bool dark);
    void hide();
    HWND window() const;
};
}
