#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
namespace desk::ocr_detail {
struct Word { std::wstring text; float x = 0, width = 0, height = 0; };
struct Tile { uint32_t y = 0, height = 0; double ownerStart = 0, ownerEnd = 0; };
std::vector<Tile> verticalTiles(uint32_t height, uint32_t maximum) {
    if (!height || !maximum) throw std::invalid_argument("OCR tile dimensions must be nonzero.");
    const uint32_t overlap = std::min(128u,maximum / 4);
    const uint32_t step = maximum - overlap;
    std::vector<Tile> result;
    for (uint32_t y = 0; y < height; y += step) {
        const uint32_t count = std::min(maximum,height - y);
        const bool final = y + count == height;
        result.push_back({y,count,y ? y + overlap / 2.0 : 0.0,final ? static_cast<double>(height) : y + count - overlap / 2.0});
        if (result.size() > 64) throw std::invalid_argument("OCR tile count exceeded the task budget.");
        if (final) break;
    }
    return result;
}
namespace {
uint32_t firstPoint(const std::wstring& text) {
    if (text.empty()) return 0;
    const uint32_t first = text.front();
    if (first >= 0xd800 && first <= 0xdbff && text.size() >= 2 && text[1] >= 0xdc00 && text[1] <= 0xdfff)
        return 0x10000 + ((first - 0xd800) << 10) + (text[1] - 0xdc00);
    return first;
}
uint32_t lastPoint(const std::wstring& text) {
    if (text.empty()) return 0;
    const uint32_t last = text.back();
    if (last >= 0xdc00 && last <= 0xdfff && text.size() >= 2 && text[text.size() - 2] >= 0xd800 && text[text.size() - 2] <= 0xdbff)
        return 0x10000 + ((text[text.size() - 2] - 0xd800) << 10) + (last - 0xdc00);
    return last;
}
bool han(uint32_t ch) { return (ch >= 0x3400 && ch <= 0x4dbf) || (ch >= 0x4e00 && ch <= 0x9fff) || (ch >= 0xf900 && ch <= 0xfaff) || (ch >= 0x20000 && ch <= 0x323af); }
bool closing(uint32_t ch) { return ch <= 0xffff && std::wstring_view(L",.!?:;)]}，。！？：；、）》】」』").find(static_cast<wchar_t>(ch)) != std::wstring_view::npos; }
bool opening(uint32_t ch) { return ch <= 0xffff && std::wstring_view(L"([{（《【「『").find(static_cast<wchar_t>(ch)) != std::wstring_view::npos; }
bool chinesePunctuation(uint32_t ch) { return ch >= 0x3000 && ch <= 0xffff && (closing(ch) || opening(ch)); }
}
std::wstring joinWords(const std::vector<Word>& words) {
    std::wstring result;
    const Word* previous = nullptr;
    for (const auto& word : words) {
        if (word.text.empty()) continue;
        if (previous) {
            const uint32_t left = lastPoint(previous->text), right = firstPoint(word.text);
            const float gap = word.x - previous->x - previous->width;
            const float height = std::max(1.0f,std::max(previous->height,word.height));
            const bool smallGap = std::isfinite(gap) && gap <= height * 0.7f;
            const bool attach = smallGap && (((han(left) || chinesePunctuation(left)) && (han(right) || chinesePunctuation(right))) || closing(right) || opening(left));
            if (!attach) result += L' ';
        }
        result += word.text; previous = &word;
    }
    return result;
}
}
#ifndef DESK_OCR_TEXT_TESTING
#include <windows.h>
#include <appmodel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cstdio>

namespace {
using nlohmann::json;
using namespace winrt;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Ocr;

json failure(const char* code) { return json{{"version",1},{"ok",false},{"error",code}}; }
json failure(const char* error, const char* stage, HRESULT value) {
    auto result = failure(error);
    char safeCode[11]{}; std::snprintf(safeCode,sizeof(safeCode),"0x%08X",static_cast<unsigned>(value));
    result["stage"] = stage; result["hresult"] = safeCode; return result;
}
bool writeResult(const std::filesystem::path& destination, const json& result) {
    std::ofstream stream(destination,std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    const auto body = result.dump();
    stream.write(body.data(),static_cast<std::streamsize>(body.size()));
    stream.flush();
    return stream.good();
}
struct ScopedBitmap {
    SoftwareBitmap value{nullptr};
    ~ScopedBitmap() { try { if (value) value.Close(); } catch (...) {} }
};
bool uniformBitmap(const SoftwareBitmap& bitmap) {
    // Only skip exactly constant RGB pixels, so faint text is never discarded
    // by a brightness or sampling threshold. The buffer is borrowed, not copied.
    try {
        auto locked = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
        auto reference = locked.CreateReference();
        const auto plane = locked.GetPlaneDescription(0);
        const auto* data = reference.data();
        bool same = data && plane.StartIndex >= 0 && plane.Stride >= plane.Width * 4 && plane.Width > 0 && plane.Height > 0;
        if (same && static_cast<uint64_t>(plane.StartIndex) + static_cast<uint64_t>(plane.Height - 1) * plane.Stride + static_cast<uint64_t>(plane.Width) * 4 > reference.Capacity()) same = false;
        if (same) {
            const auto* first = data + plane.StartIndex;
            for (int y = 0; same && y < plane.Height; ++y) {
                const auto* row = data + plane.StartIndex + static_cast<ptrdiff_t>(y) * plane.Stride;
                for (int x = 0; x < plane.Width; ++x) {
                    const auto* pixel = row + x * 4;
                    if (pixel[0] != first[0] || pixel[1] != first[1] || pixel[2] != first[2]) { same = false; break; }
                }
            }
        }
        reference.Close(); locked.Close(); return same;
    } catch (const hresult_error&) { return false; }
}
json recognize(const std::filesystem::path& image) {
    const ULONGLONG started = GetTickCount64();
    UINT32 packageLength = 0;
    const auto identity = GetCurrentPackageFullName(&packageLength,nullptr);
    if (identity == APPMODEL_ERROR_NO_PACKAGE) return failure("identity_required");
    if (identity != ERROR_INSUFFICIENT_BUFFER && identity != ERROR_SUCCESS) return failure("identity_required");
    init_apartment(apartment_type::multi_threaded);

    OcrEngine engine{nullptr};
    // The Chinese recognizer also handles Latin words in mixed screenshots.
    // Prefer an installed Chinese model, then an installed English model.
    const auto available = OcrEngine::AvailableRecognizerLanguages();
    for (const auto& entry : available) {
        const auto tag = entry.LanguageTag();
        if (std::wstring_view(tag).starts_with(L"zh")) { engine = OcrEngine::TryCreateFromLanguage(entry); if (engine) break; }
    }
    if (!engine) for (const auto& entry : available) {
        const auto tag = entry.LanguageTag();
        if (std::wstring_view(tag).starts_with(L"en")) { engine = OcrEngine::TryCreateFromLanguage(entry); if (engine) break; }
    }
    if (!engine) return failure("language_missing");

    Windows::Storage::StorageFile file{nullptr};
    Windows::Storage::Streams::IRandomAccessStream stream{nullptr};
    BitmapDecoder decoder{nullptr};
    const char* inputStage = "open_image";
    try {
        file = Windows::Storage::StorageFile::GetFileFromPathAsync(std::filesystem::absolute(image).wstring()).get();
        inputStage = "open_stream";
        stream = file.OpenAsync(Windows::Storage::FileAccessMode::Read).get();
        inputStage = "decode_image";
        decoder = BitmapDecoder::CreateAsync(stream).get();
    } catch (const hresult_error& error) { return failure("image_invalid",inputStage,error.code()); }
    const auto width = decoder.PixelWidth(), height = decoder.PixelHeight();
    if (!width || !height || width > 32768 || height > 32768 || static_cast<uint64_t>(width) * height > 64ULL * 1024 * 1024)
        return failure("image_invalid","image_dimensions",E_INVALIDARG);
    const auto maximum = OcrEngine::MaxImageDimension();
    if (!maximum) return failure("engine_failed","engine_initialization",E_UNEXPECTED);
    // Height never forces small text to shrink. Tall images are cropped in the
    // scaled coordinate system before each engine call.
    const double factor = std::min(1.0,static_cast<double>(maximum) / width);
    const auto scaledWidth = std::max(1u,static_cast<uint32_t>(std::floor(width * factor)));
    const auto scaledHeight = std::max(1u,static_cast<uint32_t>(std::floor(height * factor)));
    // At most 8 Mi pixels / 32 MiB of software-bitmap storage per tile. The
    // parent also enforces its existing 512 MiB job and 30 second deadline.
    const uint32_t tileHeight = std::min(maximum,std::max(1u,(8u * 1024u * 1024u) / scaledWidth));
    const auto tiles = desk::ocr_detail::verticalTiles(scaledHeight,tileHeight);
    const double scaleX = static_cast<double>(width) / scaledWidth;
    const double scaleY = static_cast<double>(height) / scaledHeight;
    struct PositionedLine { std::string text; double x, y, width, height; };
    std::vector<PositionedLine> positioned;
    size_t blankTiles = 0;
    for (const auto& tile : tiles) {
        if (GetTickCount64() - started > 25000) return failure("engine_failed","recognize",HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        BitmapTransform transform;
        transform.ScaledWidth(scaledWidth); transform.ScaledHeight(scaledHeight);
        transform.InterpolationMode(BitmapInterpolationMode::Fant);
        transform.Bounds(BitmapBounds{0,tile.y,scaledWidth,tile.height});
        ScopedBitmap bitmap;
        try {
            bitmap.value = decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8,BitmapAlphaMode::Ignore,
                transform,ExifOrientationMode::IgnoreExifOrientation,ColorManagementMode::DoNotColorManage).get();
        } catch (const hresult_error& error) { return failure("image_invalid","transform_image",error.code()); }
        if (uniformBitmap(bitmap.value)) { ++blankTiles; continue; }
        winrt::Windows::Media::Ocr::OcrResult recognized{nullptr};
        try { recognized = engine.RecognizeAsync(bitmap.value).get(); }
        catch (const hresult_error& error) { return failure("engine_failed","recognize",error.code()); }
        for (const auto& line : recognized.Lines()) {
            double left = std::numeric_limits<double>::max(), top = left, right = 0, bottom = 0;
            std::vector<desk::ocr_detail::Word> words;
            for (const auto& word : line.Words()) {
                const auto box = word.BoundingRect();
                words.push_back({std::wstring(word.Text()),box.X,box.Width,box.Height});
                left = std::min(left,static_cast<double>(box.X)); top = std::min(top,static_cast<double>(box.Y));
                right = std::max(right,static_cast<double>(box.X + box.Width)); bottom = std::max(bottom,static_cast<double>(box.Y + box.Height));
            }
            if (left == std::numeric_limits<double>::max()) continue;
            const double center = tile.y + (top + bottom) / 2;
            if (center < tile.ownerStart || center >= tile.ownerEnd) continue;
            const auto lineText = to_string(winrt::hstring(desk::ocr_detail::joinWords(words)));
            if (lineText.empty()) continue;
            left = std::clamp(left * scaleX,0.0,static_cast<double>(width)); top = std::clamp((tile.y + top) * scaleY,0.0,static_cast<double>(height));
            right = std::clamp(right * scaleX,left,static_cast<double>(width)); bottom = std::clamp((tile.y + bottom) * scaleY,top,static_cast<double>(height));
            if (positioned.size() >= 10000) return failure("output_limit");
            positioned.push_back({lineText,left,top,right - left,bottom - top});
        }
    }
    std::stable_sort(positioned.begin(),positioned.end(),[](const auto& left,const auto& right) { return left.y != right.y ? left.y < right.y : left.x < right.x; });
    json lines = json::array();
    std::string text;
    for (const auto& line : positioned) {
        lines.push_back({{"text",line.text},{"x",line.x},{"y",line.y},{"width",line.width},{"height",line.height}});
        if (!text.empty()) text += '\n'; text += line.text;
    }
    stream.Close();
    return json{{"version",1},{"ok",true},{"width",width},{"height",height},{"text",text},{"lines",std::move(lines)},
        {"language",to_string(engine.RecognizerLanguage().LanguageTag())},{"scaled",factor < 1.0},
        {"tileCount",tiles.size()},{"blankTiles",blankTiles},{"scaledWidth",scaledWidth},{"scaledHeight",scaledHeight}};
}
}
int wmain(int argc, wchar_t** argv) {
    std::filesystem::path input, output;
    if (argc == 5 && std::wstring_view(argv[1]) == L"--input" && std::wstring_view(argv[3]) == L"--output") {
        input = argv[2]; output = argv[4];
    } else {
        std::cerr << "Usage: DeskOCR.exe --input <image.png> --output <result.json>\n";
        return 64;
    }
    json result;
    try { result = recognize(input); }
    catch (const hresult_error& error) { result = failure("engine_failed","engine_initialization",error.code()); }
    catch (const std::exception&) { result = failure("engine_failed"); }
    // Only stable error codes leave this process; filenames and screenshot text
    // are never printed to console logs.
    if (!writeResult(output,result)) { std::cerr << "Cannot write OCR result.\n"; return 3; }
    return result.value("ok",false) ? 0 : 2;
}
#endif
