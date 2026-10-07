// Synthetic-only diagnostic. Does not inspect clipboard history or capture the screen.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include "capture.hpp"
#include "common.hpp"
#include "translation.hpp"
#include "json.hpp"
#include <cstdio>
#include <fstream>

using nlohmann::json;
namespace {
std::string text(const std::filesystem::path& path) { return winrt::to_string(winrt::hstring(path.wstring())); }
std::string code(HRESULT error) { char buffer[11]{}; std::snprintf(buffer,sizeof(buffer),"0x%08X",static_cast<unsigned>(error)); return buffer; }
json winrtImage(const std::filesystem::path& path) {
    using namespace winrt::Windows;
    const char* stage = "storage_file";
    try {
        auto file = Storage::StorageFile::GetFileFromPathAsync(path.wstring()).get();
        stage = "open_stream";
        auto stream = file.OpenAsync(Storage::FileAccessMode::Read).get();
        stage = "decode";
        auto decoder = Graphics::Imaging::BitmapDecoder::CreateAsync(stream).get();
        stage = "software_bitmap";
        auto bitmap = decoder.GetSoftwareBitmapAsync(Graphics::Imaging::BitmapPixelFormat::Bgra8,Graphics::Imaging::BitmapAlphaMode::Ignore).get();
        stage = "create_ocr";
        auto engine = Media::Ocr::OcrEngine::TryCreateFromLanguage(Globalization::Language(L"zh-Hans"));
        if (!engine) engine = Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        stage = "recognize";
        auto result = engine.RecognizeAsync(bitmap).get();
        json report{{"ok",true},{"width",decoder.PixelWidth()},{"height",decoder.PixelHeight()},{"syntheticText",winrt::to_string(result.Text())}};
        bitmap.Close(); stream.Close(); return report;
    } catch (const winrt::hresult_error& error) { return {{"ok",false},{"stage",stage},{"hresult",code(error.code())}}; }
}
json workerImage(const std::filesystem::path& worker, const std::filesystem::path& image) {
    try { auto result = desk::runOcr(worker,image); return {{"ok",true},{"width",result.width},{"height",result.height},{"syntheticText",winrt::to_string(winrt::hstring(result.text))}}; }
    catch (const std::exception& error) { return {{"ok",false},{"error",error.what()}}; }
}
std::filesystem::path finalPath(const std::filesystem::path& logical) {
    HANDLE file = CreateFileW(logical.c_str(),GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    wchar_t buffer[32768]{}; const auto length = GetFinalPathNameByHandleW(file,buffer,32768,FILE_NAME_NORMALIZED);
    CloseHandle(file);
    if (!length || length >= 32768) return {};
    std::wstring result(buffer,length);
    if (result.starts_with(L"\\\\?\\")) result.erase(0,4);
    return result;
}
json probe(const std::filesystem::path& report, const std::filesystem::path& worker) {
    wchar_t package[1024]{}; UINT32 packageLength = 1024;
    if (GetCurrentPackageFullName(&packageLength,package) != ERROR_SUCCESS)
        return {{"ok",false},{"error","probe_requires_genuine_package_identity"}};
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    json result{{"syntheticOnly",true},{"package",winrt::to_string(winrt::hstring(package))}};
    PWSTR localPath{};
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&localPath));
    const auto originalRoot = std::filesystem::path(localPath) / L"DeskFlow"; CoTaskMemFree(localPath);
    localPath = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData,0x40000,nullptr,&localPath));
    const auto filteredRoot = std::filesystem::path(localPath) / L"DeskFlow"; CoTaskMemFree(localPath);
    const auto name = L"ocr-synthetic-path-probe-" + std::to_wstring(GetCurrentProcessId()) + L".png";
    const auto original = originalRoot / name, filtered = filteredRoot / name;
    const auto application = desk::dataDirectory() / (L"ocr-synthetic-roundtrip-" + std::to_wstring(GetCurrentProcessId()) + L".png");
    const auto invalid = application.parent_path() / (L"ocr-synthetic-invalid-" + std::to_wstring(GetCurrentProcessId()) + L".png");
    const auto control = report.parent_path() / (L"ocr-synthetic-control-" + std::to_wstring(GetCurrentProcessId()) + L".png");
    result["logicalRoot"] = text(originalRoot); result["filteredRoot"] = text(filteredRoot);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 900; info.bmiHeader.biHeight = -240;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits{}; HBITMAP bitmap = CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if (!bitmap) throw std::runtime_error("Cannot allocate synthetic diagnostic bitmap.");
    HDC dc = CreateCompatibleDC(nullptr); auto oldBitmap = SelectObject(dc,bitmap);
    RECT bounds{0,0,900,240}; FillRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HFONT font = CreateFontW(-38,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
    auto oldFont = SelectObject(dc,font); SetTextColor(dc,RGB(0,0,0)); SetBkMode(dc,TRANSPARENT);
    RECT english{30,30,880,100}, chinese{30,110,880,200};
    DrawTextW(dc,L"Hello DeskFlow 2026",-1,&english,DT_LEFT | DT_NOPREFIX);
    DrawTextW(dc,L"你好世界",-1,&chinese,DT_LEFT | DT_NOPREFIX); GdiFlush();
    result["controlSave"] = desk::saveBitmapPng(bitmap,control);
    SetLastError(ERROR_SUCCESS); result["logicalSave"] = desk::saveBitmapPng(bitmap,original);
    result["logicalSaveLastErrorAfterCleanup"] = GetLastError();
    SetLastError(ERROR_SUCCESS); result["filteredSave"] = desk::saveBitmapPng(bitmap,filtered);
    result["filteredSaveLastErrorAfterCleanup"] = GetLastError();
    result["applicationRoot"] = text(application.parent_path());
    result["applicationSave"] = desk::saveBitmapPng(bitmap,application);
    SelectObject(dc,oldFont); SelectObject(dc,oldBitmap); DeleteObject(font); DeleteDC(dc); DeleteObject(bitmap);
    // A direct Win32 copy bypasses only the encoder's atomic-rename step, so
    // StorageFile versus Win32 path resolution can be tested independently.
    if (!std::filesystem::is_regular_file(original)) result["logicalDirectCopy"] = CopyFileW(control.c_str(),original.c_str(),TRUE) != FALSE;
    const auto physical = finalPath(original);
    result["logicalWin32Exists"] = std::filesystem::is_regular_file(original);
    result["logicalFinalPath"] = text(physical);
    result["controlWinrt"] = winrtImage(control);
    result["logicalWinrt"] = winrtImage(original);
    result["filteredWinrt"] = winrtImage(filtered);
    if (!physical.empty()) result["physicalWinrt"] = winrtImage(physical);
    result["logicalWorker"] = workerImage(worker,original);
    result["filteredWorker"] = workerImage(worker,filtered);
    result["applicationWorker"] = workerImage(worker,application);
    { std::ofstream corrupted(invalid,std::ios::binary); corrupted << "synthetic invalid PNG"; }
    result["invalidWorker"] = workerImage(worker,invalid);
    const auto recognizedText = result["applicationWorker"].value("syntheticText",std::string{});
    result["chineseJoined"] = recognizedText.find("你好世界") != std::string::npos;
    result["latinSpacePreserved"] = recognizedText.find("DeskFlow 2026") != std::string::npos;
    result["invalidDecodeReported"] = !result["invalidWorker"].value("ok",true) && result["invalidWorker"].value("error",std::string{}).find("解码") != std::string::npos;
    std::error_code ignored; std::filesystem::remove(original,ignored); std::filesystem::remove(filtered,ignored);
    std::filesystem::remove(application,ignored); std::filesystem::remove(invalid,ignored);
    result["ok"] = result["applicationSave"].get<bool>() && result["applicationWorker"].value("ok",false) && result["chineseJoined"].get<bool>() && result["latinSpacePreserved"].get<bool>() && result["invalidDecodeReported"].get<bool>();
    return result;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int argc{}; auto argv = CommandLineToArgvW(GetCommandLineW(),&argc);
    if (argc != 5 || std::wstring_view(argv[1]) != L"--report" || std::wstring_view(argv[3]) != L"--worker") { LocalFree(argv); return 64; }
    const std::filesystem::path report = argv[2], worker = argv[4]; LocalFree(argv);
    json result;
    try { result = probe(report,worker); }
    catch (const winrt::hresult_error& error) { result = {{"ok",false},{"hresult",code(error.code())}}; }
    catch (const std::exception& error) { result = {{"ok",false},{"error",error.what()}}; }
    std::ofstream output(report,std::ios::binary); output << result.dump(2); output.flush();
    return result.value("ok",false) && output.good() ? 0 : 1;
}
