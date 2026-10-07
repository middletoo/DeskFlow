#pragma once
#include <Windows.h>
#include <filesystem>
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
namespace desk {
struct ClipFormat {
    UINT format = 0;
    std::wstring name;
    std::vector<unsigned char> data;
};
struct ClipPayload {
    std::vector<ClipFormat> formats;
    std::wstring source;
};
struct HistoryItem {
    int64_t id = 0, created = 0;
    std::wstring kind, title, preview, source;
    bool pinned = false;
    uint64_t bytes = 0;
};
class HistoryStore {
  public:
    explicit HistoryStore(const std::filesystem::path &directory);
    ~HistoryStore();
    int64_t append(const ClipPayload &payload);
    std::vector<HistoryItem> list(const std::wstring &query = L"", int64_t beforeId = 0,
                                  size_t limit = 100, bool pinnedOnly = false,
                                  const std::wstring& kind = L"");
    ClipPayload load(int64_t id);
    void setPinned(int64_t id, bool pinned);
    void erase(int64_t id);
    int64_t count();
    void setLimits(size_t maximumEntryBytes, uint64_t imageQuotaBytes);
    void backup(const std::filesystem::path &target);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
ClipPayload readClipboardPayload(HWND owner, size_t maximumBytes = 32 * 1024 * 1024);
bool restoreClipboardPayload(HWND owner, const ClipPayload &payload);
} // namespace desk
