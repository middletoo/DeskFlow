// Runs only on generated text images under genuine DeskFlow package identity.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <appmodel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Ocr.h>
#include "capture.hpp"
#include "translation.hpp"
#include "json.hpp"
#include <chrono>
#include <fstream>
#include <algorithm>

using nlohmann::json;
struct Row { std::wstring english, chinese; int englishY, chineseY; };
std::string utf8(const std::wstring& value) { return winrt::to_string(winrt::hstring(value)); }
std::wstring upper(std::wstring value) { for (auto& ch : value) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A'; return value; }
std::wstring compactMarker(std::wstring value) { value = upper(std::move(value)); value.erase(std::remove_if(value.begin(),value.end(),[](wchar_t ch) { return ch == L' ' || ch == L'\t'; }),value.end()); return value; }
bool saveSynthetic(const std::filesystem::path& path, int width, int height, const std::vector<Row>& rows) {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits{}; HBITMAP bitmap = CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if (!bitmap) return false;
    HDC dc = CreateCompatibleDC(nullptr); auto oldBitmap = SelectObject(dc,bitmap);
    RECT bounds{0,0,width,height}; FillRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HFONT latin = CreateFontW(-32,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HFONT chinese = CreateFontW(-32,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
    auto oldFont = SelectObject(dc,latin); SetTextColor(dc,RGB(0,0,0)); SetBkMode(dc,TRANSPARENT);
    for (const auto& row : rows) {
        RECT englishRect{30,row.englishY,width - 20,row.englishY + 42}; SelectObject(dc,latin);
        DrawTextW(dc,row.english.c_str(),-1,&englishRect,DT_LEFT | DT_NOPREFIX | DT_SINGLELINE);
        RECT chineseRect{30,row.chineseY,width - 20,row.chineseY + 42}; SelectObject(dc,chinese);
        DrawTextW(dc,row.chinese.c_str(),-1,&chineseRect,DT_LEFT | DT_NOPREFIX | DT_SINGLELINE);
    }
    GdiFlush();
    const bool saved = desk::saveBitmapPng(bitmap,path);
    SelectObject(dc,oldFont); SelectObject(dc,oldBitmap); DeleteObject(latin); DeleteObject(chinese); DeleteDC(dc); DeleteObject(bitmap);
    return saved;
}
json verify(const std::filesystem::path& worker, const std::filesystem::path& input, const std::vector<Row>& expected, int width, int height) {
    const auto started = std::chrono::steady_clock::now();
    json result;
    try {
        auto recognized = desk::runOcr(worker,input);
        result["width"] = recognized.width; result["height"] = recognized.height;
        result["text"] = utf8(recognized.text); result["lines"] = json::array();
        for (const auto& line : recognized.lines) result["lines"].push_back({{"text",utf8(line.text)},{"x",line.x},{"y",line.y},{"width",line.width},{"height",line.height}});
        bool correct = recognized.width == width && recognized.height == height;
        result["checks"] = json::array();
        for (const auto& row : expected) {
            const auto label = upper(row.english.substr(0,row.english.find(L' ')));
            int englishCount = 0, chineseCount = 0; bool englishPosition = false, chinesePosition = false, latinSpacing = false;
            for (const auto& line : recognized.lines) {
                // Windows may segment an uppercase marker as "TO P". Match its
                // spelling independently, and verify phrase spaces separately.
                if (compactMarker(line.text).find(label) != std::wstring::npos) { ++englishCount; englishPosition = std::abs(line.y - row.englishY) < 20; latinSpacing = line.text.find(L"DeskFlow 2026") != std::wstring::npos; }
                if (line.text.find(row.chinese) != std::wstring::npos) { ++chineseCount; chinesePosition = std::abs(line.y - row.chineseY) < 20; }
            }
            const bool passed = englishCount == 1 && chineseCount == 1 && englishPosition && chinesePosition && latinSpacing;
            correct &= passed;
            result["checks"].push_back({{"label",utf8(label)},{"englishCount",englishCount},{"chineseCount",chineseCount},{"englishPosition",englishPosition},{"chinesePosition",chinesePosition},{"latinSpacing",latinSpacing},{"ok",passed}});
        }
        result["ok"] = correct;
    } catch (const std::exception& error) { result = {{"ok",false},{"error",error.what()}}; }
    result["milliseconds"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    return result;
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int argc{}; auto argv = CommandLineToArgvW(GetCommandLineW(),&argc);
    if (argc != 5 || std::wstring_view(argv[1]) != L"--worker" || std::wstring_view(argv[3]) != L"--report") { LocalFree(argv); return 64; }
    const std::filesystem::path worker = argv[2], report = argv[4]; LocalFree(argv);
    json result{{"syntheticOnly",true}};
    UINT32 length = 0;
    if (GetCurrentPackageFullName(&length,nullptr) != ERROR_INSUFFICIENT_BUFFER) result["error"] = "Genuine package identity is required.";
    else try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto maximum = winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension();
        result["engineMaxDimension"] = maximum;
        const int seam = static_cast<int>(maximum) - 64;
        std::vector<Row> rows{{L"TOP DeskFlow 2026",L"顶部你好世界",120,170},{L"SEAM DeskFlow 2026",L"接缝你好世界",seam - 45,seam - 3},{L"MIDDLE DeskFlow 2026",L"中部你好世界",15000,15050},{L"BOTTOM DeskFlow 2026",L"底部你好世界",29740,29790}};
        const auto longImage = report.parent_path() / L"synthetic-700x30000.png";
        if (!saveSynthetic(longImage,700,30000,rows)) throw std::runtime_error("Could not save the synthetic long image.");
        result["long"] = verify(worker,longImage,rows,700,30000);
        const std::vector<Row> normal{{L"NORMAL DeskFlow 2026",L"普通你好世界",30,100}};
        const auto normalImage = report.parent_path() / L"synthetic-900x240.png";
        if (!saveSynthetic(normalImage,900,240,normal)) throw std::runtime_error("Could not save the synthetic normal image.");
        result["normal"] = verify(worker,normalImage,normal,900,240);
        const auto blankImage = report.parent_path() / L"synthetic-blank-700x30000.png";
        if (!saveSynthetic(blankImage,700,30000,{})) throw std::runtime_error("Could not save the synthetic blank image.");
        result["blank"] = verify(worker,blankImage,{},700,30000);
        result["blank"]["ok"] = result["blank"].value("ok",false) && result["blank"].value("text",std::string{}).empty() && result["blank"]["lines"].empty();
        result["ok"] = result["long"].value("ok",false) && result["normal"].value("ok",false) && result["blank"].value("ok",false);
    } catch (const std::exception& error) { result["ok"] = false; result["error"] = error.what(); }
    std::ofstream stream(report,std::ios::binary); stream << result.dump(2); stream.flush();
    return result.value("ok",false) && stream.good() ? 0 : 1;
}
