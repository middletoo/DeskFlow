#pragma once
#include <Windows.h>
#include <winioctl.h>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace desk::search_detail {
// Only collapse repeated metadata events in a single bounded USN read.
// Namespace events and directory events are barriers, preserving rename,
// delete/recreate and parent/child ordering.
class UsnMetadataBatch {
    struct Record {
        uint64_t file, parent;
        int64_t stamp;
        DWORD reason, attributes;
        std::wstring name;
    };
    using Visitor = std::function<void(uint64_t,uint64_t,int64_t,DWORD,DWORD,const std::wstring&)>;
    Visitor visitor_;
    std::vector<Record> records_;
    std::unordered_map<uint64_t, size_t> positions_;

  public:
    explicit UsnMetadataBatch(Visitor visitor) : visitor_(std::move(visitor)) {}
    void flush() {
        for (const auto& r : records_)
            visitor_(r.file, r.parent, r.stamp, r.reason, r.attributes, r.name);
        records_.clear();
        positions_.clear();
    }
    void add(uint64_t file, uint64_t parent, int64_t stamp, DWORD reason,
             DWORD attributes, const std::wstring& name) {
        constexpr DWORD metadata = USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND |
            USN_REASON_DATA_TRUNCATION | USN_REASON_BASIC_INFO_CHANGE | USN_REASON_CLOSE;
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) || (reason & ~metadata)) {
            flush();
            visitor_(file, parent, stamp, reason, attributes, name);
            return;
        }
        auto found = positions_.find(file);
        if (found != positions_.end()) {
            auto& previous = records_[found->second];
            if (previous.parent == parent && previous.name == name) {
                previous.stamp = stamp;
                previous.reason |= reason;
                previous.attributes = attributes;
                return;
            }
            // Different hardlink names must remain separate even if no
            // namespace event was included in this read.
            flush();
        }
        positions_.emplace(file, records_.size());
        records_.push_back({file, parent, stamp, reason, attributes, name});
    }
};
}
