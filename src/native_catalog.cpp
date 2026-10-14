#include "native_catalog.hpp"
#include "json.hpp"
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <system_error>
#include <queue>
#include <future>
#include <atomic>

namespace desk {
namespace {
namespace fs=std::filesystem;
using Key=std::pair<uint16_t,uint64_t>;
constexpr uint32_t format=1;
constexpr uint16_t folderBit=0x8000,knownBit=0x4000,duplicateBit=0x2000,lengthMask=0x0fff;
constexpr size_t orders=8;
enum Order {ByName,ByPath,BySize,ByType,ByModified,ByFile,ByParent,ById};
struct Header {
    char magic[8];uint32_t version=0,roots=0;
    uint64_t generation=0,rows=0,total=0,stringsSize=0,records=0,strings=0;
    uint64_t order[orders]{};uint64_t bytes=0,maximumId=0;
};
struct RootDisk {wchar_t path[4]{};DWORD serial=0;uint32_t reserved=0;uint64_t file=0,journal=0,cursor=0;};
struct Row {
    uint64_t file=0,parent=0,id=0,size=0,modified=0;
    uint32_t name=0,path=0;
    uint16_t root=0,flags=0;
};
static_assert(sizeof(Row)==56);
struct KeyHash {size_t operator()(const Key& k)const{return std::hash<uint64_t>{}(k.second)^(size_t(k.first)*0x9e3779b9);}};
struct Spool {uint64_t file,parent,id,size,modified;uint16_t root,flags,length,reserved;};
struct LookupHeader {char magic[8]{};uint64_t generation=0,rows=0,keys=0,directory=0,payload=0,bytes=0;};
struct LookupKey {uint32_t key=0,count=0;uint64_t offset=0,length=0;};
std::string bytes(const std::wstring& s){
    if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    if(!n)throw std::runtime_error("Invalid Unicode filename");std::string r(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;
}
std::wstring text(std::string_view s){
    if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    if(!n)throw std::runtime_error("Invalid catalog Unicode");std::wstring r(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n);return r;
}
std::wstring errorText(const std::exception& e){
    const char* raw=e.what();int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,raw,-1,nullptr,0);UINT code=n?CP_UTF8:CP_ACP;
    if(!n)n=MultiByteToWideChar(code,0,raw,-1,nullptr,0);std::wstring result(n,L'\0');if(n)MultiByteToWideChar(code,0,raw,-1,result.data(),n);if(!result.empty())result.pop_back();return result;
}
std::string lower(std::string_view s){
    if(std::all_of(s.begin(),s.end(),[](unsigned char c){return c<128;})){
        std::string r(s);for(auto& c:r)if(c>='A'&&c<='Z')c+=32;return r;
    }
    auto w=text(s);std::wstring f(w.size()+4,L'\0');
    int n=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,w.data(),(int)w.size(),f.data(),(int)f.size(),nullptr,nullptr,0);
    if(!n)return std::string(s);f.resize(n);return bytes(f);
}
uint64_t stamp(){FILETIME t{};GetSystemTimePreciseAsFileTime(&t);return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
uint32_t checksum(const void* p,size_t size){uint32_t h=2166136261u;const auto* b=(const unsigned char*)p;for(size_t i=0;i<size;++i)h=(h^b[i])*16777619u;return h;}
void fail(const char* what){throw std::system_error(GetLastError(),std::system_category(),what);}
uint64_t align(uint64_t n){return (n+4095)&~uint64_t(4095);}
struct Mapping {
    HANDLE file=INVALID_HANDLE_VALUE,map=nullptr;unsigned char* data=nullptr;uint64_t length=0;
    ~Mapping(){close();}
    void close(){if(data)UnmapViewOfFile(data);if(map)CloseHandle(map);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);data=nullptr;map=nullptr;file=INVALID_HANDLE_VALUE;length=0;}
    void open(const fs::path& path,bool write=false,uint64_t size=0){
        close();file=CreateFileW(path.c_str(),GENERIC_READ|(write?GENERIC_WRITE:0),FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,write?CREATE_ALWAYS:OPEN_EXISTING,0,nullptr);
        if(file==INVALID_HANDLE_VALUE)fail("Open native catalog");
        if(write){LARGE_INTEGER n{};n.QuadPart=(LONGLONG)size;if(!SetFilePointerEx(file,n,nullptr,FILE_BEGIN)||!SetEndOfFile(file))fail("Size native catalog");}
        LARGE_INTEGER n{};if(!GetFileSizeEx(file,&n)||n.QuadPart<=0)throw std::runtime_error("Empty native catalog");length=(uint64_t)n.QuadPart;
        if(length>SIZE_MAX)throw std::runtime_error("Catalog exceeds process address space; use x64");
        map=CreateFileMappingW(file,nullptr,write?PAGE_READWRITE:PAGE_READONLY,0,0,nullptr);if(!map)fail("Map native catalog");
        data=(unsigned char*)MapViewOfFile(map,write?FILE_MAP_WRITE:FILE_MAP_READ,0,0,0);if(!data)fail("Read native catalog mapping");
    }
};
struct SharedInput {
    HANDLE file=INVALID_HANDLE_VALUE;
    explicit SharedInput(const fs::path& path){file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);}
    ~SharedInput(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    uint64_t size()const{LARGE_INTEGER n{};if(file==INVALID_HANDLE_VALUE||!GetFileSizeEx(file,&n)||n.QuadPart<0)return 0;return (uint64_t)n.QuadPart;}
    void seek(uint64_t position){LARGE_INTEGER at{};at.QuadPart=(LONGLONG)position;if(!SetFilePointerEx(file,at,nullptr,FILE_BEGIN))fail("Seek catalog input");}
    size_t read(void* target,size_t length){DWORD got=0;if(length>MAXDWORD||!ReadFile(file,target,(DWORD)length,&got,nullptr))fail("Read catalog input");return got;}
};
bool replaceFile(const fs::path& from,const fs::path& to,DWORD flags){
    for(unsigned attempt=0;attempt<16;++attempt){if(MoveFileExW(from.c_str(),to.c_str(),flags))return true;DWORD error=GetLastError();if(error!=ERROR_ACCESS_DENIED&&error!=ERROR_SHARING_VIOLATION)return false;Sleep(10);SetLastError(error);}return false;
}
struct CatalogPointer {char magic[8]{};uint64_t generation=0;};
uint64_t currentGeneration(const fs::path& data){
    SharedInput input(data/L"catalog.current");if(input.file==INVALID_HANDLE_VALUE)return 0;
    CatalogPointer pointer{};if(input.size()!=sizeof(pointer)||input.read(&pointer,sizeof(pointer))!=sizeof(pointer)||memcmp(pointer.magic,"DFCAT001",8)||!pointer.generation)throw std::runtime_error("Invalid catalog publication pointer");
    return pointer.generation;
}
fs::path snapshotName(const fs::path& data,uint64_t generation){return generation?data/(L"catalog."+std::to_wstring(generation)+L".index"):data/L"catalog.index";}
fs::path lookupName(const fs::path& data,uint64_t generation){return data/(L"catalog."+std::to_wstring(generation)+L".lookup");}
void cleanRetiredGenerations(const fs::path& data,uint64_t current){
    std::vector<std::pair<uint64_t,fs::path>> files;uint64_t previous=0;std::error_code ec;
    for(fs::directory_iterator it(data,ec),end;!ec&&it!=end;it.increment(ec)){
        auto name=it->path().filename().wstring();if(!name.starts_with(L"catalog.")||(!name.ends_with(L".index")&&!name.ends_with(L".lookup")))continue;
        auto last=name.rfind(L'.');auto digits=name.substr(8,last-8);if(digits.empty()||digits.size()>20||digits.find_first_not_of(L"0123456789")!=std::wstring::npos)continue;
        try{auto generation=std::stoull(digits);if(generation<current){files.emplace_back(generation,it->path());if(name.ends_with(L".index"))previous=std::max(previous,generation);}}catch(...){}
    }
    // Retain the current/previous generation. Mapped older files may remain
    // until their readers release them; retry cleanup on the next idle pass.
    for(const auto& [generation,path]:files)if(generation!=previous)DeleteFileW(path.c_str());
}
void publishGeneration(const fs::path& data,uint64_t generation){
    CatalogPointer pointer{};memcpy(pointer.magic,"DFCAT001",8);pointer.generation=generation;
    const auto temporary=data/L"catalog.current.tmp";
    HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_ALWAYS,FILE_FLAG_WRITE_THROUGH,nullptr);if(handle==INVALID_HANDLE_VALUE)fail("Create catalog pointer");
    DWORD written=0;bool ok=WriteFile(handle,&pointer,sizeof(pointer),&written,nullptr)&&written==sizeof(pointer)&&FlushFileBuffers(handle);DWORD error=GetLastError();CloseHandle(handle);if(!ok){SetLastError(error);fail("Write catalog pointer");}
    if(!replaceFile(temporary,data/L"catalog.current",MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))fail("Publish catalog pointer");
    cleanRetiredGenerations(data,generation);
}
std::string_view name(const Mapping& m,const Header& h,const Row& r,bool folded=false){
    if(r.root>=h.roots||!r.id||r.id>h.maximumId||!(r.flags&lengthMask)||(r.flags&lengthMask)>1024)throw std::runtime_error("Catalog row metadata bounds");
    uint64_t off=r.name+(folded?(r.flags&lengthMask)+1:0);
    if(off>=h.stringsSize)throw std::runtime_error("Catalog string offset");
    const char* p=(const char*)m.data+h.strings+off;
    const size_t maximum=(size_t)std::min<uint64_t>(1025,h.stringsSize-off);
    const size_t len=strnlen_s(p,maximum);if(len==maximum)throw std::runtime_error("Catalog string bounds");return {p,len};
}
std::string_view prefix(const Mapping& m,const Header& h,const Row& r,bool folded=false){
    if(r.path>=h.stringsSize)throw std::runtime_error("Catalog path offset");
    const char* p=(const char*)m.data+h.strings+r.path;size_t max=(size_t)std::min<uint64_t>(32768*4,h.stringsSize-r.path);
    auto n=strnlen_s(p,max);if(n==max)throw std::runtime_error("Catalog path bounds");if(!folded)return {p,n};
    p+=n+1;max-=n+1;auto len=strnlen_s(p,max);if(len==max)throw std::runtime_error("Catalog folded path bounds");return {p,len};
}
int joined(std::string_view a,std::string_view an,std::string_view b,std::string_view bn){
    std::array<std::string_view,2> x{a,an},y{b,bn};size_t i=0,j=0;
    while(i<2&&j<2){if(x[i].empty()){++i;continue;}if(y[j].empty()){++j;continue;}
        size_t n=std::min(x[i].size(),y[j].size());int c=memcmp(x[i].data(),y[j].data(),n);if(c)return c;
        x[i].remove_prefix(n);y[j].remove_prefix(n);
    }
    bool left=std::any_of(x.begin()+std::min<size_t>(i,2),x.end(),[](auto v){return !v.empty();});
    bool right=std::any_of(y.begin()+std::min<size_t>(j,2),y.end(),[](auto v){return !v.empty();});return left?1:right?-1:0;
}
std::string_view extension(std::string_view n){auto dot=n.rfind('.');return dot==std::string_view::npos||dot==0?std::string_view{}:n.substr(dot+1);}
CatalogEntry entry(const Mapping& m,const Header& h,const Row& r){return {r.file,r.parent,r.id,r.size,r.modified,r.root,(r.flags&folderBit)!=0,(r.flags&knownBit)!=0,std::string(name(m,h,r))};}
std::vector<uint32_t> tokensFor(std::string_view input){
    std::vector<uint32_t> points,keys;points.reserve(input.size());
    for(size_t i=0;i<input.size();){unsigned char c=input[i++];uint32_t p=c;
        unsigned extra=c<128?0:(c&0xe0)==0xc0?1:(c&0xf0)==0xe0?2:(c&0xf8)==0xf0?3:4;
        if(extra>3||extra>input.size()-i)throw std::runtime_error("Catalog lookup Unicode bounds");
        if(extra)p=c&((1u<<(6-extra))-1);for(unsigned j=0;j<extra;++j){unsigned char b=input[i++];if((b&0xc0)!=0x80)throw std::runtime_error("Catalog lookup Unicode continuation");p=(p<<6)|(b&63);}points.push_back(p);}
    keys.reserve(points.size()*2);
    for(size_t i=0;i<points.size();++i){keys.push_back(points[i]);if(i&&points[i-1]<128&&points[i]<128)keys.push_back(0x110000u+(points[i-1]<<7)+points[i]);}
    std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());return keys;
}
void lookup(const fs::path& data,const Mapping& mapped,const Header& h,const std::function<bool()>& stopped,const std::function<void(const std::wstring&)>& progress){
    progress(L"建立文件名候选索引");const auto* rows=(const Row*)(mapped.data+h.records);
    constexpr size_t chunk=4*1024*1024;std::vector<uint64_t> pairs;pairs.reserve(chunk);std::vector<fs::path> runs;
    auto flush=[&]{if(pairs.empty())return;std::sort(pairs.begin(),pairs.end());auto path=data/(L"catalog.lookup.run-"+std::to_wstring(runs.size())+L".tmp");std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write((const char*)pairs.data(),pairs.size()*8);if(!out)throw std::runtime_error("Catalog lookup sort write failed");runs.push_back(path);pairs.clear();};
    for(uint64_t i=0;i<h.rows;++i){if((i%4096)==0&&stopped())throw std::runtime_error("Native indexing stopped");
        if(rows[i].flags&duplicateBit)continue;for(auto key:tokensFor(name(mapped,h,rows[i],true))){pairs.push_back((uint64_t(key)<<32)|i);if(pairs.size()==chunk)flush();}}
    flush();pairs.clear();pairs.shrink_to_fit();progress(L"压缩文件名候选索引");
    std::vector<std::ifstream> inputs;for(const auto& path:runs)inputs.emplace_back(path,std::ios::binary);
    using Item=std::pair<uint64_t,size_t>;std::priority_queue<Item,std::vector<Item>,std::greater<Item>> heap;
    for(size_t i=0;i<inputs.size();++i){uint64_t value=0;if(inputs[i].read((char*)&value,8))heap.push({value,i});}
    const auto payloadPath=data/L"catalog.lookup.payload.tmp";std::ofstream payload(payloadPath,std::ios::binary|std::ios::trunc);
    std::vector<LookupKey> directory;uint64_t offset=0,processed=0;uint32_t previous=0;
    while(!heap.empty()){
        if((++processed%65536)==0&&stopped())throw std::runtime_error("Native indexing stopped");
        auto [value,run]=heap.top();heap.pop();uint32_t key=(uint32_t)(value>>32),row=(uint32_t)value;
        if(directory.empty()||directory.back().key!=key){if(!directory.empty())directory.back().length=offset-directory.back().offset;directory.push_back({key,0,offset,0});previous=0;}
        uint32_t delta=row-previous;previous=row;do{unsigned char b=(unsigned char)(delta&127);delta>>=7;if(delta)b|=128;payload.put((char)b);++offset;}while(delta);++directory.back().count;
        if(inputs[run].read((char*)&value,8))heap.push({value,run});
    }
    if(!directory.empty())directory.back().length=offset-directory.back().offset;payload.close();if(!payload)throw std::runtime_error("Catalog lookup compression failed");
    LookupHeader header{};memcpy(header.magic,"DFLOOKUP",8);header.generation=h.generation;header.rows=h.rows;header.keys=directory.size();header.directory=sizeof(header);header.payload=header.directory+directory.size()*sizeof(LookupKey);header.bytes=header.payload+offset;
    const auto temp=data/L"catalog.lookup.tmp";std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write((const char*)&header,sizeof(header));out.write((const char*)directory.data(),directory.size()*sizeof(LookupKey));std::ifstream compressed(payloadPath,std::ios::binary);out<<compressed.rdbuf();out.flush();if(!out)throw std::runtime_error("Catalog lookup publish write failed");out.close();compressed.close();
    if(!MoveFileExW(temp.c_str(),lookupName(data,h.generation).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))fail("Publish catalog lookup");
    for(auto& input:inputs)input.close();for(const auto& path:runs)DeleteFileW(path.c_str());DeleteFileW(payloadPath.c_str());
}
std::vector<ntfs::Root> roots(const Mapping& m,const Header& h){
    const auto* disk=(const RootDisk*)(m.data+sizeof(Header));std::vector<ntfs::Root> result;
    for(uint32_t i=0;i<h.roots;++i){ntfs::Root r;r.path.assign(disk[i].path,3);r.serial=disk[i].serial;r.rootFile=disk[i].file;r.journal=disk[i].journal;r.nextUsn=disk[i].cursor;result.push_back(std::move(r));}return result;
}
}
struct CatalogBuilder::Impl {
    fs::path directory,spoolPath,stringPath;
    std::ofstream spool,strings;
    std::vector<ntfs::Root> roots;
    struct Directory {uint64_t parent=0;std::string name,path;uint32_t offset=0;bool resolving=false,ready=false;};
    std::unordered_map<Key,Directory,KeyHash> dirs;
    uint64_t count=0,stringBytes=0,maximumId=0,unresolved=0;
    Impl(const fs::path& p,const std::vector<ntfs::Root>& r):directory(p),roots(r){
        fs::create_directories(p);spoolPath=p/L"catalog.spool.tmp";stringPath=p/L"catalog.strings.tmp";
        spool.open(spoolPath,std::ios::binary|std::ios::trunc);strings.open(stringPath,std::ios::binary|std::ios::trunc);
        if(!spool||!strings)throw std::runtime_error("Cannot create catalog build files");
    }
    uint32_t stringPair(std::string_view raw){
        auto folded=lower(raw);if(stringBytes+raw.size()+folded.size()+2>UINT32_MAX)throw std::runtime_error("Catalog string table exceeds 4 GiB");
        auto at=(uint32_t)stringBytes;strings.write(raw.data(),raw.size());strings.put(0);strings.write(folded.data(),folded.size());strings.put(0);stringBytes+=raw.size()+folded.size()+2;return at;
    }
    std::optional<std::string> directoryPath(Key key,unsigned depth=0){
        if(key.first>=roots.size()||depth>512)return {};
        if(key.second==roots[key.first].rootFile)return bytes(roots[key.first].path);
        auto it=dirs.find(key);if(it==dirs.end()||it->second.resolving)return {};
        auto& d=it->second;if(d.ready)return d.path;d.resolving=true;
        auto parent=directoryPath({key.first,d.parent},depth+1);d.resolving=false;if(!parent)return {};
        if(parent->size()+d.name.size()+1>32768*4)return {};
        d.path=*parent+d.name+"\\";d.offset=stringPair(d.path);d.ready=true;return d.path;
    }
};
CatalogBuilder::CatalogBuilder(const fs::path& p,const std::vector<ntfs::Root>& r):impl_(std::make_unique<Impl>(p,r)){}
CatalogBuilder::~CatalogBuilder()=default;
void CatalogBuilder::add(const CatalogEntry& e){
    if(e.root>=impl_->roots.size()||e.name.empty()||e.name.size()>1024)throw std::runtime_error("Catalog entry bounds");
    auto id=e.id?e.id:impl_->count+1;impl_->maximumId=std::max(impl_->maximumId,id);
    Spool s{e.file,e.parent,id,e.size,e.modified,e.root,(uint16_t)((e.folder?folderBit:0)|(e.sizeKnown?knownBit:0)),(uint16_t)e.name.size(),0};
    impl_->spool.write((const char*)&s,sizeof(s));impl_->spool.write(e.name.data(),e.name.size());
    if(!impl_->spool)throw std::runtime_error("Catalog spool write failed");++impl_->count;
    if(e.folder)impl_->dirs.try_emplace({e.root,e.file},Impl::Directory{e.parent,e.name});
}
uint64_t CatalogBuilder::finish(const std::function<bool()>& stopped,const std::function<void(const std::wstring&)>& progress){
    auto& b=*impl_;b.spool.close();
    if(b.count>UINT32_MAX)throw std::runtime_error("Catalog exceeds 32-bit row index");
    progress(L"建立目录路径");
    std::vector<uint32_t> rootOffsets;for(const auto& root:b.roots)rootOffsets.push_back(b.stringPair(bytes(root.path)));
    for(auto& [key,d]:b.dirs){if(stopped())throw std::runtime_error("Native indexing stopped");b.directoryPath(key);}
    // Stream rows into a temporary mapped record table; names remain streamed.
    const auto recordsPath=b.directory/L"catalog.records.tmp";Mapping recordMap;
    recordMap.open(recordsPath,true,std::max<uint64_t>(4096,b.count*sizeof(Row)));
    auto* rows=(Row*)recordMap.data;std::ifstream input(b.spoolPath,std::ios::binary);uint64_t accepted=0;
    for(uint64_t i=0;i<b.count;++i){
        if((i%4096)==0&&stopped())throw std::runtime_error("Native indexing stopped");
        Spool s{};input.read((char*)&s,sizeof(s));std::string n(s.length,'\0');input.read(n.data(),n.size());if(!input)throw std::runtime_error("Catalog spool incomplete");
        auto parent=b.directoryPath({s.root,s.parent});if(!parent){++b.unresolved;continue;}
        auto path=s.parent==b.roots[s.root].rootFile?rootOffsets[s.root]:b.dirs.at({s.root,s.parent}).offset;
        auto off=b.stringPair(n);rows[accepted++]={s.file,s.parent,s.id,s.size,s.modified,off,path,s.root,(uint16_t)(s.flags|s.length)};
    }
    b.strings.close();Header h{};memcpy(h.magic,"DFLOWIDX",8);h.version=format;h.roots=(uint32_t)b.roots.size();h.generation=stamp();h.rows=accepted;h.total=accepted;h.maximumId=b.maximumId;
    h.records=align(sizeof(Header)+h.roots*sizeof(RootDisk));h.strings=align(h.records+h.rows*sizeof(Row));h.stringsSize=b.stringBytes;
    uint64_t at=align(h.strings+h.stringsSize);for(auto& offset:h.order){offset=at;at=align(at+h.rows*sizeof(uint32_t));}h.bytes=std::max<uint64_t>(4096,at);
    const auto target=b.directory/L"catalog.index.tmp";Mapping mapped;mapped.open(target,true,h.bytes);memcpy(mapped.data,&h,sizeof(h));
    auto* rd=(RootDisk*)(mapped.data+sizeof(Header));for(size_t i=0;i<b.roots.size();++i){wcsncpy_s(rd[i].path,b.roots[i].path.c_str(),3);rd[i].serial=b.roots[i].serial;rd[i].file=b.roots[i].rootFile;rd[i].journal=b.roots[i].journal;rd[i].cursor=b.roots[i].nextUsn;}
    memcpy(mapped.data+h.records,rows,h.rows*sizeof(Row));recordMap.close();
    std::ifstream strings(b.stringPath,std::ios::binary);strings.read((char*)mapped.data+h.strings,h.stringsSize);if(!strings)throw std::runtime_error("Catalog string copy failed");
    auto* records=(Row*)(mapped.data+h.records);
    for(size_t index=0;index<orders;++index){
        if(stopped())throw std::runtime_error("Native indexing stopped");progress(L"建立快速排序 "+std::to_wstring(index+1)+L" / "+std::to_wstring(orders));
        auto* ids=(uint32_t*)(mapped.data+h.order[index]);for(uint64_t i=0;i<h.rows;++i)ids[i]=(uint32_t)i;
        uint64_t comparisons=0;
        std::sort(ids,ids+h.rows,[&](uint32_t left,uint32_t right){if((++comparisons&65535)==0&&stopped())throw std::runtime_error("Native indexing stopped");const auto& a=records[left];const auto& c=records[right];int relation=0;
            if(index==ByFile||index==ByParent){auto ak=Key{a.root,index==ByFile?a.file:a.parent},bk=Key{c.root,index==ByFile?c.file:c.parent};relation=ak<bk?-1:ak>bk?1:0;
                if(!relation&&index==ByFile){relation=a.parent<c.parent?-1:a.parent>c.parent?1:0;if(!relation)relation=name(mapped,h,a).compare(name(mapped,h,c));}}
            else if(index==ByModified)relation=a.modified<c.modified?-1:a.modified>c.modified?1:0;
            else if(index==BySize){bool ka=(a.flags&knownBit)&&!(a.flags&folderBit),kc=(c.flags&knownBit)&&!(c.flags&folderBit);if(ka!=kc)relation=ka?1:-1;else if(ka)relation=a.size<c.size?-1:a.size>c.size?1:0;}
            else if(index==ByPath)relation=joined(prefix(mapped,h,a,true),name(mapped,h,a,true),prefix(mapped,h,c,true),name(mapped,h,c,true));
            else if(index==ByType){bool da=(a.flags&folderBit)!=0,dc=(c.flags&folderBit)!=0;if(da!=dc)relation=da?-1:1;else if(!da)relation=extension(name(mapped,h,a,true)).compare(extension(name(mapped,h,c,true)));}
            else if(index!=ById)relation=name(mapped,h,a,true).compare(name(mapped,h,c,true));
            return relation?relation<0:a.id<c.id;
        });
    }
    auto* byFile=(uint32_t*)(mapped.data+h.order[ByFile]);
    for(uint64_t i=1;i<h.rows;++i){auto& a=records[byFile[i-1]];auto& c=records[byFile[i]];if(a.root==c.root&&a.file==c.file&&a.parent==c.parent&&name(mapped,h,a)==name(mapped,h,c)){c.flags|=duplicateBit;--h.total;}}
    memcpy(mapped.data,&h,sizeof(h));lookup(b.directory,mapped,h,stopped,progress);
    if(!FlushViewOfFile(mapped.data,0)||!FlushFileBuffers(mapped.file))fail("Flush native catalog");mapped.close();
    // A mapped Windows file cannot be replaced while readers use its section.
    // Publish immutable generation files, then atomically switch a tiny pointer.
    if(!MoveFileExW(target.c_str(),snapshotName(b.directory,h.generation).c_str(),MOVEFILE_WRITE_THROUGH))fail("Publish native catalog generation");
    publishGeneration(b.directory,h.generation);
    // Only owned temporary build files are removed, never user databases.
    input.close();strings.close();DeleteFileW(b.spoolPath.c_str());DeleteFileW(b.stringPath.c_str());DeleteFileW(recordsPath.c_str());
    return b.unresolved;
}
namespace {
struct DeltaHeader {uint32_t length=0,checksum=0;uint64_t generation=0;};
struct DeltaFile {uint64_t file=0;uint16_t root=0,reserved=0;uint32_t count=0;};
struct DeltaEntry {uint64_t parent=0,size=0,modified=0,id=0;uint16_t flags=0,length=0;uint32_t reserved=0;};
template<class T>void append(std::vector<unsigned char>& b,const T& v){const auto* p=(const unsigned char*)&v;b.insert(b.end(),p,p+sizeof(T));}
template<class T>T take(const std::vector<unsigned char>& b,size_t& at){if(at>b.size()||sizeof(T)>b.size()-at)throw std::runtime_error("Catalog change bounds");T v{};memcpy(&v,b.data()+at,sizeof(T));at+=sizeof(T);return v;}
bool same(const SearchQuery& a,const SearchQuery& b){return a.text==b.text&&a.sort==b.sort&&a.descending==b.descending&&a.matchCase==b.matchCase&&a.matchPath==b.matchPath&&a.after.has_value()==b.after.has_value()&&(!a.after||a.after->id==b.after->id);}
bool less(const SearchItem& a,const SearchItem& b,const SearchQuery& q){
    int relation=0;
    if(q.sort==SearchSort::Size||q.sort==SearchSort::Modified){bool ka=q.sort==SearchSort::Size?(a.sizeKnown&&!a.folder):a.modified!=0,kb=q.sort==SearchSort::Size?(b.sizeKnown&&!b.folder):b.modified!=0;if(ka!=kb)return ka;
        uint64_t va=ka?(q.sort==SearchSort::Size?a.size:a.modified):0,vb=kb?(q.sort==SearchSort::Size?b.size:b.modified):0;relation=va<vb?-1:va>vb?1:0;}
    else if(q.sort!=SearchSort::Id){std::string av,bv;
        if(q.sort==SearchSort::Type){av=a.folder?"0folder":"1:"+std::string(extension(lower(bytes(a.name))));bv=b.folder?"0folder":"1:"+std::string(extension(lower(bytes(b.name))));}
        else{av=lower(bytes(q.sort==SearchSort::Path?a.path:a.name));bv=lower(bytes(q.sort==SearchSort::Path?b.path:b.name));}
        relation=av.compare(bv);
    }
    if(!relation)relation=a.id<b.id?-1:a.id>b.id?1:0;return q.descending?relation>0:relation<0;
}
}
void appendCatalogChange(const fs::path& data,uint64_t generation,uint16_t root,uint64_t file,const std::vector<CatalogEntry>& entries,bool durable){
    if(entries.size()>32768)throw std::runtime_error("Too many NTFS hardlinks");
    std::vector<unsigned char> payload;append(payload,DeltaFile{file,root,0,(uint32_t)entries.size()});
    for(const auto& e:entries){if(e.name.empty()||e.name.size()>1024)throw std::runtime_error("Catalog update name bounds");
        append(payload,DeltaEntry{e.parent,e.size,e.modified,e.id,(uint16_t)((e.folder?folderBit:0)|(e.sizeKnown?knownBit:0)),(uint16_t)e.name.size(),0});payload.insert(payload.end(),e.name.begin(),e.name.end());}
    DeltaHeader header{(uint32_t)payload.size(),checksum(payload.data(),payload.size()),generation};
    HANDLE handle=CreateFileW((data/L"catalog.delta").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,0,nullptr);
    if(handle==INVALID_HANDLE_VALUE)fail("Open catalog changes");
    DWORD written=0;bool ok=WriteFile(handle,&header,sizeof(header),&written,nullptr)&&written==sizeof(header)&&WriteFile(handle,payload.data(),(DWORD)payload.size(),&written,nullptr)&&written==payload.size()&&(!durable||FlushFileBuffers(handle));DWORD error=GetLastError();CloseHandle(handle);if(!ok){SetLastError(error);fail("Commit catalog changes");}
}
void flushCatalogChanges(const fs::path& data){
    HANDLE handle=CreateFileW((data/L"catalog.delta").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);if(handle==INVALID_HANDLE_VALUE)fail("Open catalog changes for commit");
    bool ok=FlushFileBuffers(handle);DWORD error=GetLastError();CloseHandle(handle);if(!ok){SetLastError(error);fail("Flush catalog change batch");}
}
struct NativeCatalog::Impl {
    fs::path data;Mapping mapped,lookupMap;Header header{};std::vector<ntfs::Root> rootInfo;
    FILETIME identity{};uint64_t selectedGeneration=0,deltaIdentity=0,deltaOffset=0,maximumId=0,objectCount=0;
    fs::path snapshot;
    std::map<Key,std::vector<CatalogEntry>> updates;
    std::unordered_set<uint32_t> moved;
    std::unordered_map<Key,std::string,KeyHash> pathCache;
    std::map<Key,std::set<Key>> directoryChildren;
    bool directoryChanges=false,queryPending=false;
    SearchQuery request;size_t limit=0;uint64_t position=0;
    std::vector<SearchItem> overlayMatches,results;
    bool active=false;
    std::optional<std::vector<uint32_t>> candidates;
    Impl(const fs::path& p):data(p){}
    const Row* records()const{return (const Row*)(mapped.data+header.records);}
    const uint32_t* order(size_t i)const{return (const uint32_t*)(mapped.data+header.order[i]);}
    bool load(){
        if(fs::exists(data/L"catalog.disabled"))return false;
        const auto generation=currentGeneration(data);auto selected=snapshotName(data,generation);
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if(!GetFileAttributesExW(selected.c_str(),GetFileExInfoStandard,&attributes))return mapped.data!=nullptr;
        if(mapped.data&&selectedGeneration==generation&&CompareFileTime(&identity,&attributes.ftLastWriteTime)==0)return true;
        if(active&&queryPending)return mapped.data!=nullptr; // Complete this query against its stable base.
        mapped.open(selected);if(mapped.length<sizeof(Header))throw std::runtime_error("Catalog header truncated");memcpy(&header,mapped.data,sizeof(header));
        if(memcmp(header.magic,"DFLOWIDX",8)||header.version!=format||header.roots>32||!header.roots||header.rows>UINT32_MAX||header.bytes!=mapped.length||header.total>header.rows)throw std::runtime_error("Invalid catalog header");
        auto range=[&](uint64_t offset,uint64_t count,uint64_t size){return offset<=mapped.length&&count<=((mapped.length-offset)/size);};
        if(!range(sizeof(Header),header.roots,sizeof(RootDisk))||!range(header.records,header.rows,sizeof(Row))||!range(header.strings,header.stringsSize,1))throw std::runtime_error("Catalog section bounds");
        for(auto offset:header.order)if(!range(offset,header.rows,sizeof(uint32_t)))throw std::runtime_error("Catalog sort bounds");
        // Validate individual rows when accessed. Walking every record here
        // would page hundreds of MiB in before even an empty first query.
        if(header.maximumId>INT64_MAX)throw std::runtime_error("Catalog object ID bounds");
        if(generation&&header.generation!=generation)throw std::runtime_error("Catalog pointer generation mismatch");
        rootInfo=roots(mapped,header);identity=attributes.ftLastWriteTime;selectedGeneration=generation;snapshot=selected;maximumId=header.maximumId;objectCount=header.total;
        deltaOffset=deltaIdentity=0;updates.clear();moved.clear();pathCache.clear();directoryChildren.clear();lookupMap.close();candidates.reset();directoryChanges=false;active=false;queryPending=false;return true;
    }
    std::pair<const uint32_t*,const uint32_t*> range(Key key,Order which)const{
        auto* first=order(which);auto* end=first+header.rows;
        auto value=[&](uint32_t id){if(id>=header.rows)throw std::runtime_error("Catalog row ID bounds");auto& r=records()[id];return Key{r.root,which==ByFile?r.file:r.parent};};
        auto low=std::lower_bound(first,end,key,[&](uint32_t id,Key k){return value(id)<k;});
        auto high=std::upper_bound(low,end,key,[&](Key k,uint32_t id){return k<value(id);});return {low,high};
    }
    std::vector<CatalogEntry> file(Key key)const{
        auto updated=updates.find(key);if(updated!=updates.end())return updated->second;
        std::vector<CatalogEntry> result;auto [first,last]=range(key,ByFile);
        for(;first!=last;++first){const auto& r=records()[*first];if(!(r.flags&duplicateBit))result.push_back(entry(mapped,header,r));}return result;
    }
    void subtree(Key key){
        std::vector<Key> queue{key};std::unordered_set<Key,KeyHash> seen;
        for(size_t at=0;at<queue.size();++at){if(!seen.insert(queue[at]).second)continue;auto [first,last]=range(queue[at],ByParent);
            for(;first!=last;++first){if(moved.insert(*first).second){auto& r=records()[*first];if(r.flags&folderBit)queue.push_back({r.root,r.file});}}
            auto children=directoryChildren.find(queue[at]);if(children!=directoryChildren.end())for(auto child:children->second)queue.push_back(child);
        }
    }
    void replace(Key key,std::vector<CatalogEntry> current){
        auto previous=file(key);std::set<uint64_t> assigned;bool folder=false;
        if(previous.empty()&&current.empty())return;
        // Reserve exact aliases before giving a renamed/new alias an old ID.
        for(auto& e:current){auto found=std::find_if(previous.begin(),previous.end(),[&](const auto& old){return old.parent==e.parent&&old.name==e.name;});if(!e.id&&found!=previous.end())e.id=found->id;if(e.id)assigned.insert(e.id);}
        const bool namespaceChanged=current.size()!=previous.size()||std::any_of(current.begin(),current.end(),[&](const auto& e){return std::none_of(previous.begin(),previous.end(),[&](const auto& p){return p.parent==e.parent&&p.name==e.name;});});
        for(auto& e:current){e.file=key.second;e.root=key.first;folder|=e.folder;
            auto found=std::find_if(previous.begin(),previous.end(),[&](const auto& old){return old.parent==e.parent&&old.name==e.name&&!assigned.contains(old.id);});
            if(found==previous.end())found=std::find_if(previous.begin(),previous.end(),[&](const auto& old){return !assigned.contains(old.id);});
            if(!e.id)e.id=found!=previous.end()?found->id:++maximumId;maximumId=std::max(maximumId,e.id);assigned.insert(e.id);
        }
        folder|=std::any_of(previous.begin(),previous.end(),[](const auto& e){return e.folder;});
        for(const auto& e:previous)if(e.folder){auto children=directoryChildren.find({e.root,e.parent});if(children!=directoryChildren.end())children->second.erase(key);}
        for(const auto& e:current)if(e.folder)directoryChildren[{e.root,e.parent}].insert(key);
        objectCount=objectCount-previous.size()+current.size();updates[key]=std::move(current);
        if(folder&&namespaceChanged){directoryChanges=true;subtree(key);pathCache.clear();}
    }
    void refresh(){
        if(!load()||(active&&queryPending))return;
        SharedInput input(data/L"catalog.delta");if(input.file==INVALID_HANDLE_VALUE)return;auto length=input.size();
        BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(input.file,&info))fail("Read catalog log identity");
        const auto identity=(uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
        if(identity!=deltaIdentity||length<deltaOffset){deltaOffset=0;deltaIdentity=identity;}
        input.seek(deltaOffset);
        // Replay final state per file, not every intermediate metadata update.
        // Directory-child lookup is indexed rather than scanning all updates.
        std::map<Key,std::vector<CatalogEntry>> latest;
        while(deltaOffset<length){DeltaHeader h{};if(input.read(&h,sizeof(h))!=sizeof(h))break;
            if(h.length<sizeof(DeltaFile)||h.length>64*1024*1024)throw std::runtime_error("Catalog update block bounds");
            std::vector<unsigned char> payload(h.length);if(input.read(payload.data(),payload.size())!=payload.size())break;
            if(checksum(payload.data(),payload.size())!=h.checksum)throw std::runtime_error("Catalog update checksum mismatch");
            if(h.generation==header.generation){size_t at=0;auto f=take<DeltaFile>(payload,at);if(f.root>=header.roots||f.count>32768)throw std::runtime_error("Catalog update file bounds");std::vector<CatalogEntry> entries;
                for(uint32_t i=0;i<f.count;++i){auto e=take<DeltaEntry>(payload,at);if(!e.length||e.length>1024||e.length>payload.size()-at)throw std::runtime_error("Catalog update string bounds");
                    entries.push_back({f.file,e.parent,e.id,e.size,e.modified,f.root,(e.flags&folderBit)!=0,(e.flags&knownBit)!=0,std::string((const char*)payload.data()+at,e.length)});at+=e.length;}
                if(at!=payload.size())throw std::runtime_error("Catalog update trailing bytes");latest[{f.root,f.file}]=std::move(entries);
            }
            deltaOffset+=sizeof(h)+h.length;
        }
        for(auto& [key,entries]:latest)replace(key,std::move(entries));
    }
    std::string directory(Key key,std::set<Key>& visiting)const{
        if(key.first>=rootInfo.size())return {};
        if(key.second==rootInfo[key.first].rootFile)return bytes(rootInfo[key.first].path);
        if(visiting.size()>512||!visiting.insert(key).second)return {};
        auto candidates=file(key);auto dir=std::find_if(candidates.begin(),candidates.end(),[](const auto& e){return e.folder;});if(dir==candidates.end())return {};
        auto parent=directory({dir->root,dir->parent},visiting);if(parent.empty())return {};return parent+dir->name+"\\";
    }
    std::string path(const CatalogEntry& e){
        Key parent{e.root,e.parent};auto cached=pathCache.find(parent);if(cached!=pathCache.end())return cached->second+e.name;
        std::set<Key> visiting;auto prefix=directory(parent,visiting);if(prefix.empty())return {};
        if(pathCache.size()>=16384)pathCache.clear();pathCache.emplace(parent,prefix);return prefix+e.name;
    }
    std::string path(const Row& r){if(!directoryChanges)return std::string(prefix(mapped,header,r))+std::string(name(mapped,header,r));return path(entry(mapped,header,r));}
    SearchItem item(const CatalogEntry& e){SearchItem r;r.id=(int64_t)e.id;r.name=text(e.name);r.path=text(path(e));r.folder=e.folder;r.size=e.size;r.sizeKnown=e.sizeKnown;r.modified=e.modified;return r;}
    SearchItem item(const Row& e){SearchItem r;r.id=(int64_t)e.id;r.name=text(name(mapped,header,e));r.path=text(path(e));r.folder=(e.flags&folderBit)!=0;r.size=e.size;r.sizeKnown=(e.flags&knownBit)!=0;r.modified=e.modified;return r;}
    std::optional<std::vector<uint32_t>> candidateRows(const std::vector<std::string>& hints){
        if(hints.empty())return {};
        try{if(!lookupMap.data){auto selected=lookupName(data,header.generation);if(!fs::exists(selected))selected=data/L"catalog.lookup";lookupMap.open(selected);}}catch(...){return {};}
        if(lookupMap.length<sizeof(LookupHeader))return {};auto& h=*(const LookupHeader*)lookupMap.data;
        if(memcmp(h.magic,"DFLOOKUP",8)||h.generation!=header.generation||h.rows!=header.rows||h.bytes!=lookupMap.length||h.keys>0x120000||h.directory>h.bytes||h.keys>(h.bytes-h.directory)/sizeof(LookupKey)||h.payload>h.bytes)return {};
        const auto* first=(const LookupKey*)(lookupMap.data+h.directory);const auto* last=first+h.keys;const LookupKey* chosen=nullptr;
        for(const auto& hint:hints)for(auto key:tokensFor(lower(hint))){auto at=std::lower_bound(first,last,key,[](const auto& entry,uint32_t k){return entry.key<k;});if(at==last||at->key!=key)return std::vector<uint32_t>{};if(!chosen||at->count<chosen->count)chosen=at;}
        if(!chosen||chosen->count>200000)return {};
        if(chosen->offset>h.bytes-h.payload||chosen->length>h.bytes-h.payload-chosen->offset)throw std::runtime_error("Catalog lookup posting bounds");
        auto* p=lookupMap.data+h.payload+chosen->offset;auto* end=p+chosen->length;uint64_t row=0;std::vector<uint32_t> ids;ids.reserve(chosen->count);
        while(p<end){uint32_t delta=0;unsigned shift=0;for(;;){if(p==end||shift>28)throw std::runtime_error("Catalog lookup variable integer bounds");unsigned char b=*p++;delta|=uint32_t(b&127)<<shift;if(!(b&128))break;shift+=7;}row+=delta;if(row>=header.rows)throw std::runtime_error("Catalog lookup row bounds");ids.push_back((uint32_t)row);}
        if(ids.size()!=chosen->count)throw std::runtime_error("Catalog lookup posting count");return ids;
    }
};
NativeCatalog::NativeCatalog(const fs::path& p):impl_(std::make_unique<Impl>(p)){}
NativeCatalog::~NativeCatalog()=default;
bool NativeCatalog::available(){return impl_->load();}
bool NativeCatalog::pending()const{return impl_->queryPending;}
uint64_t NativeCatalog::total()const{return impl_->objectCount;}
uint64_t NativeCatalog::generation()const{return impl_->header.generation;}
fs::path NativeCatalog::snapshotFile()const{return impl_->snapshot;}
std::vector<ntfs::Root> NativeCatalog::volumes(){return impl_->rootInfo;}
void NativeCatalog::repairIncompleteLog(){
    auto& b=*impl_;b.refresh();std::error_code ec;auto length=fs::file_size(b.data/L"catalog.delta",ec);if(ec||length==b.deltaOffset)return;
    HANDLE handle=CreateFileW((b.data/L"catalog.delta").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);if(handle==INVALID_HANDLE_VALUE)fail("Repair incomplete catalog update");
    LARGE_INTEGER at{};at.QuadPart=(LONGLONG)b.deltaOffset;bool ok=SetFilePointerEx(handle,at,nullptr,FILE_BEGIN)&&SetEndOfFile(handle)&&FlushFileBuffers(handle);DWORD error=GetLastError();CloseHandle(handle);if(!ok){SetLastError(error);fail("Truncate incomplete catalog update");}
}
std::vector<CatalogEntry> NativeCatalog::file(uint16_t root,uint64_t reference){impl_->refresh();return impl_->file({root,reference});}
void NativeCatalog::replace(uint16_t root,uint64_t reference,const std::vector<CatalogEntry>& e){impl_->replace({root,reference},e);}
void NativeCatalog::each(const std::function<void(const CatalogEntry&)>& visit)const{
    auto& b=*impl_;for(uint64_t i=0;i<b.header.rows;++i){auto& r=b.records()[i];if(!(r.flags&duplicateBit)&&!b.updates.contains({r.root,r.file}))visit(entry(b.mapped,b.header,r));}
    for(const auto& [key,rows]:b.updates)for(const auto& r:rows)visit(r);
}
std::vector<SearchItem> NativeCatalog::query(const SearchQuery& query,size_t limit,const CatalogPredicate& predicate,const std::vector<std::string>& hints){
    auto& b=*impl_;const bool continuing=b.active&&b.queryPending&&same(b.request,query)&&b.limit==limit;
    if(!continuing){b.active=false;b.queryPending=false;b.refresh();b.request=query;b.limit=limit;b.position=0;b.results.clear();b.overlayMatches.clear();
        auto accept=[&](const CatalogEntry& e){auto folded=lower(e.name);CatalogView v{e.name,folded,e.folder,e.sizeKnown,e.size,e.modified,[&]{return b.path(e);}};
            if(predicate(v)){auto item=b.item(e);if(!item.path.empty()&&(!query.after||less(*query.after,item,query)))b.overlayMatches.push_back(std::move(item));}};
        for(const auto& [key,entries]:b.updates)for(const auto& e:entries)accept(e);
        if(query.sort==SearchSort::Path)for(auto i:b.moved){auto& r=b.records()[i];if(!(r.flags&duplicateBit)&&!b.updates.contains({r.root,r.file}))accept(entry(b.mapped,b.header,r));}
        std::sort(b.overlayMatches.begin(),b.overlayMatches.end(),[&](const auto& a,const auto& c){return less(a,c,query);});
    }
    if(!continuing){b.candidates=b.candidateRows(hints);
        if(b.candidates){
            std::erase_if(*b.candidates,[&](uint32_t id){auto& r=b.records()[id];return (r.flags&duplicateBit)||b.updates.contains({r.root,r.file})||(query.sort==SearchSort::Path&&b.moved.contains(id));});
            std::sort(b.candidates->begin(),b.candidates->end(),[&](uint32_t left,uint32_t right){const auto& a=b.records()[left];const auto& c=b.records()[right];int relation=0;
                if(query.sort==SearchSort::Modified||query.sort==SearchSort::Size){bool ka=query.sort==SearchSort::Modified?a.modified!=0:((a.flags&knownBit)&&!(a.flags&folderBit));bool kc=query.sort==SearchSort::Modified?c.modified!=0:((c.flags&knownBit)&&!(c.flags&folderBit));if(ka!=kc)return ka;
                    uint64_t av=ka?(query.sort==SearchSort::Modified?a.modified:a.size):0,cv=kc?(query.sort==SearchSort::Modified?c.modified:c.size):0;relation=av<cv?-1:av>cv?1:0;}
                else if(query.sort==SearchSort::Path)relation=joined(prefix(b.mapped,b.header,a,true),name(b.mapped,b.header,a,true),prefix(b.mapped,b.header,c,true),name(b.mapped,b.header,c,true));
                else if(query.sort==SearchSort::Type){bool da=(a.flags&folderBit)!=0,dc=(c.flags&folderBit)!=0;if(da!=dc)relation=da?-1:1;else if(!da)relation=extension(name(b.mapped,b.header,a,true)).compare(extension(name(b.mapped,b.header,c,true)));}
                else if(query.sort!=SearchSort::Id)relation=name(b.mapped,b.header,a,true).compare(name(b.mapped,b.header,c,true));
                if(!relation)relation=a.id<c.id?-1:a.id>c.id?1:0;return query.descending?relation>0:relation<0;
            });
        }}
    b.active=true;b.queryPending=false;const auto began=std::chrono::steady_clock::now();
    Order which=query.sort==SearchSort::Path?ByPath:query.sort==SearchSort::Size?BySize:query.sort==SearchSort::Type?ByType:query.sort==SearchSort::Modified?ByModified:query.sort==SearchSort::Id?ById:ByName;
    const auto* sorted=b.order(which);uint64_t split=0;
    if(query.sort==SearchSort::Size||query.sort==SearchSort::Modified){
        auto known=[&](uint32_t id){const auto& r=b.records()[id];return query.sort==SearchSort::Size?((r.flags&knownBit)&&!(r.flags&folderBit)):r.modified!=0;};
        split=std::partition_point(sorted,sorted+b.header.rows,[&](uint32_t id){return !known(id);})-sorted;
    }
    auto rowAt=[&](uint64_t ordinal){
        if(query.sort==SearchSort::Size||query.sort==SearchSort::Modified){auto known=b.header.rows-split;ordinal=ordinal<known?(query.descending?b.header.rows-1-ordinal:split+ordinal):(query.descending?split-1-(ordinal-known):ordinal-known);}
        else if(query.descending)ordinal=b.header.rows-1-ordinal;
        auto i=sorted[ordinal];if(i>=b.header.rows)throw std::runtime_error("Catalog sorted row bounds");return i;
    };
    if(!b.candidates&&!continuing&&query.after&&!(query.sort==SearchSort::Path&&!b.moved.empty())){
        uint64_t lo=0,hi=b.header.rows;
        while(lo<hi){auto mid=lo+(hi-lo)/2;auto row=b.item(b.records()[rowAt(mid)]);if(less(*query.after,row,query))hi=mid;else lo=mid+1;}b.position=lo;
    }
    const uint64_t scanRows=b.candidates?b.candidates->size():b.header.rows;
    while(b.position<scanRows&&b.results.size()<limit){
        if((b.position%64)==0&&std::chrono::steady_clock::now()-began>=std::chrono::milliseconds(100)){b.queryPending=true;break;}
        uint32_t i=b.candidates?(*b.candidates)[b.position++]:rowAt(b.position++);const auto& r=b.records()[i];
        if((r.flags&duplicateBit)||b.updates.contains({r.root,r.file})||(query.sort==SearchSort::Path&&b.moved.contains(i)))continue;
        CatalogView view{name(b.mapped,b.header,r),name(b.mapped,b.header,r,true),(r.flags&folderBit)!=0,(r.flags&knownBit)!=0,r.size,r.modified,[&]{return b.path(r);}};
        if(!predicate(view))continue;
        auto item=b.item(r);if(item.path.empty()||(query.after&&!less(*query.after,item,query)))continue;
        b.results.push_back(std::move(item));
    }
    if(b.queryPending)return b.results;
    b.results.insert(b.results.end(),b.overlayMatches.begin(),b.overlayMatches.end());
    std::sort(b.results.begin(),b.results.end(),[&](const auto& a,const auto& c){return less(a,c,query);});if(b.results.size()>limit)b.results.resize(limit);b.active=false;return b.results;
}
void catalogStatus(const fs::path& data,const SearchStatus& s,DWORD pid,const std::vector<ntfs::Root>& roots){
    nlohmann::json j{{"engine","mapped-ntfs"},{"pid",pid},{"total",s.total},{"revision",s.revision},{"building",s.building},{"complete",s.complete},{"limited",s.limited},{"roots",s.roots},{"completedRoots",s.completedRoots},{"readRecords",s.readRecords},{"materializedRecords",s.materializedRecords},{"elapsedSeconds",s.elapsedSeconds},{"estimatedRecords",s.estimatedRecords},{"stage",bytes(s.stage)},{"message",bytes(s.message)},{"volumes",nlohmann::json::array()}};
    for(const auto& r:roots)j["volumes"].push_back({{"serial",r.serial},{"journal",r.journal},{"cursor",r.nextUsn}});
    auto temporary=data/L"catalog.status.tmp";{std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out<<j.dump();out.flush();if(!out)throw std::runtime_error("Catalog status write failed");}
    if(!replaceFile(temporary,data/L"catalog.status",MOVEFILE_REPLACE_EXISTING))fail("Publish catalog status");
}
SearchStatus NativeCatalog::status(){
    if(fs::exists(impl_->data/L"catalog.disabled"))return {};
    SharedInput input(impl_->data/L"catalog.status");if(input.file==INVALID_HANDLE_VALUE)return {};auto length=input.size();if(!length||length>1024*1024)throw std::runtime_error("Catalog status bounds");std::string content((size_t)length,'\0');if(input.read(content.data(),content.size())!=content.size())throw std::runtime_error("Catalog status incomplete");auto j=nlohmann::json::parse(content);SearchStatus s;
    s.total=j.value("total",int64_t{});s.revision=j.value("revision",int64_t{});s.building=j.value("building",false);s.complete=j.value("complete",false);s.limited=j.value("limited",false);s.roots=j.value("roots",0);s.completedRoots=j.value("completedRoots",0);s.readRecords=j.value("readRecords",int64_t{});s.materializedRecords=j.value("materializedRecords",int64_t{});s.elapsedSeconds=j.value("elapsedSeconds",int64_t{});s.estimatedRecords=j.value("estimatedRecords",int64_t{});s.stage=text(j.value("stage",std::string{}));s.message=text(j.value("message",std::string{}));
    if(!s.total){SharedInput existing(snapshotName(impl_->data,currentGeneration(impl_->data)));Header h{};if(existing.file!=INVALID_HANDLE_VALUE&&existing.read(&h,sizeof(h))==sizeof(h)&&!memcmp(h.magic,"DFLOWIDX",8)&&h.version==format)s.total=(int64_t)h.total;}
    DWORD pid=j.value("pid",DWORD{});HANDLE p=pid?OpenProcess(SYNCHRONIZE,FALSE,pid):nullptr;if(pid&&((p&&WaitForSingleObject(p,0)==WAIT_OBJECT_0)||(!p&&GetLastError()==ERROR_INVALID_PARAMETER))){s.building=false;s.message=L"索引进程已停止，已有索引可查";s.stage=L"stopped";}if(p)CloseHandle(p);return s;
}
void buildCatalogLookup(const fs::path& data,const std::function<bool()>& stopped,const std::function<void(const std::wstring&)>& progress){
    NativeCatalog validated(data);if(!validated.available())throw std::runtime_error("Native snapshot unavailable for lookup construction");
    Mapping m;m.open(validated.snapshotFile());Header h{};memcpy(&h,m.data,sizeof(h));if(h.generation!=validated.generation()||h.bytes!=m.length)throw std::runtime_error("Native snapshot changed during lookup construction");
    lookup(data,m,h,stopped,progress);
}
bool runNativeCatalog(const fs::path& data,const std::vector<std::wstring>& configured,HANDLE stop,HANDLE parent){
    std::vector<std::wstring> paths=configured;
    if(paths.empty()){
        DWORD mask=GetLogicalDrives();for(unsigned i=0;i<26;++i)if(mask&(1u<<i)){std::wstring path=L"A:\\";path[0]+=i;if(GetDriveTypeW(path.c_str())==DRIVE_FIXED)paths.push_back(path);}
    }
    std::vector<std::unique_ptr<ntfs::Volume>> volumes;std::vector<ntfs::Root> roots;
    try{
        if(paths.empty()||paths.size()>32)throw std::runtime_error("Native root count");
        for(auto path:paths){std::replace(path.begin(),path.end(),L'/',L'\\');if(path.size()==2)path+=L'\\';
            if(std::any_of(roots.begin(),roots.end(),[&](const auto& r){return _wcsicmp(path.c_str(),r.path.c_str())==0;}))continue;
            auto volume=std::make_unique<ntfs::Volume>(path);roots.push_back(volume->info);volumes.push_back(std::move(volume));}
    }catch(...){
        if(fs::exists(data/L"catalog.index")||fs::exists(data/L"catalog.current")){std::ofstream disabled(data/L"catalog.disabled");disabled<<"Current roots or permissions require the directory index";}
        return false;
    }
    DeleteFileW((data/L"catalog.disabled").c_str());
    fs::create_directories(data);
    auto stopped=[&]{return WaitForSingleObject(stop,0)==WAIT_OBJECT_0||(parent&&WaitForSingleObject(parent,0)==WAIT_OBJECT_0);};
    SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_END);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    const auto began=GetTickCount64();SearchStatus status;status.roots=(int)roots.size();status.building=true;status.stage=L"records";status.message=L"原生 NTFS 索引 · 新快照构建期间已有结果可查";
    for(auto& r:roots)status.estimatedRecords+=r.records;
    uint64_t revision=stamp()&INT64_MAX;auto lastPublished=GetTickCount64();
    auto publish=[&](bool force=false){if(!force&&GetTickCount64()-lastPublished<1000)return;status.elapsedSeconds=(GetTickCount64()-began)/1000;status.revision=(int64_t)revision;catalogStatus(data,status,GetCurrentProcessId(),roots);lastPublished=GetTickCount64();};
    // Exclude only this tool's own catalog/SQLite index files, by physical parent ID.
    Key own{UINT16_MAX,0};HANDLE directory=CreateFileW(data.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(directory!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION info{};wchar_t path[32768]{};if(GetFileInformationByHandle(directory,&info)&&GetFinalPathNameByHandleW(directory,path,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS)){
        std::wstring actual(path);if(actual.starts_with(L"\\\\?\\"))actual.erase(0,4);
        for(uint16_t i=0;i<roots.size();++i)if(_wcsnicmp(actual.c_str(),roots[i].path.c_str(),3)==0)own={i,(uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow};
    }CloseHandle(directory);}
    auto ownName=[&](uint16_t root,uint64_t parent,std::wstring_view n){if(Key{root,parent}!=own)return false;std::wstring folded(n);std::transform(folded.begin(),folded.end(),folded.begin(),towlower);return folded.starts_with(L"catalog.")||folded==L"files.db"||folded.starts_with(L"files.db-");};
    try{
        NativeCatalog reader(data);bool rebuild=true;const bool requestedRebuild=fs::exists(data/L"catalog.rebuild");
        try{
            if(reader.available()){
                auto saved=reader.volumes();rebuild=saved.size()!=roots.size();
                if(!rebuild)for(size_t i=0;i<roots.size();++i)if(saved[i].serial!=roots[i].serial||saved[i].journal!=roots[i].journal||saved[i].path!=roots[i].path)rebuild=true;
                if(requestedRebuild)rebuild=true;
                if(!rebuild){
                    reader.repairIncompleteLog();
                    std::ifstream input(data/L"catalog.status");nlohmann::json previous;if(input)input>>previous;
                    for(size_t i=0;i<roots.size();++i){roots[i].nextUsn=saved[i].nextUsn;if(previous.contains("volumes")&&previous["volumes"].size()==roots.size()){
                        const auto& p=previous["volumes"][i];if(p.value("serial",DWORD{})==roots[i].serial&&p.value("journal",uint64_t{})==roots[i].journal)roots[i].nextUsn=p.value("cursor",roots[i].nextUsn);}}
                    for(size_t i=0;i<volumes.size();++i){std::vector<ntfs::Change> changes;volumes[i]->changes(roots[i].nextUsn,changes);}
                }
            }
        }catch(...){rebuild=true;}
        publish(true);
        if(rebuild){
            // Validation may have installed an expired saved cursor before
            // discovering a journal gap. A fresh MFT snapshot must start at
            // the current journal head, or it immediately rebuilds again.
            for(size_t i=0;i<roots.size();++i)roots[i].nextUsn=volumes[i]->info.nextUsn;
            CatalogBuilder builder(data,roots);status.readRecords=0;
            for(uint16_t root=0;root<volumes.size();++root){
                struct Meta {uint64_t file=0,size=0,modified=0;DWORD attributes=0;uint32_t known=0;};
                if(roots[root].records>SIZE_MAX/sizeof(Meta))throw std::runtime_error("NTFS metadata table exceeds address space");
                std::vector<Meta> metadata((size_t)roots[root].records);
                const auto namesPath=data/L"catalog.aliases.tmp";std::ofstream names(namesPath,std::ios::binary|std::ios::trunc);
                uint64_t aliasCount=0;
                volumes[root]->enumerate([&](const ntfs::Record& record){
                    auto ordinal=record.file&((uint64_t{1}<<48)-1);
                    auto reference=record.base?record.base:record.file;if(reference==roots[root].rootFile)return;
                    auto primary=reference&((uint64_t{1}<<48)-1);
                    if(primary<metadata.size()){
                        auto& m=metadata[(size_t)primary];
                        if(m.file!=reference)m={reference,0,0,0,0};
                        if(!record.base)m.attributes=record.attributes;
                        if(record.modified)m.modified=record.modified;
                        if(record.sizeKnown){m.size=record.size;m.known=1;}
                    }
                    for(const auto& n:record.names){if(ownName(root,n.parent,n.text))continue;auto raw=bytes(n.text);if(raw.empty())continue;
                        Spool row{reference,n.parent,0,0,0,root,0,(uint16_t)raw.size(),0};names.write((const char*)&row,sizeof(row));names.write(raw.data(),raw.size());++aliasCount;}
                    ++status.readRecords;
                },stopped,[&](uint64_t){publish();});
                names.close();if(!names)throw std::runtime_error("NTFS alias spool write failed");std::ifstream input(namesPath,std::ios::binary);
                for(uint64_t i=0;i<aliasCount;++i){if((i%4096)==0&&stopped())throw std::runtime_error("Native indexing stopped");Spool row{};input.read((char*)&row,sizeof(row));std::string n(row.length,'\0');input.read(n.data(),n.size());if(!input)throw std::runtime_error("NTFS alias spool truncated");
                    auto ordinal=row.file&((uint64_t{1}<<48)-1);if(ordinal>=metadata.size()||metadata[(size_t)ordinal].file!=row.file)continue;const auto& m=metadata[(size_t)ordinal];
                    builder.add({row.file,row.parent,0,m.size,m.modified,root,(m.attributes&FILE_ATTRIBUTE_DIRECTORY)!=0,m.known!=0,std::move(n)});++status.materializedRecords;
                }
                input.close();DeleteFileW(namesPath.c_str());
            }
            status.stage=L"sorts";publish(true);
            auto unresolved=builder.finish(stopped,[&](const std::wstring& step){status.message=L"原生 NTFS 索引 · "+step;publish(true);});
            status.limited=unresolved!=0;
            // A new immutable snapshot includes the initial journal cursor;
            // superseded update logs are rotated after the snapshot is durable.
            if(fs::exists(data/L"catalog.delta"))MoveFileExW((data/L"catalog.delta").c_str(),(data/L"catalog.delta.previous").c_str(),MOVEFILE_REPLACE_EXISTING);
            reader.available();
            DeleteFileW((data/L"catalog.rebuild").c_str());
        }
        if(!fs::exists(lookupName(data,reader.generation()))&&!fs::exists(data/L"catalog.lookup")){status.stage=L"sorts";publish(true);buildCatalogLookup(data,stopped,[&](const auto& step){status.message=step;publish(true);});}
        status.total=(int64_t)reader.total();status.completedRoots=(int)roots.size();status.stage=L"changes";publish(true);
        auto lastCompaction=GetTickCount64(),lastCleanup=lastCompaction;uint64_t failedUpdates=0;const bool baseLimited=status.limited;
        auto compactCancelled=std::make_shared<std::atomic<bool>>(false);
        std::future<std::vector<ntfs::Root>> compaction;
        struct CancelCompaction {std::shared_ptr<std::atomic<bool>> cancelled;~CancelCompaction(){*cancelled=true;}} cancelCompaction{compactCancelled};
        while(!stopped()){
            if(compaction.valid()&&compaction.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
                // The snapshot records the cursors captured at compaction start.
                // Replay subsequent USN records instead of losing changes that
                // arrived while the background snapshot was being constructed.
                auto checkpoint=compaction.get();
                MoveFileExW((data/L"catalog.delta").c_str(),(data/L"catalog.delta.previous").c_str(),MOVEFILE_REPLACE_EXISTING);
                reader.available();for(size_t i=0;i<roots.size();++i)roots[i].nextUsn=checkpoint[i].nextUsn;
                ++revision;lastCompaction=GetTickCount64();
            }
            bool changed=false,caughtUp=true;
            for(uint16_t root=0;root<volumes.size();++root){
                std::vector<ntfs::Change> events;auto next=volumes[root]->changes(roots[root].nextUsn,events);
                std::set<uint64_t> files;
                for(const auto& e:events)if(e.file!=roots[root].rootFile&&!ownName(root,e.parent,e.name))files.insert(e.file);
                bool completeBatch=true,batchChanged=false;
                for(auto file:files){
                    if(stopped())throw std::runtime_error("Native indexing stopped");
                    try{
                        auto current=volumes[root]->file(file);std::vector<CatalogEntry> entries;
                        if(current)for(const auto& n:current->names)if(!ownName(root,n.parent,n.text))entries.push_back({file,n.parent,0,current->size,current->modified,root,current->folder,current->sizeKnown,bytes(n.text)});
                        auto previous=reader.file(root,file);
                        auto equal=[&]{if(previous.size()!=entries.size())return false;for(const auto& e:entries)if(std::none_of(previous.begin(),previous.end(),[&](const auto& p){return p.name==e.name&&p.parent==e.parent&&p.size==e.size&&p.modified==e.modified&&p.folder==e.folder&&p.sizeKnown==e.sizeKnown;}))return false;return true;};
                        if(equal())continue;
                        reader.replace(root,file,entries);entries=reader.file(root,file);
                        appendCatalogChange(data,reader.generation(),root,file,entries,false);changed=batchChanged=true;
                    }catch(const std::exception&){completeBatch=false;++failedUpdates;status.limited=true;break;}
                }
                // Never acknowledge a failed targeted update. Retry the same
                // bounded journal chunk; do not schedule directory recursion.
                if(batchChanged)flushCatalogChanges(data);
                if(completeBatch)roots[root].nextUsn=next;
                caughtUp&=completeBatch&&roots[root].nextUsn>=volumes[root]->info.nextUsn;
            }
            if(changed)++revision;status.total=(int64_t)reader.total();status.building=!caughtUp;status.complete=caughtUp;status.limited=baseLimited||!caughtUp;
            status.stage=caughtUp?L"ready":L"changes";status.message=L"原生 MFT 索引 · USN 实时更新 · "+std::to_wstring(roots.size())+L" 个卷";
            if(compaction.valid())status.message+=L" · 后台整理索引，搜索与监控继续";
            if(failedUpdates)status.message+=L" · 定点更新重试 "+std::to_wstring(failedUpdates);
            if(status.limited)status.message+=L" · 存在未还原或待验证项目";publish(changed);
            std::error_code ec;auto logSize=fs::file_size(data/L"catalog.delta",ec);
            if(!compaction.valid()&&GetTickCount64()-lastCleanup>60000){cleanRetiredGenerations(data,reader.generation());lastCleanup=GetTickCount64();}
            if(!compaction.valid()&&caughtUp&&((!ec&&logSize>32*1024*1024&&GetTickCount64()-lastCompaction>60000)||fs::exists(data/L"catalog.compact"))){
                DeleteFileW((data/L"catalog.compact").c_str());
                compaction=std::async(std::launch::async,[data,checkpoint=roots,stopped,compactCancelled]{
                    auto cancelled=[&]{return compactCancelled->load()||stopped();};
                    NativeCatalog snapshot(data);if(!snapshot.available())throw std::runtime_error("Compaction snapshot unavailable");
                    snapshot.file(0,checkpoint.front().rootFile); // Capture committed overlay before streaming.
                    CatalogBuilder compact(data,checkpoint);snapshot.each([&](const auto& e){if(cancelled())throw std::runtime_error("Native compaction stopped");compact.add(e);});
                    compact.finish(cancelled,[](const std::wstring&){});return checkpoint;
                });
            }
            HANDLE waits[]{stop,parent};WaitForMultipleObjects(parent?2:1,waits,FALSE,caughtUp?200:1);
        }
        status.building=false;status.stage=L"stopped";status.message=L"索引进程已停止，已保存快照与增量检查点";publish(true);
    }catch(const std::exception& e){status.building=false;status.complete=false;status.limited=true;status.stage=L"stopped";status.message=stopped()?L"索引已停止，已有快照保留":L"原生索引暂停："+errorText(e);
        if(!stopped()&&std::string(e.what()).find("checkpoint expired")!=std::string::npos){std::ofstream marker(data/L"catalog.rebuild");marker<<"Journal checkpoint expired";status.building=true;status.stage=L"records";status.message=L"变化日志已断档，正在重建可信元数据快照";}
        else DeleteFileW((data/L"catalog.rebuild").c_str());publish(true);}
    SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN);
    return true;
}
}
