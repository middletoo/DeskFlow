#pragma once
#include <algorithm>
namespace desk::panel_layout {
constexpr float menuHeight=30, searchTop=36, searchHeight=26;
constexpr float fileHeaderTop=70, fileRowsTop=96, historyRowsTop=70;
constexpr float fileRowHeight=26, historyRowHeight=28, margin=8;
constexpr float fileFooterHeight=25;
inline float rowTop(int mode){return mode==0?fileRowsTop:historyRowsTop;}
inline float rowHeight(int mode){return mode==0?fileRowHeight:historyRowHeight;}
inline float listBottom(int mode,float height){return height-margin-(mode==0?fileFooterHeight:0);}
inline int visible(int mode,float height){return std::max(1,(int)((listBottom(mode,height)-rowTop(mode))/rowHeight(mode)));}
inline float nameEnd(float width,bool details){return std::max(144.f,(width-(details?340.f:256.f))*.42f);}
inline float pathEnd(float width,bool details){return width-(details?340.f:256.f);}
inline float typeEnd(float width){return width-256.f;}
inline float sizeEnd(float width){return width-168.f;}
enum class FileColumn {None,Name,Path,Type,Size,Modified};
enum class DoubleClick {None,Locate,CopyPath};
inline FileColumn column(float x,float width,bool details){
    if(x<margin||x>=width-margin)return FileColumn::None;
    if(x<nameEnd(width,details))return FileColumn::Name;
    if(x<pathEnd(width,details))return FileColumn::Path;
    if(details&&x<typeEnd(width))return FileColumn::Type;
    return x<sizeEnd(width)?FileColumn::Size:FileColumn::Modified;
}
inline DoubleClick doubleClick(float x,float width,bool details){auto value=column(x,width,details);return value==FileColumn::Name?DoubleClick::Locate:value==FileColumn::Path?DoubleClick::CopyPath:DoubleClick::None;}
}
