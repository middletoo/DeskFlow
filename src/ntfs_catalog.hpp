#pragma once
#include <Windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace desk::ntfs {
struct Name {uint64_t parent=0;std::wstring text;};
struct Run {uint64_t vcn=0,length=0;int64_t lcn=-1;};
struct Record {
    uint64_t file=0,base=0,size=0,modified=0;
    DWORD attributes=0;
    bool folder=false,sizeKnown=false;
    uint16_t links=0;
    std::vector<Name> names;
    std::vector<Run> dataRuns,listRuns;
    std::vector<unsigned char> attributeList;
    uint64_t listSize=0;
};
enum class Decode {Unused,Valid,Invalid};
Decode decodeRecord(const void* bytes,size_t length,uint64_t ordinal,Record& output,bool sectorFixup=true);
std::vector<Run> decodeRuns(const void* bytes,size_t length,uint64_t startVcn);
struct Root {
    std::wstring path;
    uint64_t rootFile=0,journal=0,nextUsn=0,records=0;
    DWORD serial=0;
    bool raw=false;
};
struct Change {uint64_t file=0,parent=0;DWORD reason=0,attributes=0;std::wstring name;};
class Volume {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    Root info;
    explicit Volume(const std::wstring& root);
    ~Volume();
    void enumerate(const std::function<void(const Record&)>& visit,
                   const std::function<bool()>& stopped,
                   const std::function<void(uint64_t)>& progress);
    std::optional<Record> file(uint64_t reference);
    // A missing journal checkpoint is an explicit rebuild condition.
    uint64_t changes(uint64_t cursor,std::vector<Change>& output);
};
}
