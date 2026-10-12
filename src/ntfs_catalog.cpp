#include "ntfs_catalog.hpp"
#include <winioctl.h>
#include <winternl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

namespace desk::ntfs {
namespace {
constexpr uint64_t ordinalMask=(uint64_t{1}<<48)-1;
template<class T>T get(const unsigned char* p,size_t n,size_t offset){
    if(offset>n||sizeof(T)>n-offset)throw std::runtime_error("NTFS attribute bounds");
    T v{};memcpy(&v,p+offset,sizeof(v));return v;
}
void winError(const char* what){throw std::system_error(GetLastError(),std::system_category(),what);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
std::vector<uint64_t> listReferences(const std::vector<unsigned char>& bytes){
    std::set<uint64_t> refs;
    for(size_t at=0;at<bytes.size();){
        auto type=get<uint32_t>(bytes.data(),bytes.size(),at);
        if(type==0||type==0xffffffff)break;
        auto length=get<uint16_t>(bytes.data(),bytes.size(),at+4);
        if(length<26||length>bytes.size()-at)throw std::runtime_error("NTFS attribute list bounds: offset="+std::to_string(at)+", type="+std::to_string(type)+", entry="+std::to_string(length)+", bytes="+std::to_string(bytes.size()));
        refs.insert(get<uint64_t>(bytes.data(),bytes.size(),at+16));at+=length;
    }
    return {refs.begin(),refs.end()};
}
}
std::vector<Run> decodeRuns(const void* raw,size_t length,uint64_t vcn){
    const auto* bytes=static_cast<const unsigned char*>(raw);
    std::vector<Run> result;int64_t lcn=0;
    for(size_t at=0;at<length;){
        unsigned tag=bytes[at++];if(!tag)return result;
        unsigned countBytes=tag&15,offsetBytes=tag>>4;
        if(!countBytes||countBytes>8||offsetBytes>8||countBytes+offsetBytes>length-at)
            throw std::runtime_error("NTFS mapping pairs bounds");
        uint64_t count=0;for(unsigned i=0;i<countBytes;++i)count|=uint64_t(bytes[at++])<<(8*i);
        if(!count||count>std::numeric_limits<uint64_t>::max()-vcn)throw std::runtime_error("NTFS run length");
        int64_t absolute=-1;
        if(offsetBytes){
            uint64_t delta=0;for(unsigned i=0;i<offsetBytes;++i)delta|=uint64_t(bytes[at+i])<<(8*i);
            if(offsetBytes<8&&(bytes[at+offsetBytes-1]&0x80))delta|=(~uint64_t{0})<<(8*offsetBytes);
            int64_t signedDelta=static_cast<int64_t>(delta);at+=offsetBytes;
            if((signedDelta>0&&lcn>INT64_MAX-signedDelta)||(signedDelta<0&&lcn<INT64_MIN-signedDelta))throw std::runtime_error("NTFS run offset");
            lcn+=signedDelta;if(lcn<0)throw std::runtime_error("NTFS negative physical run");absolute=lcn;
        }
        result.push_back({vcn,count,absolute});vcn+=count;
    }
    throw std::runtime_error("NTFS mapping pairs terminator");
}
Decode decodeRecord(const void* raw,size_t length,uint64_t ordinal,Record& out,bool sectorFixup){
    out={};if(length<48)return Decode::Invalid;
    const auto* original=static_cast<const unsigned char*>(raw);
    if(memcmp(original,"FILE",4))return memcmp(original,"BAAD",4)==0?Decode::Invalid:Decode::Unused;
    try{
        std::vector<unsigned char> bytes(original,original+length);auto* p=bytes.data();
        auto usa=get<uint16_t>(p,length,4),count=get<uint16_t>(p,length,6);
        if(count<2||usa>length||size_t(count)*2>length-usa||length%(count-1))return Decode::Invalid;
        const size_t sector=length/(count-1);
        if(sector<512||sector>4096||(sector&(sector-1)))return Decode::Invalid;
        auto signature=get<uint16_t>(p,length,usa);
        for(unsigned i=1;sectorFixup&&i<count;++i){
            auto end=sector*i-2;auto before=get<uint16_t>(p,length,end),replacement=get<uint16_t>(p,length,usa+2*i);
            if(before!=signature&&before!=replacement)return Decode::Invalid;
            memcpy(p+end,&replacement,2);
        }
        auto flags=get<uint16_t>(p,length,22);if(!(flags&1))return Decode::Unused;
        auto sequence=get<uint16_t>(p,length,16);if(!sequence)return Decode::Invalid;
        out.file=(uint64_t(sequence)<<48)|(ordinal&ordinalMask);
        out.links=get<uint16_t>(p,length,18);
        out.base=get<uint64_t>(p,length,32);out.folder=(flags&2)!=0;
        out.attributes=out.folder?FILE_ATTRIBUTE_DIRECTORY:0;
        auto used=get<uint32_t>(p,length,24);auto at=get<uint16_t>(p,length,20);
        if(used>length||used<48||at<48||at>used)return Decode::Invalid;
        for(size_t offset=at;offset<used;){
            auto type=get<uint32_t>(p,used,offset);if(type==0xffffffff)return Decode::Valid;
            auto n=get<uint32_t>(p,used,offset+4);
            if(n<24||n>used-offset||n%8)return Decode::Invalid;
            const auto* a=p+offset;bool resident=get<uint8_t>(a,n,8)==0;
            const auto named=get<uint8_t>(a,n,9);
            if(resident){
                auto valueSize=get<uint32_t>(a,n,16);auto valueOffset=get<uint16_t>(a,n,20);
                if(valueOffset>n||valueSize>n-valueOffset)return Decode::Invalid;
                const auto* value=a+valueOffset;
                if(type==0x10&&valueSize>=36){out.modified=get<uint64_t>(value,valueSize,8);out.attributes=get<DWORD>(value,valueSize,32);if(out.folder)out.attributes|=FILE_ATTRIBUTE_DIRECTORY;}
                else if(type==0x20)out.attributeList.assign(value,value+valueSize);
                else if(type==0x30){
                    if(valueSize<66)return Decode::Invalid;
                    unsigned chars=value[64],space=value[65];if(chars*2>valueSize-66)return Decode::Invalid;
                    if(space!=2&&chars){
                        Name name;name.parent=get<uint64_t>(value,valueSize,0);name.text.resize(chars);
                        memcpy(name.text.data(),value+66,chars*2);
                        if(name.text.find(L'\0')!=std::wstring::npos)return Decode::Invalid;
                        out.names.push_back(std::move(name));
                    }
                }else if(type==0x80&&!named){out.size=valueSize;out.sizeKnown=true;}
            }else if((type==0x80&&!named)||type==0x20){
                if(n<64)return Decode::Invalid;
                auto start=get<uint64_t>(a,n,16);auto mapping=get<uint16_t>(a,n,32);
                if(mapping<64||mapping>=n)return Decode::Invalid;
                std::vector<Run> runs;
                if(type==0x20||ordinal==0||(out.base&&(out.base&ordinalMask)==0))runs=decodeRuns(a+mapping,n-mapping,start);
                if(type==0x80){out.dataRuns.insert(out.dataRuns.end(),runs.begin(),runs.end());if(!start){out.size=get<uint64_t>(a,n,48);out.sizeKnown=true;}}
                else{out.listRuns.insert(out.listRuns.end(),runs.begin(),runs.end());if(!start)out.listSize=get<uint64_t>(a,n,48);}
            }
            offset+=n;
        }
        return Decode::Invalid;
    }catch(...){return Decode::Invalid;}
}
struct Volume::Impl {
    Handle volume;
    struct Disk {DISK_EXTENT extent{};std::unique_ptr<Handle> handle;};
    std::vector<Disk> disks;
    bool volumeRead=true;
    NTFS_VOLUME_DATA_BUFFER layout{};
    std::vector<Run> mft;
    std::optional<Record> record(uint64_t file,bool nearest=false){
        NTFS_FILE_RECORD_INPUT_BUFFER input{};input.FileReferenceNumber.QuadPart=static_cast<LONGLONG>(file&ordinalMask);
        std::vector<unsigned char> bytes(offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER,FileRecordBuffer)+layout.BytesPerFileRecordSegment);
        DWORD returned=0;
        if(!DeviceIoControl(volume.value,FSCTL_GET_NTFS_FILE_RECORD,&input,sizeof(input),bytes.data(),static_cast<DWORD>(bytes.size()),&returned,nullptr))winError("Read NTFS file record");
        auto* output=reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(bytes.data());
        if(returned<offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER,FileRecordBuffer)||output->FileRecordLength>returned-offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER,FileRecordBuffer))throw std::runtime_error("NTFS returned record bounds");
        const auto actual=uint64_t(output->FileReferenceNumber.QuadPart)&ordinalMask;
        if(actual!=(file&ordinalMask)&&!nearest)return {};
        // FSCTL returns the coherent cache record, with sector fixups already
        // applied. Its USA replacements can predate a still-open file's edits.
        Record result;auto decoded=decodeRecord(output->FileRecordBuffer,output->FileRecordLength,actual,result,false);
        if(decoded==Decode::Unused)return {};
        if(decoded!=Decode::Valid)throw std::runtime_error("NTFS file record validation failed");
        if((file>>48)&&result.file!=file&&!nearest)return {};
        return result;
    }
    void physical(uint64_t offset,void* data,DWORD length){
        LARGE_INTEGER at{};at.QuadPart=static_cast<LONGLONG>(offset);
        if(!SetFilePointerEx(volume.value,at,nullptr,FILE_BEGIN))winError("Seek NTFS volume");
        // Raw volume I/O may require sector-aligned memory even when the
        // caller did not explicitly request unbuffered I/O.
        void* aligned=VirtualAlloc(nullptr,length,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!aligned)winError("Allocate NTFS read buffer");
        DWORD read=0;bool ok=volumeRead&&ReadFile(volume.value,aligned,length,&read,nullptr)&&read==length;DWORD error=GetLastError();
        if(!ok&&(error==ERROR_NOT_SUPPORTED||!volumeRead)){
            volumeRead=false;
            if(disks.empty()){
                std::vector<unsigned char> extents(65536);DWORD returned=0;
                if(!DeviceIoControl(volume.value,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,extents.data(),(DWORD)extents.size(),&returned,nullptr)){VirtualFree(aligned,0,MEM_RELEASE);winError("Read NTFS disk extents");}
                auto* description=(VOLUME_DISK_EXTENTS*)extents.data();
                if(returned<offsetof(VOLUME_DISK_EXTENTS,Extents)||!description->NumberOfDiskExtents||description->NumberOfDiskExtents>(returned-offsetof(VOLUME_DISK_EXTENTS,Extents))/sizeof(DISK_EXTENT)){VirtualFree(aligned,0,MEM_RELEASE);throw std::runtime_error("NTFS volume extent bounds");}
                for(DWORD i=0;i<description->NumberOfDiskExtents;++i){Disk d;d.extent=description->Extents[i];d.handle=std::make_unique<Handle>();
                    d.handle->value=CreateFileW((L"\\\\.\\PhysicalDrive"+std::to_wstring(d.extent.DiskNumber)).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING,nullptr);
                    if(d.handle->value==INVALID_HANDLE_VALUE){VirtualFree(aligned,0,MEM_RELEASE);winError("Open NTFS physical disk for metadata reads");}disks.push_back(std::move(d));}
            }
            uint64_t base=0,remaining=length,logical=offset;auto* target=(unsigned char*)aligned;ok=true;
            for(auto& disk:disks){uint64_t span=(uint64_t)disk.extent.ExtentLength.QuadPart;if(logical>=base+span){base+=span;continue;}
                uint64_t within=logical-base;DWORD part=(DWORD)std::min<uint64_t>(remaining,span-within);
                LARGE_INTEGER position{};position.QuadPart=disk.extent.StartingOffset.QuadPart+(LONGLONG)within;
                if(!SetFilePointerEx(disk.handle->value,position,nullptr,FILE_BEGIN)||!ReadFile(disk.handle->value,target,part,&read,nullptr)||read!=part){error=GetLastError();ok=false;break;}
                remaining-=part;logical+=part;target+=part;base+=span;if(!remaining)break;
            }
            if(remaining)ok=false;
        }
        if(ok)memcpy(data,aligned,length);VirtualFree(aligned,0,MEM_RELEASE);
        if(!ok){SetLastError(error);winError("Read NTFS metadata extent");}
    }
    void stream(const std::vector<Run>& runs,uint64_t offset,void* destination,size_t length){
        auto* out=static_cast<unsigned char*>(destination);const uint64_t cluster=layout.BytesPerCluster;
        while(length){
            auto vcn=offset/cluster;auto found=std::find_if(runs.begin(),runs.end(),[&](const auto& r){return vcn>=r.vcn&&vcn-r.vcn<r.length;});
            if(found==runs.end())throw std::runtime_error("NTFS stream extent missing");
            const auto within=offset-found->vcn*cluster;
            const size_t part=static_cast<size_t>(std::min<uint64_t>(length,found->length*cluster-within));
            if(found->lcn<0)memset(out,0,part);
            else physical(uint64_t(found->lcn)*cluster+within,out,static_cast<DWORD>(part));
            out+=part;offset+=part;length-=part;
        }
    }
    void loadList(Record& record){
        if(!record.attributeList.empty()||!record.listSize)return;
        if(record.listSize>2*1024*1024)throw std::runtime_error("NTFS attribute list exceeds bound");
        const auto sector=layout.BytesPerSector;
        auto aligned=(record.listSize+sector-1)/sector*sector;
        std::vector<unsigned char> bytes(static_cast<size_t>(aligned));stream(record.listRuns,0,bytes.data(),bytes.size());
        bytes.resize(static_cast<size_t>(record.listSize));record.attributeList=std::move(bytes);
    }
    std::vector<Name> hardlinks(uint64_t file){
        FILE_ID_DESCRIPTOR id{};id.dwSize=sizeof(id);id.Type=FileIdType;id.FileId.QuadPart=(LONGLONG)file;
        Handle h;h.value=OpenFileById(volume.value,&id,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,FILE_FLAG_BACKUP_SEMANTICS);
        if(h.value==INVALID_HANDLE_VALUE)winError("Open NTFS file ID for hardlink metadata");
        using Query=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
        auto query=(Query)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationFile");if(!query)throw std::runtime_error("Native file information API unavailable");
        std::vector<unsigned char> bytes(4096);IO_STATUS_BLOCK io{};NTSTATUS status=0;
        for(;;){status=query(h.value,&io,bytes.data(),(ULONG)bytes.size(),(FILE_INFORMATION_CLASS)46);if(status>=0)break;
            if((uint32_t)status!=0x80000005u&&(uint32_t)status!=0xc0000004u)throw std::runtime_error("NTFS hardlink information status "+std::to_string((uint32_t)status));
            if(bytes.size()>=2*1024*1024)throw std::runtime_error("NTFS hardlink information exceeds bound");bytes.resize(bytes.size()*2);}
        size_t n=std::min<size_t>(bytes.size(),io.Information);if(n<8)throw std::runtime_error("NTFS hardlink information header bounds");
        auto count=get<uint32_t>(bytes.data(),n,4);if(count>32768)throw std::runtime_error("NTFS hardlink count exceeds bound");
        std::vector<Name> result;size_t at=8;
        for(uint32_t i=0;i<count;++i){auto next=get<uint32_t>(bytes.data(),n,at);auto parent=get<uint64_t>(bytes.data(),n,at+8);auto length=get<uint32_t>(bytes.data(),n,at+16);
            if(length>255||at+20>n||size_t(length)*2>n-at-20)throw std::runtime_error("NTFS hardlink name bounds");
            Name name;name.parent=parent;name.text.resize(length);memcpy(name.text.data(),bytes.data()+at+20,size_t(length)*2);
            if(!(parent>>48)){auto directory=record(parent);if(!directory)throw std::runtime_error("NTFS hardlink parent reference disappeared");name.parent=directory->file;}
            result.push_back(std::move(name));if(i+1<count){if(next<20||next>n-at)throw std::runtime_error("NTFS hardlink next-entry bounds");at+=next;}
        }
        return result;
    }
    Record complete(Record base,bool needRuns=false){
        if(!needRuns&&base.names.size()>=base.links)return base;
        if(!needRuns&&base.attributeList.empty()&&base.listSize){base.names=hardlinks(base.file);return base;}
        loadList(base);auto refs=listReferences(base.attributeList);
        for(auto ref:refs)if(ref!=base.file){
            auto part=record(ref);if(!part||part->base!=base.file)throw std::runtime_error("NTFS extension reference changed");
            base.names.insert(base.names.end(),part->names.begin(),part->names.end());
            base.dataRuns.insert(base.dataRuns.end(),part->dataRuns.begin(),part->dataRuns.end());
            if(part->sizeKnown&&!base.sizeKnown){base.size=part->size;base.sizeKnown=true;}
        }
        return base;
    }
    void metadata(Record& r){
        if((r.sizeKnown||r.folder)&&r.modified)return;
        FILE_ID_DESCRIPTOR id{};id.dwSize=sizeof(id);id.Type=FileIdType;id.FileId.QuadPart=(LONGLONG)r.file;
        Handle h;h.value=OpenFileById(volume.value,&id,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,FILE_FLAG_BACKUP_SEMANTICS);
        if(h.value==INVALID_HANDLE_VALUE)winError("Open NTFS file ID for metadata");
        FILE_STANDARD_INFO size{};FILE_BASIC_INFO dates{};
        if(GetFileInformationByHandleEx(h.value,FileStandardInfo,&size,sizeof(size))){r.folder=size.Directory!=FALSE;r.size=(uint64_t)size.EndOfFile.QuadPart;r.sizeKnown=!r.folder;}
        if(GetFileInformationByHandleEx(h.value,FileBasicInfo,&dates,sizeof(dates))){r.modified=(uint64_t)dates.LastWriteTime.QuadPart;r.attributes=dates.FileAttributes;}
    }
};
Volume::Volume(const std::wstring& path):impl_(std::make_unique<Impl>()){
    if(path.size()!=3||path[1]!=L':'||path[2]!=L'\\')throw std::runtime_error("Native catalog requires an NTFS volume root");
    info.path=path;
    impl_->volume.value=CreateFileW((L"\\\\.\\"+path.substr(0,2)).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING,nullptr);
    if(impl_->volume.value==INVALID_HANDLE_VALUE)winError("Open NTFS volume");
    DWORD returned=0;
    DeviceIoControl(impl_->volume.value,FSCTL_ALLOW_EXTENDED_DASD_IO,nullptr,0,nullptr,0,&returned,nullptr);
    if(!DeviceIoControl(impl_->volume.value,FSCTL_GET_NTFS_VOLUME_DATA,nullptr,0,&impl_->layout,sizeof(impl_->layout),&returned,nullptr))winError("Read NTFS layout");
    const auto& layout=impl_->layout;
    if(layout.BytesPerFileRecordSegment<512||layout.BytesPerFileRecordSegment>65536||!layout.BytesPerSector||!layout.BytesPerCluster||layout.MftValidDataLength.QuadPart<=0)throw std::runtime_error("Unsupported NTFS layout");
    USN_JOURNAL_DATA_V0 journal{};
    if(!DeviceIoControl(impl_->volume.value,FSCTL_QUERY_USN_JOURNAL,nullptr,0,&journal,sizeof(journal),&returned,nullptr))winError("Read NTFS journal");
    info.serial=static_cast<DWORD>(layout.VolumeSerialNumber.QuadPart);info.journal=journal.UsnJournalID;info.nextUsn=journal.NextUsn;
    info.records=static_cast<uint64_t>(layout.MftValidDataLength.QuadPart)/layout.BytesPerFileRecordSegment;
    auto root=impl_->record(5);if(!root)throw std::runtime_error("NTFS root record unavailable");info.rootFile=root->file;
    auto mft=impl_->record(0);if(!mft)throw std::runtime_error("NTFS MFT record unavailable");
    try{mft=impl_->complete(std::move(*mft),true);impl_->mft=std::move(mft->dataRuns);
        std::sort(impl_->mft.begin(),impl_->mft.end(),[](const auto& a,const auto& b){return a.vcn<b.vcn;});
        if(impl_->mft.empty()||impl_->mft.front().vcn!=0)throw std::runtime_error("NTFS MFT extents unavailable");
        std::vector<unsigned char> test(layout.BytesPerFileRecordSegment);impl_->stream(impl_->mft,0,test.data(),test.size());Record decoded;
        if(decodeRecord(test.data(),test.size(),0,decoded)!=Decode::Valid)throw std::runtime_error("NTFS raw record not readable; use decrypted file-record API");info.raw=true;
    }catch(...){impl_->mft.clear();info.raw=false;}
}
Volume::~Volume()=default;
std::optional<Record> Volume::file(uint64_t ref){auto record=impl_->record(ref);if(record){record=impl_->complete(std::move(*record));impl_->metadata(*record);}return record;}
void Volume::enumerate(const std::function<void(const Record&)>& visit,const std::function<bool()>& stopped,const std::function<void(uint64_t)>& progress){
    if(!info.raw){
        uint64_t before=info.records;
        while(before){if(stopped())throw std::runtime_error("Native indexing stopped");auto r=impl_->record(before-1,true);
            if(!r){--before;continue;}auto ordinal=r->file&ordinalMask;if(ordinal>=before)throw std::runtime_error("NTFS file-record cursor did not advance");
            visit(*r);before=ordinal;progress(info.records-before);
        }
        return;
    }
    const size_t stride=impl_->layout.BytesPerFileRecordSegment;
    const size_t batch=std::max<size_t>(stride,1024*1024/stride*stride);
    std::vector<unsigned char> bytes(batch);
    for(uint64_t first=0;first<info.records;){
        if(stopped())throw std::runtime_error("Native indexing stopped");
        size_t records=static_cast<size_t>(std::min<uint64_t>(batch/stride,info.records-first));
        impl_->stream(impl_->mft,first*stride,bytes.data(),records*stride);
        for(size_t i=0;i<records;++i){
            Record record;auto result=decodeRecord(bytes.data()+i*stride,stride,first+i,record);
            if(result==Decode::Invalid){auto current=impl_->record(first+i);if(current)visit(*current);}
            else if(result==Decode::Valid)visit(record);
        }
        first+=records;progress(first);
    }
}
uint64_t Volume::changes(uint64_t cursor,std::vector<Change>& out){
    DWORD returned=0;USN_JOURNAL_DATA_V0 journal{};out.clear();
    if(!DeviceIoControl(impl_->volume.value,FSCTL_QUERY_USN_JOURNAL,nullptr,0,&journal,sizeof(journal),&returned,nullptr))winError("Query NTFS changes");
    if(journal.UsnJournalID!=info.journal||cursor<uint64_t(journal.FirstUsn)||cursor>uint64_t(journal.NextUsn))throw std::runtime_error("NTFS journal checkpoint expired; rebuild required");
    info.nextUsn=static_cast<uint64_t>(journal.NextUsn);
    READ_USN_JOURNAL_DATA_V0 input{};input.StartUsn=static_cast<USN>(cursor);input.ReasonMask=0xffffffff;input.UsnJournalID=info.journal;
    std::vector<unsigned char> bytes(64*1024);
    if(!DeviceIoControl(impl_->volume.value,FSCTL_READ_USN_JOURNAL,&input,sizeof(input),bytes.data(),static_cast<DWORD>(bytes.size()),&returned,nullptr))winError("Read NTFS changes");
    if(returned<8)throw std::runtime_error("NTFS journal header bounds");
    auto next=get<uint64_t>(bytes.data(),returned,0);
    for(size_t at=8;at<returned;){
        constexpr size_t fixed=offsetof(USN_RECORD_V2,FileName);
        if(returned-at<fixed)throw std::runtime_error("NTFS change bounds");
        USN_RECORD_V2 r{};memcpy(&r,bytes.data()+at,fixed);
        if(r.MajorVersion!=2||r.RecordLength<fixed||r.RecordLength>returned-at||r.FileNameOffset<fixed||r.FileNameLength%2||r.FileNameOffset>r.RecordLength||r.FileNameLength>r.RecordLength-r.FileNameOffset)throw std::runtime_error("Unsupported NTFS journal record");
        Change change{r.FileReferenceNumber,r.ParentFileReferenceNumber,r.Reason,r.FileAttributes,{}};change.name.resize(r.FileNameLength/2);memcpy(change.name.data(),bytes.data()+at+r.FileNameOffset,r.FileNameLength);out.push_back(std::move(change));at+=r.RecordLength;
    }
    return next;
}
}
