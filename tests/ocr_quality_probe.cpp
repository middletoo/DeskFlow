// Runs with a temporary validation package, using generated images only.
#include <windows.h>
#include <shellapi.h>
#include <winrt/Windows.Media.Ocr.h>
#include "capture.hpp"
#include "translation.hpp"
#include "json.hpp"
#include <fstream>
#include <algorithm>

using nlohmann::json;
static std::string utf8(const std::wstring& value){return winrt::to_string(winrt::hstring(value));}
static std::wstring compact(std::wstring value){
    value.erase(std::remove_if(value.begin(),value.end(),[](auto character){return iswspace(character)!=0;}),value.end());
    for(auto& character:value)if(character>=L'a'&&character<=L'z')character-=L'a'-L'A';
    return value;
}
struct Sample{const char* name;int width,height,font;COLORREF background,foreground;bool wide=false,pureEnglish=false;};
static bool drawSample(const std::filesystem::path& output,const Sample& sample){
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=sample.width;info.bmiHeader.biHeight=-sample.height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits=nullptr;
    HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    HDC dc=CreateCompatibleDC(nullptr);auto old=SelectObject(dc,bitmap);HBRUSH brush=CreateSolidBrush(sample.background);
    RECT bounds{0,0,sample.width,sample.height};FillRect(dc,&bounds,brush);DeleteObject(brush);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,sample.foreground);
    HFONT font=CreateFontW(-sample.font,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
    auto oldFont=SelectObject(dc,font);
    const std::wstring latin=L"DeskFlow 2026";TextOutW(dc,24,30,latin.c_str(),(int)latin.size());
    const std::wstring chinese=L"你好世界";
    if(!sample.pureEnglish)TextOutW(dc,24,75,chinese.c_str(),(int)chinese.size());
    if(sample.wide){const std::wstring right=L"WIDERIGHT DeskFlow 2026";TextOutW(dc,sample.width-460,30,right.c_str(),(int)right.size());}
    GdiFlush();const bool saved=desk::saveBitmapPng(bitmap,output);
    SelectObject(dc,oldFont);DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return saved;
}
static json recognize(const std::filesystem::path& worker,const std::filesystem::path& image,const Sample& sample){
    json report;
    try{
        const auto began=GetTickCount64();auto result=desk::runOcr(worker,image);
        const auto text=compact(result.text);bool latin=text.find(L"DESKFLOW2026")!=std::wstring::npos;
        bool chinese=sample.pureEnglish||text.find(L"你好世界")!=std::wstring::npos;
        bool right=!sample.wide;bool geometry=true;
        for(const auto& line:result.lines){
            if(compact(line.text).find(L"WIDERIGHT")!=std::wstring::npos)right=line.x>sample.width-500;
            geometry&=line.x>=0&&line.y>=0&&line.x+line.width<=sample.width+1&&line.y+line.height<=sample.height+1;
        }
        report={{"ok",latin&&chinese&&right&&geometry},{"latin",latin},{"chinese",chinese},{"wideRight",right},{"geometry",geometry},
                {"text",utf8(result.text)},{"milliseconds",GetTickCount64()-began}};
        report["lines"]=json::array();for(const auto& line:result.lines)report["lines"].push_back({{"text",utf8(line.text)},{"x",line.x},{"y",line.y},{"width",line.width},{"height",line.height}});
        report["noDuplicateLines"]=result.lines.size()<=(sample.pureEnglish?1u:sample.wide?3u:2u);
    }catch(const std::exception& error){report={{"ok",false},{"error",error.what()}};}
    return report;
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);std::filesystem::path output,folder;
    for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--output"&&i+1<argc)output=argv[++i];else if(std::wstring(argv[i])==L"--folder"&&i+1<argc)folder=argv[++i];}
    LocalFree(argv);if(output.empty()||folder.empty())return 2;
    json report;
    try{
        winrt::init_apartment();std::filesystem::create_directories(folder);
        const auto maximum=winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension();
        const std::vector<Sample> samples{
            {"small",800,160,14,RGB(255,255,255),RGB(0,0,0)},
            {"dark",800,160,16,RGB(30,32,36),RGB(238,238,238)},
            {"faint",800,160,16,RGB(250,250,250),RGB(225,225,225)},
            {"english",800,160,14,RGB(255,255,255),RGB(0,0,0),false,true},
            {"clear",800,180,28,RGB(255,255,255),RGB(0,0,0)},
            {"wide",(int)std::min(12000u,maximum+1200),180,14,RGB(255,255,255),RGB(0,0,0),true}
        };
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);const auto directory=std::filesystem::path(executable).parent_path();
        report["engineMaximum"]=maximum;report["samples"]=json::array();int afterPassed=0;
        for(const auto& sample:samples){
            const auto image=folder/(std::wstring(sample.name,sample.name+strlen(sample.name))+L".png");
            if(!drawSample(image,sample))throw std::runtime_error("Synthetic image creation failed");
            auto before=recognize(directory/L"DeskOCR-before.exe",image,sample),after=recognize(directory/L"DeskOCR-after.exe",image,sample);
            if(after.value("ok",false))++afterPassed;
            report["samples"].push_back({{"name",sample.name},{"before",before},{"after",after}});
        }
        report["afterPassed"]=afterPassed;report["total"]=samples.size();report["syntheticOnly"]=true;
    }catch(const std::exception& error){report["error"]=error.what();}
    std::ofstream file(output);file<<report.dump(2);return 0;
}
