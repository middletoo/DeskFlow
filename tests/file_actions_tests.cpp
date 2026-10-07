#include "file_actions.hpp"
#include <shellapi.h>
#include <ole2.h>
#include <fstream>
#include <iostream>
static void require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
int main() {
    OleInitialize(nullptr);
    auto folder=std::filesystem::temp_directory_path()/(L"DeskFlow-file-actions-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    try {
        auto first=folder/L"你好 world.txt",second=folder/L"other file.txt";
        {std::ofstream file(first,std::ios::binary);file<<"\xEF\xBB\xBFHello preview \xE4\xBD\xA0\xE5\xA5\xBD";}
        {std::ofstream file(second);file<<"second";}
        auto preview=desk::loadFilePreview(first);
        require(preview.text.find(L"Hello preview 你好")!=std::wstring::npos,"UTF-8 preview must show real text");
        require(!preview.bitmap,"text preview must not allocate image");
        auto longText=folder/L"utf8-prefix.txt";
        {std::ofstream file(longText,std::ios::binary);for(int i=0;i<23000;i++)file<<"\xE4\xBD\xA0";}
        auto bounded=desk::loadFilePreview(longText);
        require(bounded.text.find(std::wstring(100,L'你'))!=std::wstring::npos,
                "UTF-8 preview truncation must not reinterpret the whole prefix as ANSI");
        std::atomic_int version{2};
        auto cancelled=desk::loadFilePreview(first,&version,1);
        require(cancelled.text.empty()&&!cancelled.bitmap,"cancelled preview must not publish content");
        require(SUCCEEDED(desk::renameFile(nullptr,first.wstring(),L"renamed.txt")),"Shell rename synthetic file");
        require(std::filesystem::exists(folder/L"renamed.txt")&&!std::filesystem::exists(first),"rename persisted on disk");
        IDataObject* object=nullptr;
        require(SUCCEEDED(desk::makeFileDataObject({(folder/L"renamed.txt").wstring(),second.wstring()},&object)),"two selected files produce Shell data object");
        FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM medium{};
        auto copied=object->GetData(&format,&medium);object->Release();
        require(SUCCEEDED(copied),"file data object supports CF_HDROP");
        require(DragQueryFileW((HDROP)medium.hGlobal,0xffffffff,nullptr,0)==2,"both selected files are available to Explorer/drag targets");
        ReleaseStgMedium(&medium);
        require(FAILED(desk::renameFile(nullptr,(folder/L"renamed.txt").wstring(),L"../outside.txt")),"rename must reject path traversal");
        require(desk::fileSizeLabel(1024).find(L"KiB")!=std::wstring::npos,"size uses stable binary unit");
        std::filesystem::remove_all(folder);
        std::cout<<"PASS bounded Unicode file preview, cancellation and native Shell rename\n";
        OleUninitialize();return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<"\n";OleUninitialize();return 1;}
}
