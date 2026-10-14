#pragma once
#include "search.hpp"
#include "ntfs_catalog.hpp"
#include <functional>
#include <string_view>

namespace desk {
struct CatalogEntry {
    uint64_t file=0,parent=0,id=0,size=0,modified=0;
    uint16_t root=0;
    bool folder=false,sizeKnown=false;
    std::string name;
};
struct CatalogView {
    std::string_view name,foldedName;
    bool folder=false,sizeKnown=false;
    uint64_t size=0,modified=0;
    std::function<std::string()> path;
};
using CatalogPredicate=std::function<bool(const CatalogView&)>;
class NativeCatalog {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    explicit NativeCatalog(const std::filesystem::path& data);
    ~NativeCatalog();
    bool available();
    std::vector<SearchItem> query(const SearchQuery&,size_t,const CatalogPredicate&,const std::vector<std::string>& nameHints={});
    bool pending()const;
    SearchStatus status();
    std::vector<CatalogEntry> file(uint16_t root,uint64_t reference);
    void replace(uint16_t root,uint64_t reference,const std::vector<CatalogEntry>& entries);
    uint64_t total()const;
    uint64_t generation()const;
    std::filesystem::path snapshotFile()const;
    std::vector<ntfs::Root> volumes();
    void repairIncompleteLog();
    void each(const std::function<void(const CatalogEntry&)>&)const;
};
// Initial builds and compaction stream entries to disk. Record/name pages and
// sorted row IDs are mapped instead of allocating millions of C++ strings.
class CatalogBuilder {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    CatalogBuilder(const std::filesystem::path&,const std::vector<ntfs::Root>&);
    ~CatalogBuilder();
    void add(const CatalogEntry&);
    uint64_t finish(const std::function<bool()>& stopped,
                    const std::function<void(const std::wstring&)>& progress);
};
void appendCatalogChange(const std::filesystem::path&,uint64_t generation,uint16_t root,
                         uint64_t file,const std::vector<CatalogEntry>&,bool durable=true);
void flushCatalogChanges(const std::filesystem::path&);
void catalogStatus(const std::filesystem::path&,const SearchStatus&,DWORD pid,
                   const std::vector<ntfs::Root>&);
bool runNativeCatalog(const std::filesystem::path&,const std::vector<std::wstring>&,
                      HANDLE stop,HANDLE parent);
void buildCatalogLookup(const std::filesystem::path&,const std::function<bool()>& stopped,
                        const std::function<void(const std::wstring&)>& progress);
}
