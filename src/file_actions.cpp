#include "file_actions.hpp"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <algorithm>
namespace desk {
namespace {
using Microsoft::WRL::ComPtr;
struct Pidls {
    std::vector<PIDLIST_ABSOLUTE> values;
    ~Pidls(){for(auto p:values)CoTaskMemFree(p);}
};
HRESULT shellArray(const std::vector<std::wstring>& paths,IShellItemArray** output) {
    if(!output) return E_POINTER;
    *output=nullptr;
    if(paths.empty()||paths.size()>100) return E_INVALIDARG;
    Pidls ids;
    for(const auto& path:paths){
        PIDLIST_ABSOLUTE id=nullptr;
        auto hr=SHParseDisplayName(path.c_str(),nullptr,&id,0,nullptr);
        if(FAILED(hr))return hr;
        ids.values.push_back(id);
    }
    return SHCreateShellItemArrayFromIDLists((UINT)ids.values.size(),
        const_cast<PCIDLIST_ABSOLUTE*>(ids.values.data()),output);
}
HRESULT contextMenu(const std::vector<std::wstring>& paths,IContextMenu** output) {
    ComPtr<IShellItemArray> items;
    auto hr=shellArray(paths,&items);
    return FAILED(hr)?hr:items->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(output));
}
struct MenuMessages {
    ComPtr<IContextMenu2> two;
    ComPtr<IContextMenu3> three;
    static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR ref){
        auto self=reinterpret_cast<MenuMessages*>(ref);
        if(msg==WM_INITMENUPOPUP||msg==WM_DRAWITEM||msg==WM_MEASUREITEM||msg==WM_MENUCHAR){
            LRESULT result=0;
            if(self->three&&SUCCEEDED(self->three->HandleMenuMsg2(msg,wp,lp,&result)))return result;
            if(self->two&&SUCCEEDED(self->two->HandleMenuMsg(msg,wp,lp)))return 0;
        }
        return DefSubclassProc(hwnd,msg,wp,lp);
    }
};
HRESULT invoke(HWND owner,IContextMenu* menu,const char* verb){
    CMINVOKECOMMANDINFOEX command{};command.cbSize=sizeof(command);
    command.fMask=CMIC_MASK_NOASYNC;
    command.hwnd=owner;command.lpVerb=verb;command.nShow=SW_SHOWNORMAL;
    return menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&command));
}
class DropSource final:public IDropSource {
    LONG refs{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** result)override{
        if(!result)return E_POINTER;*result=nullptr;
        if(id==IID_IUnknown||id==IID_IDropSource){*result=static_cast<IDropSource*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,DWORD keys)override{
        if(escape)return DRAGDROP_S_CANCEL;
        if(!(keys&MK_LBUTTON))return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD)override{return DRAGDROP_S_USEDEFAULTCURSORS;}
};
bool cancelled(const std::atomic_int* version,int expected){return version&&version->load()!=expected;}
std::wstring decodeText(const std::vector<char>& bytes){
    if(bytes.empty())return {};
    size_t offset=0;
    if(bytes.size()>=2&&((BYTE)bytes[0]==0xff&&(BYTE)bytes[1]==0xfe||(BYTE)bytes[0]==0xfe&&(BYTE)bytes[1]==0xff)){
        bool little=(BYTE)bytes[0]==0xff;std::wstring out;
        for(size_t i=2;i+1<bytes.size();i+=2)out.push_back((wchar_t)(little?((BYTE)bytes[i]|(BYTE)bytes[i+1]<<8):((BYTE)bytes[i]<<8|(BYTE)bytes[i+1])));
        return out;
    }
    if(bytes.size()>=3&&(BYTE)bytes[0]==0xef&&(BYTE)bytes[1]==0xbb&&(BYTE)bytes[2]==0xbf)offset=3;
    if(std::find(bytes.begin()+offset,bytes.end(),'\0')!=bytes.end())return L"二进制文件：可通过打开或系统属性查看。";
    size_t length=bytes.size()-offset;
    // The bounded preview may end halfway through a valid UTF-8 code point.
    // Drop just that incomplete suffix, retaining the rest of the file encoding.
    size_t lead=bytes.size();
    while(lead>offset&&lead+3>bytes.size()&&((BYTE)bytes[lead-1]&0xc0)==0x80)--lead;
    if(lead>offset){
        BYTE first=(BYTE)bytes[lead-1];
        size_t expected=first>=0xc2&&first<=0xdf?2:first>=0xe0&&first<=0xef?3:first>=0xf0&&first<=0xf4?4:1;
        if(expected>bytes.size()-(lead-1))length=lead-1-offset;
    }
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data()+offset,(int)length,nullptr,0);
    UINT page=CP_UTF8;DWORD flags=MB_ERR_INVALID_CHARS;
    if(!count){page=CP_ACP;flags=0;length=bytes.size()-offset;count=MultiByteToWideChar(page,flags,bytes.data()+offset,(int)length,nullptr,0);}
    std::wstring out(count,0);
    if(count)MultiByteToWideChar(page,flags,bytes.data()+offset,(int)length,out.data(),count);
    return out;
}
}
HRESULT makeFileDataObject(const std::vector<std::wstring>& paths,IDataObject** output){
    if(!output)return E_POINTER;*output=nullptr;
    ComPtr<IShellItemArray> items;auto hr=shellArray(paths,&items);
    return FAILED(hr)?hr:items->BindToHandler(nullptr,BHID_DataObject,IID_PPV_ARGS(output));
}
HRESULT copyFiles(const std::vector<std::wstring>& paths,bool cut){
    ComPtr<IDataObject> object;auto hr=makeFileDataObject(paths,&object);
    if(FAILED(hr))return hr;
    STGMEDIUM medium{};medium.tymed=TYMED_HGLOBAL;medium.hGlobal=GlobalAlloc(GMEM_MOVEABLE,sizeof(DWORD));
    if(!medium.hGlobal)return E_OUTOFMEMORY;
    auto data=static_cast<DWORD*>(GlobalLock(medium.hGlobal));
    if(!data){GlobalFree(medium.hGlobal);return E_OUTOFMEMORY;}
    *data=cut?DROPEFFECT_MOVE:DROPEFFECT_COPY;GlobalUnlock(medium.hGlobal);
    FORMATETC format{(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    hr=object->SetData(&format,&medium,TRUE);
    if(FAILED(hr))ReleaseStgMedium(&medium);
    if(SUCCEEDED(hr))hr=OleSetClipboard(object.Get());
    if(SUCCEEDED(hr))hr=OleFlushClipboard();
    return hr;
}
HRESULT dragFiles(const std::vector<std::wstring>& paths){
    ComPtr<IDataObject> object;auto hr=makeFileDataObject(paths,&object);if(FAILED(hr))return hr;
    ComPtr<IDropSource> source;source.Attach(new DropSource);
    DWORD effect=0;return DoDragDrop(object.Get(),source.Get(),DROPEFFECT_COPY|DROPEFFECT_MOVE|DROPEFFECT_LINK,&effect);
}
HRESULT showFileContextMenu(HWND owner,const std::vector<std::wstring>& paths,POINT screen,bool* renameRequested){
    if(renameRequested)*renameRequested=false;
    ComPtr<IContextMenu> menu;auto hr=contextMenu(paths,&menu);if(FAILED(hr))return hr;
    auto popup=CreatePopupMenu();if(!popup)return E_OUTOFMEMORY;
    UINT flags=CMF_NORMAL|CMF_CANRENAME;
    if(GetKeyState(VK_SHIFT)&0x8000)flags|=CMF_EXTENDEDVERBS;
    hr=menu->QueryContextMenu(popup,0,1,0x6fff,flags);
    if(SUCCEEDED(hr)){
        MenuMessages messages;menu.As(&messages.two);menu.As(&messages.three);
        SetWindowSubclass(owner,MenuMessages::procedure,0xDF31,(DWORD_PTR)&messages);
        SetForegroundWindow(owner);
        UINT chosen=TrackPopupMenuEx(popup,TPM_RETURNCMD|TPM_RIGHTBUTTON,screen.x,screen.y,owner,nullptr);
        RemoveWindowSubclass(owner,MenuMessages::procedure,0xDF31);
        char verb[128]{};
        if(chosen&&renameRequested&&SUCCEEDED(menu->GetCommandString(chosen-1,GCS_VERBA,nullptr,verb,sizeof(verb)))&&lstrcmpiA(verb,"rename")==0){
            *renameRequested=true;hr=S_OK;
        }else if(chosen){CMINVOKECOMMANDINFOEX command{};command.cbSize=sizeof(command);command.fMask=CMIC_MASK_UNICODE;
            command.hwnd=owner;command.lpVerb=MAKEINTRESOURCEA(chosen-1);command.lpVerbW=MAKEINTRESOURCEW(chosen-1);command.nShow=SW_SHOWNORMAL;
            hr=menu->InvokeCommand((CMINVOKECOMMANDINFO*)&command);
        }else hr=S_FALSE;
    }
    DestroyMenu(popup);return hr;
}
HRESULT showFileProperties(HWND owner,const std::vector<std::wstring>& paths){
    ComPtr<IContextMenu> menu;auto hr=contextMenu(paths,&menu);if(FAILED(hr))return hr;
    auto popup=CreatePopupMenu();if(!popup)return E_OUTOFMEMORY;
    hr=menu->QueryContextMenu(popup,0,1,0x6fff,CMF_NORMAL);
    if(SUCCEEDED(hr))hr=invoke(owner,menu.Get(),"properties");
    DestroyMenu(popup);return hr;
}
HRESULT renameFile(HWND owner,const std::wstring& path,const std::wstring& newName){
    if(newName.empty()||newName==L"."||newName==L".."||newName.find_first_of(L"\\/<>:\"|?*")!=std::wstring::npos||newName.back()==L'.'||newName.back()==L' ')return E_INVALIDARG;
    ComPtr<IShellItem> item;auto hr=SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item));if(FAILED(hr))return hr;
    ComPtr<IFileOperation> operation;hr=CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation));if(FAILED(hr))return hr;
    operation->SetOwnerWindow(owner);operation->SetOperationFlags(FOF_ALLOWUNDO|FOF_NOCONFIRMMKDIR);
    hr=operation->RenameItem(item.Get(),newName.c_str(),nullptr);if(SUCCEEDED(hr))hr=operation->PerformOperations();
    BOOL aborted=FALSE;operation->GetAnyOperationsAborted(&aborted);return aborted?HRESULT_FROM_WIN32(ERROR_CANCELLED):hr;
}
HRESULT recycleFiles(HWND owner,const std::vector<std::wstring>& paths){
    ComPtr<IShellItemArray> items;auto hr=shellArray(paths,&items);if(FAILED(hr))return hr;
    ComPtr<IFileOperation> operation;hr=CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation));if(FAILED(hr))return hr;
    operation->SetOwnerWindow(owner);operation->SetOperationFlags(FOF_ALLOWUNDO|FOFX_RECYCLEONDELETE);
    hr=operation->DeleteItems(items.Get());if(SUCCEEDED(hr))hr=operation->PerformOperations();
    BOOL aborted=FALSE;operation->GetAnyOperationsAborted(&aborted);return aborted?HRESULT_FROM_WIN32(ERROR_CANCELLED):hr;
}
HRESULT openFileLocation(HWND,const std::wstring& path){
    PIDLIST_ABSOLUTE full=nullptr;auto hr=SHParseDisplayName(path.c_str(),nullptr,&full,0,nullptr);if(FAILED(hr))return hr;
    auto parent=ILCloneFull(full);if(!parent){CoTaskMemFree(full);return E_OUTOFMEMORY;}
    PCUITEMID_CHILD child=ILFindLastID(full);ILRemoveLastID(parent);
    hr=SHOpenFolderAndSelectItems(parent,1,&child,0);CoTaskMemFree(parent);CoTaskMemFree(full);return hr;
}
std::wstring fileSizeLabel(uint64_t bytes){
    if(bytes<1024)return std::to_wstring(bytes)+L" B";
    const wchar_t* units[]{L"KiB",L"MiB",L"GiB",L"TiB"};double value=bytes;int unit=-1;
    do{value/=1024;++unit;}while(value>=1024&&unit<3);
    std::wostringstream text;text<<std::fixed<<std::setprecision(value>=100?0:1)<<value<<L" "<<units[unit];return text.str();
}
std::wstring fileModifiedLabel(uint64_t utcFileTime){
    if(!utcFileTime)return L"—";
    FILETIME stamp{(DWORD)utcFileTime,(DWORD)(utcFileTime>>32)};
    SYSTEMTIME utc{},local{};
    if(!FileTimeToSystemTime(&stamp,&utc)||!SystemTimeToTzSpecificLocalTimeEx(nullptr,&utc,&local))return L"—";
    wchar_t value[32]{};swprintf_s(value,L"%04u/%02u/%02u %02u:%02u",local.wYear,local.wMonth,local.wDay,local.wHour,local.wMinute);
    return value;
}
HBITMAP loadImageBitmap(const std::filesystem::path& path) {
    auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool uninitialize=SUCCEEDED(initialized);
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
    auto hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder);
    if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
    UINT width=0,height=0;if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);
    if(!width||!height||(uint64_t)width*height>32ULL*1024*1024)hr=E_INVALIDARG;
    if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&converter);
    if(SUCCEEDED(hr))hr=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    HBITMAP bitmap=nullptr;
    if(SUCCEEDED(hr)){
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-(LONG)height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void* pixels=nullptr;bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap)hr=E_OUTOFMEMORY;
        else hr=converter->CopyPixels(nullptr,width*4,width*height*4,(BYTE*)pixels);
    }
    converter.Reset();frame.Reset();decoder.Reset();factory.Reset();if(uninitialize)CoUninitialize();
    if(FAILED(hr)){if(bitmap)DeleteObject(bitmap);return nullptr;}return bitmap;
}
FilePreview loadFilePreview(const std::filesystem::path& path,const std::atomic_int* version,int expected){
    FilePreview result;if(cancelled(version,expected))return result;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)){result.text=L"文件当前不可访问，可能已移动或离线。";return result;}
    uint64_t bytes=(uint64_t(attributes.nFileSizeHigh)<<32)|attributes.nFileSizeLow;
    FILETIME local{};SYSTEMTIME time{};FileTimeToLocalFileTime(&attributes.ftLastWriteTime,&local);FileTimeToSystemTime(&local,&time);
    wchar_t date[80]{};swprintf_s(date,L"%04u-%02u-%02u %02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute);
    result.text=path.filename().wstring()+L"\n"+((attributes.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?L"文件夹":fileSizeLabel(bytes))+L"  ·  修改 "+date+L"\n"+path.parent_path().wstring();
    if(attributes.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)return result;
    auto ext=path.extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),towlower);
    const std::wstring images=L";.png;.jpg;.jpeg;.bmp;.gif;.tif;.tiff;.ico;.webp;";
    if(!ext.empty()&&images.find(L";"+ext+L";")!=std::wstring::npos){
        auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool uninitialize=SUCCEEDED(initialized);
        ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICBitmapScaler> scaler;ComPtr<IWICFormatConverter> converter;
        auto hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
        if(SUCCEEDED(hr))hr=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder);
        if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
        UINT width=0,height=0;if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);
        if(!width||!height||(uint64_t)width*height>128ULL*1024*1024)hr=E_INVALIDARG;
        UINT w=width,h=height;if(std::max(w,h)>700){double ratio=700.0/std::max(w,h);w=std::max(1u,(UINT)(w*ratio));h=std::max(1u,(UINT)(h*ratio));}
        if(SUCCEEDED(hr))hr=factory->CreateBitmapScaler(&scaler);
        if(SUCCEEDED(hr))hr=scaler->Initialize(frame.Get(),w,h,WICBitmapInterpolationModeFant);
        if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&converter);
        if(SUCCEEDED(hr))hr=converter->Initialize(scaler.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
        if(SUCCEEDED(hr)&&!cancelled(version,expected)){
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-(LONG)h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
            void* pixels=nullptr;auto bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
            if(bitmap){hr=converter->CopyPixels(nullptr,w*4,w*h*4,(BYTE*)pixels);if(SUCCEEDED(hr)&&!cancelled(version,expected))result.bitmap=bitmap;else DeleteObject(bitmap);}
        }
        // COM references must be released before this worker apartment ends.
        converter.Reset();scaler.Reset();frame.Reset();decoder.Reset();factory.Reset();
        if(uninitialize)CoUninitialize();
    }else{
        const std::wstring textTypes=L";.txt;.md;.csv;.tsv;.log;.json;.xml;.yaml;.yml;.ini;.toml;.cpp;.c;.hpp;.h;.py;.js;.ts;.html;.css;.ps1;.bat;.sql;";
        if(!ext.empty()&&textTypes.find(L";"+ext+L";")!=std::wstring::npos){
            std::ifstream input(path,std::ios::binary);std::vector<char> buffer((size_t)std::min<uint64_t>(bytes,64*1024));
            input.read(buffer.data(),(std::streamsize)buffer.size());buffer.resize((size_t)input.gcount());
            if(!cancelled(version,expected)){result.text+=L"\n\n"+decodeText(buffer);if(bytes>buffer.size())result.text+=L"\n\n[仅预览前 64 KiB]";}
        }
    }
    if(cancelled(version,expected)){if(result.bitmap)DeleteObject(result.bitmap);return {};}
    return result;
}
}
