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
struct ImageTile {uint32_t x{},y{},width{},height{};double left{},top{},right{},bottom{};};
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
std::vector<ImageTile> imageTiles(uint32_t width,uint32_t height,uint32_t maximum){
    if(!width||!height||!maximum)throw std::invalid_argument("OCR image dimensions must be nonzero");
    const auto practicalMaximum=std::min(maximum,2400u);
    const auto tileWidth=std::min(width,practicalMaximum);
    const auto tileHeight=std::min(practicalMaximum,std::max(1u,(8u*1024u*1024u)/tileWidth));
    const auto columns=verticalTiles(width,tileWidth),rows=verticalTiles(height,tileHeight);
    if(columns.size()*rows.size()>64)throw std::invalid_argument("OCR tile count exceeded the task budget");
    std::vector<ImageTile> result;
    for(const auto& row:rows)for(const auto& column:columns)
        result.push_back({column.y,row.y,column.height,row.height,column.ownerStart,row.ownerStart,column.ownerEnd,row.ownerEnd});
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
    // Preserve wide-image detail and bound enlargement of small screenshots.
    const auto area=static_cast<uint64_t>(width)*height;
    struct PositionedWord {std::wstring text;double x,y,width,height;};
    std::vector<PositionedWord> positioned;
    size_t blankTiles=0,normalizedTiles=0,englishTiles=0;
    OcrEngine english{nullptr};
    auto normalize=[](const SoftwareBitmap& bitmap,bool enhance=true){
        auto locked=bitmap.LockBuffer(BitmapBufferAccessMode::ReadWrite);
        auto reference=locked.CreateReference();const auto plane=locked.GetPlaneDescription(0);
        auto* data=reference.data();
        if(!data||plane.StartIndex<0||plane.Stride<plane.Width*4||plane.Width<=0||plane.Height<=0||
           static_cast<uint64_t>(plane.StartIndex)+static_cast<uint64_t>(plane.Height-1)*plane.Stride+static_cast<uint64_t>(plane.Width)*4>reference.Capacity()){
            reference.Close();locked.Close();return false;
        }
        uint32_t low=255,high=0;uint64_t dark=0,pixels=static_cast<uint64_t>(plane.Width)*plane.Height;bool alpha=false;
        for(int y=0;y<plane.Height;++y)for(int x=0;x<plane.Width;++x){
            auto* pixel=data+plane.StartIndex+static_cast<ptrdiff_t>(y)*plane.Stride+x*4;
            if(pixel[3]!=255){
                const auto extra=255-pixel[3];alpha=true;
                for(int channel=0;channel<3;++channel)pixel[channel]=static_cast<uint8_t>(std::min(255u,static_cast<unsigned>(pixel[channel])+extra));
                pixel[3]=255;
            }
            const unsigned light=(pixel[2]*77+pixel[1]*150+pixel[0]*29)>>8;
            low=std::min(low,light);high=std::max(high,light);if(light<96)++dark;
        }
        const bool invert=enhance&&dark*100>pixels*65;
        const bool stretch=enhance&&high>low+2&&high-low<80;
        if(invert||stretch)for(int y=0;y<plane.Height;++y)for(int x=0;x<plane.Width;++x){
            auto* pixel=data+plane.StartIndex+static_cast<ptrdiff_t>(y)*plane.Stride+x*4;
            int light=(pixel[2]*77+pixel[1]*150+pixel[0]*29)>>8;
            if(stretch)light=std::clamp((light-static_cast<int>(low))*255/static_cast<int>(high-low),0,255);
            if(invert)light=255-light;
            pixel[0]=pixel[1]=pixel[2]=static_cast<uint8_t>(light);pixel[3]=255;
        }
        reference.Close();locked.Close();return alpha||invert||stretch;
    };
    Windows::Media::Ocr::OcrResult original{nullptr};
    std::vector<float> nativeHeights;
    if(area<=2ULL*1024*1024&&width<=maximum&&height<=maximum){
        try{
            BitmapTransform nativeTransform;ScopedBitmap native;
            native.value=decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8,BitmapAlphaMode::Premultiplied,
                nativeTransform,ExifOrientationMode::IgnoreExifOrientation,ColorManagementMode::DoNotColorManage).get();
            normalize(native.value,false);
            if(!uniformBitmap(native.value)){
                original=engine.RecognizeAsync(native.value).get();
                for(const auto& line:original.Lines())for(const auto& word:line.Words())nativeHeights.push_back(word.BoundingRect().Height);
            }
        }catch(const hresult_error&){}
    }
    std::sort(nativeHeights.begin(),nativeHeights.end());
    const bool readableSize=!nativeHeights.empty()&&nativeHeights[nativeHeights.size()/2]>=20;
    const double factor=readableSize?1.0:area<=512ULL*1024?4.0:area<=2ULL*1024*1024?2.0:1.0;
    const auto scaledWidth=static_cast<uint32_t>(width*factor),scaledHeight=static_cast<uint32_t>(height*factor);
    const auto tiles=desk::ocr_detail::imageTiles(scaledWidth,scaledHeight,maximum);
    const double scaleX=static_cast<double>(width)/scaledWidth,scaleY=static_cast<double>(height)/scaledHeight;
    for(const auto& tile:tiles){
        if(GetTickCount64()-started>25000)return failure("engine_failed","recognize",HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        BitmapTransform transform;transform.ScaledWidth(scaledWidth);transform.ScaledHeight(scaledHeight);
        transform.InterpolationMode(BitmapInterpolationMode::Cubic);transform.Bounds(BitmapBounds{tile.x,tile.y,tile.width,tile.height});
        ScopedBitmap bitmap;
        try{
            bitmap.value=decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8,BitmapAlphaMode::Premultiplied,
                transform,ExifOrientationMode::IgnoreExifOrientation,ColorManagementMode::DoNotColorManage).get();
            if(normalize(bitmap.value))++normalizedTiles;
        }catch(const hresult_error& error){return failure("image_invalid","transform_image",error.code());}
        if(uniformBitmap(bitmap.value)){++blankTiles;continue;}
        winrt::Windows::Media::Ocr::OcrResult recognized{nullptr};
        try{recognized=engine.RecognizeAsync(bitmap.value).get();}
        catch(const hresult_error& error){return failure("engine_failed","recognize",error.code());}
        bool hasHan=false,hasLatin=false;
        for(const auto& line:recognized.Lines())for(const auto& word:line.Words()){
            const auto wordText=word.Text();
            for(auto character:std::wstring_view(wordText)){
                if((character>=0x3400&&character<=0x9fff)||(character>=0xf900&&character<=0xfaff))hasHan=true;
                if((character>=L'a'&&character<=L'z')||(character>=L'A'&&character<=L'Z'))hasLatin=true;
            }
        }
        if(!hasHan&&(hasLatin||recognized.Lines().Size()==0)&&std::wstring_view(engine.RecognizerLanguage().LanguageTag()).starts_with(L"zh")){
            if(!english)for(const auto& language:available)if(std::wstring_view(language.LanguageTag()).starts_with(L"en")){
                english=OcrEngine::TryCreateFromLanguage(language);if(english)break;
            }
            if(english&&GetTickCount64()-started<22000){
                try{
                    auto alternative=english.RecognizeAsync(bitmap.value).get();
                    if(alternative.Lines().Size()){recognized=alternative;++englishTiles;}
                }catch(const hresult_error&){}
            }
        }
        for(const auto& line:recognized.Lines())for(const auto& word:line.Words()){
            const auto box=word.BoundingRect();const double centerX=tile.x+box.X+box.Width/2,centerY=tile.y+box.Y+box.Height/2;
            if(centerX<tile.left||centerX>=tile.right||centerY<tile.top||centerY>=tile.bottom)continue;
            const auto text=std::wstring(word.Text());if(text.empty())continue;
            const double x=std::clamp((tile.x+box.X)*scaleX,0.0,static_cast<double>(width));
            const double y=std::clamp((tile.y+box.Y)*scaleY,0.0,static_cast<double>(height));
            if(positioned.size()>=50000)return failure("output_limit");
            positioned.push_back({text,x,y,std::min(box.Width*scaleX,width-x),std::min(box.Height*scaleY,height-y)});
        }
    }
    // Keep native CJK results when enlargement loses characters. Match the
    // same visual line before replacing it; never append a second copy.
    if(original && factor>1.0 && GetTickCount64()-started<20000){
      try {
        auto hanCount=[](const auto& words){size_t count=0;for(const auto& word:words)for(auto character:word.text)
            if((character>=0x3400&&character<=0x9fff)||(character>=0xf900&&character<=0xfaff))++count;return count;};
        for(const auto& line:original.Lines()){
            std::vector<PositionedWord> candidate;double left=width,top=height,right=0,bottom=0;
            for(const auto& word:line.Words()){
                const auto box=word.BoundingRect();candidate.push_back({std::wstring(word.Text()),box.X,box.Y,box.Width,box.Height});
                left=std::min(left,(double)box.X);top=std::min(top,(double)box.Y);right=std::max(right,(double)(box.X+box.Width));bottom=std::max(bottom,(double)(box.Y+box.Height));
            }
            if(!hanCount(candidate))continue;
            const double middle=(top+bottom)/2;
            std::vector<PositionedWord> existing;
            auto matches=[&](const auto& word){return std::abs(word.y+word.height/2-middle)<std::max(word.height,bottom-top)*1.5&&word.x<right+4&&word.x+word.width>left-4;};
            for(const auto& word:positioned)if(matches(word))existing.push_back(word);
            if(!existing.empty()&&hanCount(existing)>0&&hanCount(candidate)>=hanCount(existing)){
                positioned.erase(std::remove_if(positioned.begin(),positioned.end(),matches),positioned.end());
                positioned.insert(positioned.end(),candidate.begin(),candidate.end());
            }
        }
      }catch(const hresult_error&){} // An optional refinement never discards the base result.
    }
    std::stable_sort(positioned.begin(),positioned.end(),[](const auto& a,const auto& b){
        const auto ac=a.y+a.height/2,bc=b.y+b.height/2;return ac!=bc?ac<bc:a.x<b.x;
    });
    struct Band {std::vector<PositionedWord> words;double center{},height{};};
    std::vector<Band> bands;
    for(auto& word:positioned){
        const double center=word.y+word.height/2;
        if(bands.empty()||std::abs(center-bands.back().center)>std::max(word.height,bands.back().height)*.55)
            bands.push_back({{},center,word.height});
        auto& band=bands.back();band.height=std::max(band.height,word.height);
        band.words.push_back(std::move(word));
    }
    struct PositionedLine {std::string text;double x,y,width,height;};
    std::vector<PositionedLine> output;
    for(auto& band:bands){
        std::stable_sort(band.words.begin(),band.words.end(),[](const auto& a,const auto& b){return a.x<b.x;});
        std::vector<desk::ocr_detail::Word> words;double left=0,top=0,right=0,bottom=0;
        auto flush=[&]{
            if(words.empty())return;
            output.push_back({to_string(winrt::hstring(desk::ocr_detail::joinWords(words))),left,top,right-left,bottom-top});words.clear();
        };
        for(const auto& word:band.words){
            if(!words.empty()&&word.x-right>std::max(word.height,bottom-top)*2.4)flush();
            if(words.empty()){left=word.x;top=word.y;right=word.x+word.width;bottom=word.y+word.height;}
            else{top=std::min(top,word.y);right=std::max(right,word.x+word.width);bottom=std::max(bottom,word.y+word.height);}
            words.push_back({word.text,(float)word.x,(float)word.width,(float)word.height});
        }
        flush();
    }
    std::stable_sort(output.begin(),output.end(),[](const auto& a,const auto& b){return a.y!=b.y?a.y<b.y:a.x<b.x;});
    json lines=json::array();std::string text;
    for(const auto& line:output){
        if(lines.size()>=10000)return failure("output_limit");
        if(line.text.empty())continue;
        lines.push_back({{"text",line.text},{"x",line.x},{"y",line.y},{"width",line.width},{"height",line.height}});
        if(!text.empty())text+='\n';text+=line.text;
    }
    stream.Close();
    return json{{"version",1},{"ok",true},{"width",width},{"height",height},{"text",text},{"lines",std::move(lines)},
        {"language",to_string(engine.RecognizerLanguage().LanguageTag())},{"scaled",factor!=1.0},{"upscaled",factor>1.0},
        {"tileCount",tiles.size()},{"blankTiles",blankTiles},{"normalizedTiles",normalizedTiles},{"englishTiles",englishTiles},
        {"scaledWidth",scaledWidth},{"scaledHeight",scaledHeight}};
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
