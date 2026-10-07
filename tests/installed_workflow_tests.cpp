#include "common.hpp"
#include "clipboard.hpp"
#include "capture.hpp"
#include "translation.hpp"
#include "image_tools.hpp"
#include "json.hpp"
#include <shellapi.h>
#include <appmodel.h>
#include <objbase.h>
#include <cstring>
#include <fstream>
#include <memory>
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int count=0;auto arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    std::filesystem::path report,worker,backup,backupSource;bool translate=false;
    for(int i=1;i<count;i++){
        if(std::wstring(arguments[i])==L"--translate")translate=true;
        else if(i+1<count&&std::wstring(arguments[i])==L"--report")report=arguments[++i];
        else if(i+1<count&&std::wstring(arguments[i])==L"--worker")worker=arguments[++i];
        else if(i+1<count&&std::wstring(arguments[i])==L"--backup")backup=arguments[++i];
        else if(i+1<count&&std::wstring(arguments[i])==L"--backup-source")backupSource=arguments[++i];
    }
    LocalFree(arguments);if(report.empty()||(worker.empty()&&backup.empty()))return 2;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    nlohmann::json result;int code=0;
    if(!backup.empty()) {
        try {desk::HistoryStore store(backupSource.empty()?desk::dataDirectory():backupSource);store.backup(backup);result={{"ok",true},{"records",store.count()},{"backup",desk::utf8(backup.wstring())}};}
        catch(const std::exception& e){result={{"ok",false},{"error",desk::utf8(desk::userError(e))}};code=1;}
        std::ofstream output(report,std::ios::binary);output<<result.dump(2,' ',false,nlohmann::json::error_handler_t::replace);output.close();CoUninitialize();return code;
    }
    auto directory=desk::dataDirectory()/(L"synthetic-workflow-"+std::to_wstring(GetCurrentProcessId()));
    result["directory"]=desk::utf8(directory.wstring());
    try {
        UINT32 length=0;require(GetCurrentPackageFullName(&length,nullptr)==ERROR_INSUFFICIENT_BUFFER,"test requires actual installed package identity");
        std::filesystem::create_directories(directory);
        std::wstring value=L"Synthetic history roundtrip 你好世界";
        desk::ClipPayload payload;desk::ClipFormat format;format.format=CF_UNICODETEXT;
        format.data.resize((value.size()+1)*2);memcpy(format.data.data(),value.c_str(),format.data.size());payload.formats.push_back(format);
        {
            desk::HistoryStore history(directory);auto id=history.append(payload);
            require(history.load(id).formats.front().data==format.data,"packaged history bytes must round-trip");
            history.backup(directory/L"backup");
            desk::HistoryStore backup(directory/L"backup");require(backup.count()==1,"packaged backup must include committed attachment");
        }
        result["historyRoundTrip"]=true;result["historyBackup"]=true;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=900;info.bmiHeader.biHeight=-220;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        require(bitmap!=nullptr,"synthetic bitmap creation");
        auto owned=std::shared_ptr<std::remove_pointer_t<HBITMAP>>(bitmap,[](HBITMAP b){if(b)DeleteObject(b);});
        HDC dc=CreateCompatibleDC(nullptr);auto old=SelectObject(dc,bitmap);RECT area{0,0,900,220};FillRect(dc,&area,(HBRUSH)GetStockObject(WHITE_BRUSH));
        auto font=CreateFontW(-42,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
        auto previous=SelectObject(dc,font);SetTextColor(dc,RGB(0,0,0));SetBkMode(dc,TRANSPARENT);
        std::wstring english=L"Hello DeskFlow 2026",chinese=L"你好世界";
        TextOutW(dc,24,24,english.c_str(),(int)english.size());
        SelectObject(dc,previous);DeleteObject(font);
        font=CreateFontW(-38,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
        previous=SelectObject(dc,font);RECT chineseArea{30,110,880,210};
        DrawTextW(dc,chinese.c_str(),(int)chinese.size(),&chineseArea,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(dc,previous);DeleteObject(font);SelectObject(dc,old);DeleteDC(dc);
        auto png=directory/L"capture.png";bool saved=desk::saveBitmapPng(bitmap,png);
        require(saved,"GUI-equivalent PNG atomic commit must succeed");result["capturePngSaved"]=true;
        auto recognized=desk::runOcr(worker,png);
        result["ocrText"]=desk::utf8(recognized.text);
        require(recognized.text.find(L"DeskFlow")!=std::wstring::npos&&recognized.text.find(chinese)!=std::wstring::npos,"OCR must read GUI-equivalent saved PNG and preserve Chinese joining");
        if(translate){
            std::atomic_bool cancel{false};desk::TranslationConfig configuration;
            auto translatedText=desk::translateLines({L"Hello"},configuration,&cancel);
            require(translatedText.size()==1&&translatedText.front()!=L"Hello","live engine must actually translate the synthetic English sample");
            auto reservation=desk::image_tools_detail::reserveMemory(900ULL*220*4*2);
            require((bool)reservation,"synthetic translation bitmap reservation");
            auto translated=desk::translateImage(bitmap,worker,configuration,cancel,directory);
            auto translatedOwner=std::shared_ptr<std::remove_pointer_t<HBITMAP>>(translated,[](HBITMAP b){if(b)DeleteObject(b);});
            require(translated!=nullptr,"installed OCR and real provider must produce translated image");
            auto translatedDc=CreateCompatibleDC(nullptr);auto selected=SelectObject(translatedDc,translated);
            require(GetPixel(translatedDc,890,210)==RGB(255,255,255),"translation must preserve pixels outside text regions");
            SelectObject(translatedDc,selected);DeleteDC(translatedDc);
            auto translatedPath=report.parent_path()/(report.stem().wstring()+L"-translated.png");
            require(desk::saveBitmapPng(translated,translatedPath),"translated synthetic image export");
            result["liveTranslation"]=true;result["translatedGreeting"]=desk::utf8(translatedText.front());
            result["translatedImage"]=desk::utf8(translatedPath.wstring());
        }
        result["ocrText"]=desk::utf8(recognized.text);result["ok"]=true;
    }catch(const std::exception& e){result["ok"]=false;result["error"]=desk::utf8(desk::userError(e));code=1;}
    // This directory was created uniquely for synthetic data; never touch live files.
    std::error_code error;if(code==0)std::filesystem::remove_all(directory,error);
    std::ofstream output(report,std::ios::binary);output<<result.dump(2,' ',false,nlohmann::json::error_handler_t::replace);output.close();
    CoUninitialize();return code;
}
