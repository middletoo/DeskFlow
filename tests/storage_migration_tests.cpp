#include "storage.hpp"
#include "clipboard.hpp"
#include "common.hpp"
#include <fstream>
#include <iostream>
#include <vector>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct RegistrySandbox{
    HKEY key{};std::wstring name=L"Software\\DeskFlow-storage-test-"+std::to_wstring(GetCurrentProcessId());
    RegistrySandbox(){
        require(RegCreateKeyExW(HKEY_CURRENT_USER,name.c_str(),0,nullptr,REG_OPTION_NON_VOLATILE,KEY_ALL_ACCESS|KEY_WOW64_64KEY,nullptr,&key,nullptr)==ERROR_SUCCESS,"create isolated storage registry");
        require(RegOverridePredefKey(HKEY_CURRENT_USER,key)==ERROR_SUCCESS,"isolate storage registry");
    }
    ~RegistrySandbox(){RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);if(key)RegCloseKey(key);RegDeleteTreeW(HKEY_CURRENT_USER,name.c_str());}
};
int main(){
    namespace fs=std::filesystem;
    const auto base=fs::temp_directory_path()/(L"DeskFlow-storage-migration-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto source=base/L"source",target=base/L"target";
    int status=0;
    try{
        int64_t id=0;
        {
            desk::HistoryStore history(source);
            desk::ClipPayload payload;payload.source=L"Synthetic fixture";
            std::wstring text=L"迁移测试 Clipboard history";
            desk::ClipFormat format;format.format=CF_UNICODETEXT;
            format.data.resize((text.size()+1)*sizeof(wchar_t));memcpy(format.data.data(),text.c_str(),format.data.size());
            payload.formats.push_back(format);id=history.append(payload);history.setPinned(id,true);
            std::ofstream(source/L"settings.json")<<"{\"synthetic\":true}";
            desk::Database index(source/L"files.db");index.exec("CREATE TABLE synthetic(value TEXT);INSERT INTO synthetic VALUES('kept');");
        }
        auto moved=desk::migrateDataDirectory(source,target);
        require(moved.changed&&moved.files>=4,"storage migration failed to copy database/settings/objects");
        require(fs::exists(source/L"history.db")&&fs::exists(source/L"settings.json"),"migration removed the original data directory");
        {
            desk::HistoryStore migrated(target);auto rows=migrated.list();
            require(rows.size()==1&&rows[0].id==id&&rows[0].pinned,"migration changed history IDs or pin state");
            auto payload=migrated.load(id);
            require(payload.formats.size()==1&&payload.formats[0].data.size()>20,"migration lost history attachment bytes");
        }
        bool rejected=false;try{desk::migrateDataDirectory(source,target);}catch(...){rejected=true;}
        require(rejected,"nonempty destination must not be merged or overwritten");
        rejected=false;try{desk::migrateDataDirectory(source,source/L"nested");}catch(...){rejected=true;}
        require(rejected,"nested migration destination must be rejected");
        require(!desk::migrateDataDirectory(source,source).changed,"same-directory selection should preserve data");
        auto broken=base/L"broken";fs::create_directories(broken);std::ofstream(broken/L"history.db")<<"broken synthetic DB";
        rejected=false;try{desk::migrateDataDirectory(broken,base/L"failed-target");}catch(...){rejected=true;}
        require(rejected&&fs::exists(broken/L"history.db")&&!fs::exists(base/L"failed-target"),"failed migration must keep source and avoid publishing a partial destination");
        {
            RegistrySandbox registry;
            require(!desk::configuredDataDirectory(),"isolated selector starts empty");
            desk::configureDataDirectory(target);
            require(desk::configuredDataDirectory()==desk::physicalDirectory(target),"installer choice persisted in registry");
            require(desk::dataDirectory()==desk::physicalDirectory(target),"application must use the chosen data directory");
            require(desk::migrateDataDirectory(source,base/L"committed",{},true).changed,"committed migration");
            require(desk::dataDirectory()==desk::physicalDirectory(base/L"committed"),"successful migration updates app routing");
            rejected=false;try{desk::migrateDataDirectory(broken,base/L"invalid-commit",{},true);}catch(...){rejected=true;}
            require(rejected&&desk::dataDirectory()==desk::physicalDirectory(base/L"committed"),"failed migration must not change the previous selector");
        }
        if(EncryptFileW((source/L"settings.json").c_str())){
            desk::migrateDataDirectory(source,base/L"encrypted");
            require(GetFileAttributesW((base/L"encrypted"/L"settings.json").c_str())&FILE_ATTRIBUTE_ENCRYPTED,"migration preserves EFS encryption");
        }
        std::cout<<"PASS storage migration, IDs/attachments/pins, retained source and failure safety\n";
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';status=1;}
    const auto checked=fs::weakly_canonical(base);
    if(checked.parent_path()==fs::weakly_canonical(fs::temp_directory_path())&&checked.filename().wstring().starts_with(L"DeskFlow-storage-migration-")){
        std::error_code error;fs::remove_all(checked,error);
    }
    return status;
}
