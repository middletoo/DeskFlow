#pragma once
#include "search.hpp"
#include <algorithm>

namespace desk {
inline std::wstring indexCountDigits(int64_t value){
    auto text=std::to_wstring(std::max<int64_t>(0,value));
    for(int position=(int)text.size()-3;position>0;position-=3)text.insert(position,1,L',');
    return text;
}
inline std::wstring indexObjectCount(int64_t value){return indexCountDigits(value)+L" 个对象";}
inline std::wstring indexDuration(int64_t seconds){
    seconds=std::max<int64_t>(0,seconds);
    if(seconds>=3600)return std::to_wstring(seconds/3600)+L"小时"+std::to_wstring(seconds%3600/60)+L"分";
    if(seconds>=60)return std::to_wstring(seconds/60)+L"分"+std::to_wstring(seconds%60)+L"秒";
    return std::to_wstring(seconds)+L"秒";
}
inline std::wstring indexProgressLabel(const SearchStatus& status){
    if(status.complete)return status.limited?L"扫描完成 · 存在未覆盖项":status.stage==L"metadata"?L"索引就绪 · 大小与时间补齐中":L"索引就绪";
    if(!status.building)return status.stage==L"stopped"?L"索引未运行 · 覆盖尚未确认":L"正在准备索引";
    std::wstring result=status.stage==L"records"?L"读取文件记录":status.stage==L"paths"?L"建立可搜索路径":status.stage==L"changes"?L"校准建库期间的文件变化":L"校准目录与硬链接";
    if(status.stage==L"records")result+=L"（"+indexCountDigits(status.readRecords)+L"）";
    result+=L" · 已用 "+indexDuration(status.elapsedSeconds);
    if(status.estimatedStageSeconds>=0)result+=L" · 本阶段预计剩余 "+indexDuration(status.estimatedStageSeconds);
    else result+=L" · 剩余时间估算中";
    return result;
}
inline std::wstring indexEmptyLabel(const SearchStatus& status){
    if(!status.complete)return status.building?L"索引尚未完成，暂未找到匹配项":L"索引覆盖尚未确认，暂未找到匹配项";
    return status.limited?L"暂无匹配项，存在未覆盖或受限路径":L"没有匹配文件";
}
}
