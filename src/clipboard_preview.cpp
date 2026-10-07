#include "clipboard_preview.hpp"
#include "common.hpp"
#include "image_tools.hpp"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <ctime>
#include <cstring>
#include <cctype>

namespace desk {
ClipboardPreviewText clipboardPreviewText(const ClipPayload& payload) {
    for(const auto& format:payload.formats)if(format.format==CF_HDROP&&format.data.size()>=sizeof(DROPFILES)){
        DROPFILES header{};memcpy(&header,format.data.data(),sizeof(header));if(header.pFiles>=format.data.size())continue;
        std::wstring text;
        if(header.fWide){size_t count=std::min<size_t>((format.data.size()-header.pFiles)/2,16000);text.resize(count);memcpy(text.data(),format.data.data()+header.pFiles,count*2);}
        else {int count=(int)std::min<size_t>(format.data.size()-header.pFiles,16000);int length=MultiByteToWideChar(CP_ACP,0,(const char*)format.data.data()+header.pFiles,count,nullptr,0);text.resize(length);if(length)MultiByteToWideChar(CP_ACP,0,(const char*)format.data.data()+header.pFiles,count,text.data(),length);}
        std::wstring list;size_t begin=0;while(begin<text.size()&&text[begin]){auto end=text.find(L'\0',begin);if(end==std::wstring::npos)end=text.size();if(!list.empty())list+=L"\r\n";list+=text.substr(begin,end-begin);begin=end+1;}return {list,true};
    }
    for(const auto& format:payload.formats)if(format.format==CF_UNICODETEXT&&format.data.size()>=2){
        size_t count=std::min<size_t>(format.data.size()/2,16000);std::wstring text(count,L'\0');
        memcpy(text.data(),format.data.data(),count*2);if(auto end=text.find(L'\0');end!=std::wstring::npos)text.resize(end);
        return {std::move(text),true};
    }
    for(const auto& format:payload.formats)if(format.name==L"HTML Format"){
        std::string html((const char*)format.data.data(),std::min<size_t>(format.data.size(),128*1024));
        auto start=html.find("<!--StartFragment-->");if(start!=std::string::npos)html=html.substr(start+20);
        auto end=html.find("<!--EndFragment-->");if(end!=std::string::npos)html.resize(end);
        std::string text;bool tag=false;std::string name;
        for(char ch:html){if(ch=='<'){tag=true;name.clear();}else if(ch=='>'&&tag){tag=false;if(name=="br"||name=="br/"||name=="/p"||name=="/div")text+='\n';}else if(tag){if(name.size()<32)name+=(char)std::tolower((unsigned char)ch);}else if(text.size()<32000)text+=ch;}
        for(auto pair:{std::pair{"&nbsp;"," "},std::pair{"&amp;","&"},std::pair{"&lt;","<"},std::pair{"&gt;",">"},std::pair{"&quot;","\""}}){size_t pos=0;while((pos=text.find(pair.first,pos))!=std::string::npos){text.replace(pos,strlen(pair.first),pair.second);pos+=strlen(pair.second);}}
        try {return {wide(text),true};}catch(...){}
    }
    for(const auto& format:payload.formats)if(format.format==CF_TEXT&&!format.data.empty()){
        int count=(int)std::min<size_t>(format.data.size(),16000);int length=MultiByteToWideChar(CP_ACP,0,(const char*)format.data.data(),count,nullptr,0);
        std::wstring text(length,L'\0');if(length)MultiByteToWideChar(CP_ACP,0,(const char*)format.data.data(),count,text.data(),length);
        if(auto end=text.find(L'\0');end!=std::wstring::npos)text.resize(end);return {std::move(text),true};
    }
    return {};
}
std::wstring clipboardDetails(const HistoryItem& item,const ClipPayload* payload){
    auto time=(time_t)(item.created/1000);tm local{};localtime_s(&local,&time);wchar_t stamp[64]{};wcsftime(stamp,64,L"%Y-%m-%d %H:%M:%S",&local);
    std::wstring text=L"类型："+item.kind+L"\r\n来源："+(item.source.empty()?L"未知应用":item.source)+L"\r\n时间："+stamp+L"\r\n大小："+std::to_wstring(item.bytes)+L" 字节\r\n收藏："+(item.pinned?L"是":L"否");
    if(payload){text+=L"\r\n\r\n保留格式：";for(const auto& f:payload->formats){
        std::wstring name=f.name;if(name.empty()){switch(f.format){case CF_UNICODETEXT:name=L"Unicode 文本";break;case CF_TEXT:name=L"文本";break;case CF_DIB:case CF_DIBV5:name=L"位图";break;case CF_HDROP:name=L"文件列表";break;default:name=L"格式 "+std::to_wstring(f.format);}}
        text+=L"\r\n"+name+L" · "+std::to_wstring(f.data.size())+L" 字节";
    }}return text;
}
struct FloatingClipboardPreview::Impl {
    HWND hwnd{},edit{};HFONT font{};HBRUSH paper{};HBITMAP bitmap{};
    image_tools_detail::MemoryLease memory;bool dark=false;std::wstring title;
    ~Impl(){if(IsWindow(hwnd))DestroyWindow(hwnd);clear();if(font)DeleteObject(font);if(paper)DeleteObject(paper);}
    void clear(){if(bitmap)DeleteObject(bitmap);bitmap=nullptr;memory.reset();}
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){
        auto state=(Impl*)GetWindowLongPtrW(window,GWLP_USERDATA);
        if(message==WM_NCCREATE){state=(Impl*)((CREATESTRUCTW*)lp)->lpCreateParams;state->hwnd=window;SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)state);}
        if(!state)return DefWindowProcW(window,message,wp,lp);
        if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        if(message==WM_CLOSE){ShowWindow(window,SW_HIDE);state->clear();SetWindowTextW(state->edit,L"");return 0;}
        if(message==WM_LBUTTONDOWN){RECT r{};GetClientRect(window,&r);if(GET_X_LPARAM(lp)>r.right-35&&GET_Y_LPARAM(lp)<34)SendMessageW(window,WM_CLOSE,0,0);return 0;}
        if(message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT){SetTextColor((HDC)wp,state->dark?RGB(235,237,242):RGB(35,40,48));SetBkColor((HDC)wp,state->dark?RGB(29,32,39):RGB(255,255,255));return(LRESULT)state->paper;}
        if(message==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(window,&ps);RECT r{};GetClientRect(window,&r);FillRect(dc,&r,state->paper);auto old=SelectObject(dc,state->font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,state->dark?RGB(235,237,242):RGB(35,40,48));
            RECT title{12,8,r.right-34,34};DrawTextW(dc,state->title.c_str(),-1,&title,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);RECT close{r.right-32,6,r.right-4,34};DrawTextW(dc,L"×",-1,&close,DT_CENTER|DT_SINGLELINE);
            if(state->bitmap){BITMAP image{};GetObjectW(state->bitmap,sizeof(image),&image);double scale=std::min((r.right-24.)/image.bmWidth,(r.bottom-48.)/image.bmHeight);int width=(int)(image.bmWidth*scale),height=(int)(image.bmHeight*scale);auto source=CreateCompatibleDC(dc);auto previous=SelectObject(source,state->bitmap);SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);StretchBlt(dc,(r.right-width)/2,40+(r.bottom-40-height)/2,width,height,source,0,0,image.bmWidth,image.bmHeight,SRCCOPY);SelectObject(source,previous);DeleteDC(source);}
            SelectObject(dc,old);EndPaint(window,&ps);return 0;}
        if(message==WM_NCDESTROY){state->hwnd=nullptr;state->edit=nullptr;SetWindowLongPtrW(window,GWLP_USERDATA,0);}
        return DefWindowProcW(window,message,wp,lp);
    }
};
FloatingClipboardPreview::FloatingClipboardPreview():impl(std::make_unique<Impl>()){}
FloatingClipboardPreview::~FloatingClipboardPreview()=default;
HWND FloatingClipboardPreview::window()const{return impl->hwnd;}
void FloatingClipboardPreview::hide(){if(IsWindow(impl->hwnd)){ShowWindow(impl->hwnd,SW_HIDE);SetWindowTextW(impl->edit,L"");}impl->clear();}
void FloatingClipboardPreview::show(HWND owner,const HistoryItem& item,const std::wstring& text,HBITMAP bitmap,RECT anchor,bool content,bool dark){
    auto& state=*impl;state.clear();state.dark=dark;state.title=content?L"内容预览":L"详细信息";
    BITMAP size{};if(bitmap&&GetObjectW(bitmap,sizeof(size),&size)){state.memory=image_tools_detail::reserveMemory((uint64_t)size.bmWidth*size.bmHeight*4);if(state.memory)state.bitmap=bitmap;else DeleteObject(bitmap);}else if(bitmap)DeleteObject(bitmap);
    if(state.paper)DeleteObject(state.paper);state.paper=CreateSolidBrush(dark?RGB(29,32,39):RGB(255,255,255));
    const float dpi=GetDpiForWindow(owner)/96.f;
    if(!state.hwnd){WNDCLASSW wc{};wc.lpfnWndProc=Impl::procedure;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DeskFlowClipboardPreview";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
        auto hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"剪贴板预览",WS_POPUP|WS_BORDER,0,0,420,340,owner,nullptr,wc.hInstance,&state);if(!hwnd){state.clear();return;}
        state.edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,12,40,390,280,hwnd,nullptr,wc.hInstance,nullptr);SendMessageW(state.edit,EM_SETLIMITTEXT,20000,0);
    }
    auto font=CreateFontW(-(int)(14*dpi),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");if(font){SendMessageW(state.edit,WM_SETFONT,(WPARAM)font,TRUE);if(state.font)DeleteObject(state.font);state.font=font;}
    MONITORINFO monitor{sizeof(monitor)};if(!GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&monitor))monitor.rcWork={0,0,std::max(800,GetSystemMetrics(SM_CXSCREEN)),std::max(600,GetSystemMetrics(SM_CYSCREEN))};
    int width=std::min<int>((int)(440*dpi),monitor.rcWork.right-monitor.rcWork.left-16),height=std::min<int>((int)((state.bitmap?400:280)*dpi),monitor.rcWork.bottom-monitor.rcWork.top-16);
    int x=anchor.right+8;if(x+width>monitor.rcWork.right)x=anchor.left-width-8;x=std::clamp(x,(int)monitor.rcWork.left+8,std::max((int)monitor.rcWork.left+8,(int)monitor.rcWork.right-width-8));
    int y=std::clamp((int)anchor.top,(int)monitor.rcWork.top+8,std::max((int)monitor.rcWork.top+8,(int)monitor.rcWork.bottom-height-8));
    SetWindowPos(state.hwnd,HWND_TOP,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);MoveWindow(state.edit,12,(int)(40*dpi),width-26,height-(int)(52*dpi),TRUE);
    SetWindowTextW(state.edit,text.empty()?clipboardDetails(item).c_str():text.c_str());ShowWindow(state.edit,state.bitmap?SW_HIDE:SW_SHOW);InvalidateRect(state.hwnd,nullptr,TRUE);
}
}
