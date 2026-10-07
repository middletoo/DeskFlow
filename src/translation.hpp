#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace desk {
struct OcrLine {
    std::wstring text;
    float x = 0, y = 0, width = 0, height = 0;
};
struct OcrResult {
    std::wstring text;
    std::vector<OcrLine> lines;
    int width = 0, height = 0;
};
struct TranslationConfig {
    std::wstring provider = L"google-free", target = L"zh-CN";
    std::wstring deeplKey, googleKey, libreUrl, proxy;
    bool deeplFree = true;
};
struct DeepLUsage { int64_t used = 0, limit = 0; };
DeepLUsage queryDeepLUsage(const TranslationConfig& config, std::atomic_bool* cancel = nullptr);
OcrResult runOcr(const std::filesystem::path& workerExe, const std::filesystem::path& image,
                 const std::atomic_bool* cancel = nullptr);
std::vector<std::wstring> translateLines(const std::vector<std::wstring>& lines,
    const TranslationConfig& config, std::atomic_bool* cancel = nullptr);
// The caller owns the returned bitmap; the source bitmap is never modified.
HBITMAP renderTranslation(HBITMAP original, const OcrResult& ocr,
    const std::vector<std::wstring>& translated);
// Runs OCR, provider translation and rendering on a background caller thread.
// Source and returned bitmap storage are reserved by the caller; transient PNG
// encoding memory is reserved internally. The source is borrowed and unchanged;
// the caller owns the returned bitmap and must release it with DeleteObject.
// tempDirectory must be a physical writable path (including in an MSIX app).
HBITMAP translateImage(HBITMAP borrowedSource, const std::filesystem::path& workerExe,
    const TranslationConfig& config, std::atomic_bool& cancel,
    const std::filesystem::path& tempDirectory);

namespace translation_detail {
struct Request {
    std::wstring url, headers, method = L"POST";
    std::string body;
};
Request buildRequest(const std::vector<std::wstring>& lines, const TranslationConfig& config);
Request buildUsageRequest(const TranslationConfig& config);
DeepLUsage parseUsageResponse(const std::string& body);
std::string executeRequest(const Request& request, const std::wstring& proxy = {}, std::atomic_bool* cancel = nullptr);
std::vector<std::wstring> parseResponse(const std::wstring& provider,
    const std::string& body, size_t expectedLines);
OcrResult parseOcrJson(const std::string& body);
std::string statusError(unsigned status);
}
}
