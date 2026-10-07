#include "search.hpp"
#include "common.hpp"
#include "json.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    std::filesystem::path data,report;
    for(int i=1;i+1<argc;++i){if(std::wstring(argv[i])==L"--data")data=argv[++i];else if(std::wstring(argv[i])==L"--report")report=argv[++i];}
    if(data.empty())return 2;
    try {
        desk::SearchStore store(data);nlohmann::json checks=nlohmann::json::array();
        for(auto sort:{desk::SearchSort::Name,desk::SearchSort::Size,desk::SearchSort::Path,desk::SearchSort::Type}) {
            desk::SearchQuery query;query.text=L"AI 金融";query.sort=sort;
            auto start=std::chrono::steady_clock::now();auto rows=store.query(query);
            auto first=std::chrono::steady_clock::now();int slices=1;
            while(store.queryPending()&&std::chrono::steady_clock::now()-start<std::chrono::seconds(3)){rows=store.query(query);++slices;}
            checks.push_back({{"sort",(int)sort},{"firstMs",std::chrono::duration<double,std::milli>(first-start).count()},
                {"totalMs",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()},
                {"rows",rows.size()},{"pending",store.queryPending()},{"slices",slices}});
        }
        auto state=store.status();nlohmann::json result{{"indexedRows",state.total},{"backgroundBuilding",state.building},{"status",desk::utf8(state.message)},{"checks",checks}};
        if(!report.empty()){std::ofstream output(report);output<<result.dump(2);}std::cout<<result.dump(2)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
