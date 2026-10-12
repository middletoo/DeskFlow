#include "ntfs_catalog.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
template<class T>static void put(std::vector<unsigned char>& b,size_t at,T value){memcpy(b.data()+at,&value,sizeof(value));}
static void resident(std::vector<unsigned char>& b,size_t& at,uint32_t type,const std::vector<unsigned char>& value){
    auto length=(24+value.size()+7)&~size_t{7};put<uint32_t>(b,at,type);put<uint32_t>(b,at+4,(uint32_t)length);put<uint32_t>(b,at+16,(uint32_t)value.size());put<uint16_t>(b,at+20,24);memcpy(b.data()+at+24,value.data(),value.size());at+=length;
}
int main(){
    try{
        std::vector<unsigned char> b(1024);memcpy(b.data(),"FILE",4);put<uint16_t>(b,4,48);put<uint16_t>(b,6,3);put<uint16_t>(b,16,9);put<uint16_t>(b,20,56);put<uint16_t>(b,22,1);size_t at=56;
        std::vector<unsigned char> info(48);put<uint64_t>(info,8,133000000000000000ULL);put<uint32_t>(info,32,FILE_ATTRIBUTE_NORMAL);resident(b,at,0x10,info);
        for(unsigned i=0;i<3;++i){std::wstring name=i==0?L"primary.txt":i==1?L"别名.txt":L"ALIAS~1.TXT";std::vector<unsigned char> value(66+name.size()*2);put<uint64_t>(value,0,5+i);value[64]=(unsigned char)name.size();value[65]=i==2?2:1;memcpy(value.data()+66,name.data(),name.size()*2);resident(b,at,0x30,value);}
        resident(b,at,0x80,std::vector<unsigned char>(15,0));put<uint32_t>(b,at,0xffffffff);at+=4;put<uint32_t>(b,24,(uint32_t)at);put<uint16_t>(b,48,0xaaaa);
        for(unsigned i=1;i<3;++i){put<uint16_t>(b,48+2*i,0);put<uint16_t>(b,i*512-2,0xaaaa);}
        desk::ntfs::Record r;require(desk::ntfs::decodeRecord(b.data(),b.size(),500,r)==desk::ntfs::Decode::Valid,"valid MFT record rejected");
        require(r.file==((uint64_t{9}<<48)|500)&&r.names.size()==2&&r.names[1].text==L"别名.txt"&&r.names[1].parent==6&&r.sizeKnown&&r.size==15&&r.modified==133000000000000000ULL,"MFT names/metadata/sequence mismatch");
        auto bad=b;put<uint16_t>(bad,510,0xbbbb);require(desk::ntfs::decodeRecord(bad.data(),bad.size(),500,r)==desk::ntfs::Decode::Invalid,"torn sector accepted");
        require(desk::ntfs::decodeRecord(bad.data(),bad.size(),500,r,false)==desk::ntfs::Decode::Valid,"coherent cache record rejected for stale USA replacements");
        bad=b;put<uint32_t>(bad,60,UINT32_MAX);require(desk::ntfs::decodeRecord(bad.data(),bad.size(),500,r)==desk::ntfs::Decode::Invalid,"attribute overflow accepted");
        bad=b;put<uint64_t>(bad,32,(uint64_t{9}<<48)|500);require(desk::ntfs::decodeRecord(bad.data(),bad.size(),600,r)==desk::ntfs::Decode::Valid&&r.base==((uint64_t{9}<<48)|500),"extension base reference lost");
        const unsigned char runs[]{0x11,3,9,0x11,4,0xfe,0x01,2,0};auto decoded=desk::ntfs::decodeRuns(runs,sizeof(runs),0);
        require(decoded.size()==3&&decoded[0].lcn==9&&decoded[1].lcn==7&&decoded[1].vcn==3&&decoded[2].lcn==-1&&decoded[2].length==2,"fragmented/sparse mapping pair decode failed");
        bool invalid=false;try{desk::ntfs::decodeRuns(runs,2,0);}catch(...){invalid=true;}require(invalid,"truncated mapping pair accepted");
        std::cout<<"PASS MFT sector fixup, bounds, sequence/base references, all long filenames, metadata and fragmented/sparse runs\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
