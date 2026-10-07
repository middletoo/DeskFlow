#include "search.hpp"
#include "sqlite3.h"
#include <iostream>
#include <stdexcept>

int main(){
    auto data=std::filesystem::temp_directory_path()/(L"DeskMetadataMigration-"+std::to_wstring(GetCurrentProcessId()));
    int outcome=0;sqlite3* db{};
    try{
        {desk::SearchStore initialize(data);}
        if(sqlite3_open((data/L"files.db").string().c_str(),&db)!=SQLITE_OK)throw std::runtime_error("open synthetic schema");
        // Model an earlier index with no modification-time column. A new
        // read-only client must still list existing filenames immediately.
        const char* sql=
            "DROP TRIGGER files_modified_insert; DROP TRIGGER files_modified_update;"
            "ALTER TABLE files DROP COLUMN modified;"
            "DELETE FROM search_meta WHERE key IN('modified_complete','modified_cursor','sort_modified');"
            "CREATE INDEX files_sort_name ON files(name_fold,id);"
            "UPDATE search_meta SET value='1' WHERE key='sort_name';"
            "DROP TRIGGER files_insert; DROP TRIGGER files_short_insert; DROP TRIGGER files_type_insert; DROP TRIGGER files_size_insert;"
            "INSERT INTO search_roots(id,path,path_key) VALUES(1,'c:\\demo','c:\\demo');"
            "INSERT INTO files(root_id,path,path_fold,name,name_fold,folder,size,seen) VALUES(1,'c:\\demo\\legacy.txt','c:\\demo\\legacy.txt','legacy.txt','legacy.txt',0,7,1);";
        char* error{};
        if(sqlite3_exec(db,sql,nullptr,nullptr,&error)!=SQLITE_OK){std::string message=error?error:"schema fixture";sqlite3_free(error);throw std::runtime_error(message);}
        sqlite3_close(db);db=nullptr;
        desk::SearchStore reader(data);desk::SearchQuery query;
        auto rows=reader.query(query);
        if(rows.size()!=1||rows.front().name!=L"legacy.txt"||rows.front().modified!=0)throw std::runtime_error("legacy read-only results must remain available with unknown timestamps");
        sqlite3_open((data/L"files.db").string().c_str(),&db);sqlite3_stmt* columns{};
        sqlite3_prepare_v2(db,"PRAGMA table_info(files)",-1,&columns,nullptr);bool added=false;
        while(sqlite3_step(columns)==SQLITE_ROW)if(std::string((const char*)sqlite3_column_text(columns,1))=="modified")added=true;
        sqlite3_finalize(columns);
        if(added)throw std::runtime_error("foreground reader must not migrate or write the legacy database");
        std::cout<<"PASS legacy read-only filename results during timestamp migration\n";
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';outcome=1;}
    if(db)sqlite3_close(db);std::error_code error;std::filesystem::remove_all(data,error);return outcome;
}
