#include "search.hpp"
#include <iostream>

int wmain(int argc,wchar_t** argv) {
    std::filesystem::path data;
    std::vector<std::wstring> roots;
    DWORD parent=0;
    try {
        for(int i=1;i<argc;++i) {
            const std::wstring argument=argv[i];
            if(argument==L"--data" && i+1<argc) data=argv[++i];
            else if(argument==L"--parent" && i+1<argc) parent=static_cast<DWORD>(std::stoul(argv[++i]));
            else if(argument==L"--root" && i+1<argc) roots.emplace_back(argv[++i]);
            else if(argument==L"--help") {
                std::wcout<<L"DeskIndex --data <directory> [--parent <PID>] [--root <directory>]\n";
                return 0;
            } else if(!argument.starts_with(L"--")) roots.push_back(argument);
            else throw std::runtime_error("Unknown or incomplete argument");
        }
        if(data.empty()) throw std::runtime_error("--data is required");
        return desk::indexWorkerMain(data,roots,parent);
    } catch(const std::exception& error) {std::cerr<<error.what()<<"\n";return 2;}
}
