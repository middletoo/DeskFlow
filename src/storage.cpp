#include "storage.hpp"
#include "common.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <vector>
#include <sqlite3.h>

namespace desk {
namespace {
namespace fs=std::filesystem;
constexpr auto registryPath=L"Software\\DeskFlow";
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle):value(handle){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
};
bool samePath(const fs::path& left,const fs::path& right){
    return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE)==CSTR_EQUAL;
}
bool beneath(const fs::path& candidate,const fs::path& directory){
    auto child=candidate.begin(),parent=directory.begin();
    for(;parent!=directory.end();++parent,++child)
        if(child==candidate.end()||!samePath(*child,*parent))return false;
    return true;
}
fs::path validated(const fs::path& path){
    if(!path.is_absolute()||path.has_root_name()&&path.root_name().wstring().starts_with(L"\\\\"))
        throw std::runtime_error("数据目录必须是本地磁盘上的完整路径");
    const auto full=fs::weakly_canonical(path);
    if(full==full.root_path())throw std::runtime_error("请选择专用数据文件夹，不能使用整个磁盘根目录");
    return full;
}
void backupDatabase(const fs::path& source,const fs::path& target){
    const auto attributes=GetFileAttributesW(source.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_ENCRYPTED)){
        Handle encrypted(CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_ENCRYPTED,nullptr));
        if(encrypted.value==INVALID_HANDLE_VALUE)throw std::runtime_error("目标目录无法保留原文件加密，请选择支持 EFS 的本地目录");
    }
    sqlite3 *input=nullptr,*output=nullptr;
    auto close=[](sqlite3* db){if(db)sqlite3_close(db);};
    std::unique_ptr<sqlite3,decltype(close)> reader(nullptr,close),writer(nullptr,close);
    auto src=utf8(source.wstring()),dst=utf8(target.wstring());
    const int opened=sqlite3_open_v2(src.c_str(),&input,SQLITE_OPEN_READONLY,nullptr);reader.reset(input);
    if(opened!=SQLITE_OK)throw std::runtime_error("无法读取原数据库，原文件已保留");
    const int created=sqlite3_open_v2(dst.c_str(),&output,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr);writer.reset(output);
    if(created!=SQLITE_OK)throw std::runtime_error("无法建立目标数据库");
    sqlite3_backup* backup=sqlite3_backup_init(output,"main",input,"main");
    if(!backup)throw std::runtime_error("无法建立数据库迁移快照");
    int result=SQLITE_OK;auto lastProgress=GetTickCount64();
    do {
        result=sqlite3_backup_step(backup,128);
        if(result==SQLITE_OK)lastProgress=GetTickCount64();
        if(result==SQLITE_BUSY||result==SQLITE_LOCKED)Sleep(10);
    }while((result==SQLITE_OK||result==SQLITE_BUSY||result==SQLITE_LOCKED)&&GetTickCount64()-lastProgress<120000);
    const int finished=sqlite3_backup_finish(backup);
    if(result!=SQLITE_DONE||finished!=SQLITE_OK)throw std::runtime_error("数据库仍被占用或迁移失败，原数据未改动");
    sqlite3_stmt* check=nullptr;
    const bool prepared=sqlite3_prepare_v2(output,"PRAGMA quick_check",-1,&check,nullptr)==SQLITE_OK;
    bool intact=prepared&&sqlite3_step(check)==SQLITE_ROW&&
        std::string(reinterpret_cast<const char*>(sqlite3_column_text(check,0)))=="ok";
    if(check)sqlite3_finalize(check);
    if(!intact)throw std::runtime_error("目标数据库完整性检查失败，原数据已保留");
}
void copyStorageFile(const fs::path& source,const fs::path& target){
    if(CopyFileW(source.c_str(),target.c_str(),TRUE))return;
    const auto attributes=GetFileAttributesW(source.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_ENCRYPTED))
        throw std::runtime_error("数据文件复制失败，原数据已保留");
    DeleteFileW(target.c_str());
    Handle output(CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_ENCRYPTED|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(output.value==INVALID_HANDLE_VALUE)throw std::runtime_error("目标目录无法保留原文件加密，请选择支持 EFS 的本地目录");
    Handle input(CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(input.value==INVALID_HANDLE_VALUE)throw std::runtime_error("无法读取原加密文件，原数据已保留");
    std::vector<unsigned char> buffer(1024*1024);DWORD read=0;
    for(;;){
        if(!ReadFile(input.value,buffer.data(),(DWORD)buffer.size(),&read,nullptr))throw std::runtime_error("加密文件读取失败");
        if(!read)break;DWORD written=0;
        if(!WriteFile(output.value,buffer.data(),read,&written,nullptr)||written!=read)throw std::runtime_error("加密文件复制失败");
    }
    if(!FlushFileBuffers(output.value))throw std::runtime_error("加密文件提交失败");
}
}
fs::path physicalDirectory(const fs::path& directory){
    Handle handle(CreateFileW(directory.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(handle.value==INVALID_HANDLE_VALUE)return directory;
    std::array<wchar_t,32768> text{};
    auto count=GetFinalPathNameByHandleW(handle.value,text.data(),(DWORD)text.size(),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!count||count>=text.size())return directory;
    std::wstring result(text.data(),count);
    if(result.starts_with(L"\\\\?\\UNC\\"))result=L"\\\\"+result.substr(8);
    else if(result.starts_with(L"\\\\?\\"))result.erase(0,4);
    return fs::path(result);
}
std::optional<fs::path> configuredDataDirectory(){
    DWORD bytes=0;
    auto status=RegGetValueW(HKEY_CURRENT_USER,registryPath,L"DataDirectory",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,nullptr,&bytes);
    if(status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND)return std::nullopt;
    if(status!=ERROR_SUCCESS||bytes<sizeof(wchar_t)||bytes>32768*sizeof(wchar_t))
        throw std::runtime_error("无法读取数据库位置设置");
    std::wstring value(bytes/sizeof(wchar_t),L'\0');
    status=RegGetValueW(HKEY_CURRENT_USER,registryPath,L"DataDirectory",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,value.data(),&bytes);
    if(status!=ERROR_SUCCESS)throw std::runtime_error("无法读取数据库位置设置");
    value.resize(wcsnlen(value.c_str(),value.size()));
    if(value.empty())return std::nullopt;
    return validated(value);
}
void configureDataDirectory(const fs::path& directory){
    const auto path=validated(directory);
    if(!fs::is_directory(path))throw std::runtime_error("数据库目标目录不可用");
    const auto probe=path/(L".DeskFlow-write-probe-"+std::to_wstring(GetCurrentProcessId()));
    HANDLE file=CreateFileW(probe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("数据库目标目录不可写");
    CloseHandle(file);
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,registryPath,0,nullptr,0,KEY_SET_VALUE|KEY_WOW64_64KEY,nullptr,&key,nullptr)!=ERROR_SUCCESS)
        throw std::runtime_error("无法保存数据库位置设置");
    const auto physical=physicalDirectory(path).wstring();
    const auto result=RegSetValueExW(key,L"DataDirectory",0,REG_SZ,reinterpret_cast<const BYTE*>(physical.c_str()),(DWORD)((physical.size()+1)*sizeof(wchar_t)));
    RegCloseKey(key);
    if(result!=ERROR_SUCCESS)throw std::runtime_error("数据库位置设置保存失败");
}
StorageMigration migrateDataDirectory(const fs::path& source,const fs::path& target,const std::function<void(uint64_t,uint64_t)>& progress,bool commitSelector){
    const auto src=physicalDirectory(validated(source)),dst=validated(target);
    if(samePath(src,physicalDirectory(dst))){
        if(commitSelector){fs::create_directories(dst);configureDataDirectory(dst);}
        return {};
    }
    if(beneath(dst,src)||beneath(src,dst))throw std::runtime_error("源目录和目标目录不能互相包含");
    if(fs::exists(dst)&&(!fs::is_directory(dst)||!fs::is_empty(dst)))
        throw std::runtime_error("目标目录已有数据，请选择空的专用文件夹");
    const auto bytes=utf8(src.wstring());
    const auto markerName=L"Local\\DeskFlow-"+wide(sha256(bytes.data(),bytes.size()));
    SetLastError(ERROR_SUCCESS);
    Handle marker(CreateMutexW(nullptr,FALSE,markerName.c_str()));
    if(!marker.value||GetLastError()==ERROR_ALREADY_EXISTS)
        throw std::runtime_error("请先关闭 DeskFlow 再迁移数据库");
    fs::create_directories(dst.parent_path());
    uint64_t required=128ULL*1024*1024;
    if(fs::exists(src))for(const auto& item:fs::recursive_directory_iterator(src)){
        const auto attributes=GetFileAttributesW(item.path().c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("原数据目录包含链接，请先使用备份功能迁移");
        if(item.is_regular_file())required+=item.file_size();
    }
    if(fs::space(dst.parent_path()).available<required)throw std::runtime_error("目标磁盘剩余空间不足，原数据已保留");
    const auto stage=dst.parent_path()/(L".DeskFlow-migration-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto stageParent=physicalDirectory(dst.parent_path());
    fs::create_directory(stage);
    StorageMigration report;
    try {
        if(fs::exists(src))for(const auto& item:fs::recursive_directory_iterator(src)){
            const auto relative=fs::relative(item.path(),src);const auto output=stage/relative;
            if(item.is_directory()){fs::create_directories(output);continue;}
            if(!item.is_regular_file())throw std::runtime_error("数据目录包含不支持的文件类型");
            if(relative==L"history.db-wal"||relative==L"history.db-shm"||relative==L"files.db-wal"||relative==L"files.db-shm")continue;
            fs::create_directories(output.parent_path());
            if(relative==L"history.db"||relative==L"files.db")backupDatabase(item.path(),output);
            else copyStorageFile(item.path(),output);
            ++report.files;report.bytes+=fs::file_size(output);
            if(progress)progress(report.files,report.bytes);
        }
        if(fs::exists(dst)&&!RemoveDirectoryW(dst.c_str()))throw std::runtime_error("目标目录已被其他程序使用");
        if(!MoveFileExW(stage.c_str(),dst.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("数据迁移提交失败，原数据已保留");
        if(commitSelector)configureDataDirectory(dst);
        report.changed=true;return report;
    }catch(...){
        // Cleanup is confined to the exact staging directory created above.
        if(physicalDirectory(stage).parent_path()==stageParent&&stage.filename().wstring().starts_with(L".DeskFlow-migration-")){
            std::error_code error;fs::remove_all(stage,error);
        }
        throw;
    }
}
}
