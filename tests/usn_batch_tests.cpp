#include "usn_batch.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    struct Event {uint64_t file, parent;int64_t stamp;DWORD reason, attributes;std::wstring name;};
    std::vector<Event> emitted;
    desk::search_detail::UsnMetadataBatch batch([&](auto file, auto parent, auto stamp,
        auto reason, auto attributes, const auto& name) {
        emitted.push_back({file,parent,stamp,reason,attributes,name});
    });
    try {
        for (int i=0;i<10000;++i)
            batch.add(7,5,i,USN_REASON_DATA_EXTEND,FILE_ATTRIBUTE_NORMAL,L"log.txt");
        batch.add(7,5,10000,USN_REASON_BASIC_INFO_CHANGE|USN_REASON_CLOSE,FILE_ATTRIBUTE_READONLY,L"log.txt");
        batch.add(8,5,10001,USN_REASON_FILE_CREATE,FILE_ATTRIBUTE_NORMAL,L"created.txt");
        require(emitted.size()==2 && emitted[0].stamp==10000 &&
                emitted[0].attributes==FILE_ATTRIBUTE_READONLY &&
                (emitted[0].reason&USN_REASON_DATA_EXTEND) && emitted[1].file==8,
                "duplicate metadata did not collapse before namespace barrier");
        emitted.clear();
        batch.add(7,5,1,USN_REASON_DATA_OVERWRITE,FILE_ATTRIBUTE_NORMAL,L"old.txt");
        batch.add(7,5,2,USN_REASON_RENAME_OLD_NAME,FILE_ATTRIBUTE_NORMAL,L"old.txt");
        batch.add(7,9,3,USN_REASON_RENAME_NEW_NAME,FILE_ATTRIBUTE_NORMAL,L"new.txt");
        batch.add(7,9,4,USN_REASON_DATA_EXTEND,FILE_ATTRIBUTE_NORMAL,L"new.txt");
        batch.add(7,9,5,USN_REASON_FILE_DELETE,FILE_ATTRIBUTE_NORMAL,L"new.txt");
        batch.add(7,9,6,USN_REASON_FILE_CREATE,FILE_ATTRIBUTE_NORMAL,L"recreated.txt");
        batch.flush();
        require(emitted.size()==6 && emitted[0].name==L"old.txt" &&
                emitted[2].parent==9 && emitted[4].reason==USN_REASON_FILE_DELETE &&
                emitted[5].name==L"recreated.txt", "rename or delete/recreate order changed");
        emitted.clear();
        batch.add(7,5,1,USN_REASON_CLOSE,FILE_ATTRIBUTE_NORMAL,L"alias-a.txt");
        batch.add(7,9,2,USN_REASON_CLOSE,FILE_ATTRIBUTE_NORMAL,L"alias-b.txt");
        batch.add(9,5,3,USN_REASON_BASIC_INFO_CHANGE,FILE_ATTRIBUTE_DIRECTORY,L"parent");
        batch.add(7,9,4,USN_REASON_CLOSE,FILE_ATTRIBUTE_NORMAL,L"alias-b.txt");
        batch.flush();
        require(emitted.size()==4 && emitted[0].parent==5 && emitted[1].parent==9 &&
                emitted[2].attributes==FILE_ATTRIBUTE_DIRECTORY && emitted[3].stamp==4,
                "hardlink names or directory barrier were collapsed");
        std::cout<<"PASS duplicate USN metadata, namespace ordering, hardlinks and directory barriers\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
