#pragma once
#include <algorithm>
namespace desk::panel_layout {
constexpr float menuHeight=30, searchTop=36, searchHeight=26;
constexpr float fileHeaderTop=70, fileRowsTop=96, historyRowsTop=70;
constexpr float fileRowHeight=26, historyRowHeight=28, margin=8;
inline float rowTop(int mode){return mode==0?fileRowsTop:historyRowsTop;}
inline float rowHeight(int mode){return mode==0?fileRowHeight:historyRowHeight;}
inline int visible(int mode,float height){return std::max(1,(int)((height-margin-rowTop(mode))/rowHeight(mode)));}
inline float nameEnd(float width,bool details){return details?std::max(180.f,(width-178.f)*.42f):std::max(180.f,width*.38f);}
inline float pathEnd(float width,bool details){return details?width-166.f:width-margin;}
enum class FileColumn {None,Name,Path,Type,Size};
enum class DoubleClick {None,Locate,CopyPath};
inline FileColumn column(float x,float width,bool details){
    if(x<margin||x>=width-margin)return FileColumn::None;
    if(x<nameEnd(width,details))return FileColumn::Name;
    if(x<pathEnd(width,details))return FileColumn::Path;
    return x<width-84?FileColumn::Type:FileColumn::Size;
}
inline DoubleClick doubleClick(float x,float width,bool details){auto value=column(x,width,details);return value==FileColumn::Name?DoubleClick::Locate:value==FileColumn::Path?DoubleClick::CopyPath:DoubleClick::None;}
}
