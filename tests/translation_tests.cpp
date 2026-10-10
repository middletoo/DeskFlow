#include <winsock2.h>
#include "translation.hpp"
#include "image_tools.hpp"
#include "json.hpp"
#include <fstream>
#include <functional>
#include <future>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>
#define DESK_OCR_TEXT_TESTING
#include "../src/ocr_worker.cpp"
#undef DESK_OCR_TEXT_TESTING

using namespace desk;
using nlohmann::json;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F work, const char* message) {
    bool rejected = false;
    try { work(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
void parsing() {
    const auto free = translation_detail::parseResponse(L"google-free",
        R"([[["Hello ","你好",null,null],["world","世界",null,null]],null,"zh-CN"])", 1);
    require(free == std::vector<std::wstring>{L"Hello world"}, "Google free must join segments of one OCR line");
    const auto google = translation_detail::parseResponse(L"google",
        R"({"data":{"translations":[{"translatedText":"A &amp; B"},{"translatedText":"two"}]}})", 2);
    require(google == std::vector<std::wstring>{L"A & B",L"two"}, "Google official must decode escaped text and retain line order");
    const auto deepl = translation_detail::parseResponse(L"deepl",
        R"({"translations":[{"text":"one"},{"text":"two"}]})", 2);
    require(deepl == std::vector<std::wstring>{L"one",L"two"}, "DeepL response order");
    const auto libre = translation_detail::parseResponse(L"libre",
        R"({"translatedText":["one","two"]})", 2);
    require(libre.size() == 2 && libre[1] == L"two", "Libre array response");
    rejects([] { translation_detail::parseResponse(L"deepl", R"({"translations":[]})", 2); }, "Missing translated lines must fail");
    rejects([] { translation_detail::parseResponse(L"google-free", "not json", 1); }, "Malformed JSON must fail");
    rejects([] { translation_detail::parseResponse(L"google-free", R"({"error":"denied"})", 1); }, "Error response cannot become empty success");
    rejects([] { translation_detail::parseResponse(L"libre", R"({"translatedText":["   \t"]})", 1); }, "Whitespace-only provider text cannot become OCR-only success");
    require(translation_detail::statusError(429).find("rate") != std::string::npos, "Rate-limit error must be distinct");
    require(translation_detail::statusError(456).find("quota") != std::string::npos, "Quota error must be distinct");
}
void requests() {
    TranslationConfig config;
    config.provider = L"deepl"; config.deeplKey = L"test:fx";
    auto request = translation_detail::buildRequest({L"first",L"second"}, config);
    require(request.url == L"https://api-free.deepl.com/v2/translate", "DeepL free URL");
    require(request.headers.find(L"Authorization: DeepL-Auth-Key test:fx\r\n") != std::wstring::npos, "DeepL auth header");
    require(request.body.find("test:fx") == std::string::npos, "DeepL key must not enter text body");
    auto body = json::parse(request.body);
    require(body["text"].size() == 2 && body["target_lang"] == "ZH", "DeepL target mapping and line array");
    config.deeplFree = false; config.deeplKey = L"pro-key";
    require(translation_detail::buildRequest({L"first"}, config).url == L"https://api.deepl.com/v2/translate", "DeepL pro URL");
    config.googleKey = L"test-key"; config.provider = L"google";
    request = translation_detail::buildRequest({L"first",L"second"}, config);
    require(request.headers.find(L"X-Goog-Api-Key: test-key") != std::wstring::npos, "Google key must be a header");
    require(json::parse(request.body)["q"].size() == 2, "Google text array");
    config.provider = L"libre"; config.libreUrl = L"http://127.0.0.1:5000/";
    require(translation_detail::buildRequest({L"first"}, config).url == L"http://127.0.0.1:5000/translate", "Libre endpoint joining");
    config.provider = L"google-free";
    request = translation_detail::buildRequest({L"A & B"}, config);
    require(request.method == L"GET" && request.url.find(L"q=A%20%26%20B") != std::wstring::npos, "Experimental endpoint uses an encoded GET request");
    rejects([&] { translation_detail::buildRequest({L"a",L"b"},config); }, "Free adapter must retain one-line boundaries");
    config.provider = L"deepl"; config.deeplKey.clear();
    rejects([&] { translation_detail::buildRequest({L"a"},config); }, "Missing key");
    config.deeplKey = L"x\r\nX-Extra: injected";
    rejects([&] { translation_detail::buildRequest({L"a"},config); }, "Header injection");
    config.provider = L"unknown";
    rejects([&] { translation_detail::buildRequest({L"a"},config); }, "Unknown provider cannot fall back");
    std::atomic_bool cancel{true};
    rejects([&] { translateLines({L"a"},config,&cancel); }, "Pre-cancelled request must not send");
}
void usageParsing() {
    TranslationConfig config; config.provider = L"deepl"; config.deeplKey = L"synthetic-key:fx";
    const auto request = translation_detail::buildUsageRequest(config);
    require(request.method == L"GET" && request.url == L"https://api-free.deepl.com/v2/usage","DeepL usage must use the selected free endpoint");
    require(request.body.empty() && request.headers.find(L"DeepL-Auth-Key synthetic-key:fx") != std::wstring::npos,"Usage request has auth header and no text payload");
    const auto usage = translation_detail::parseUsageResponse(R"({"character_count":4200,"character_limit":500000})");
    require(usage.used == 4200 && usage.limit == 500000,"Usage parses exact billed character counts");
    config.deeplFree = false;
    require(translation_detail::buildUsageRequest(config).url == L"https://api.deepl.com/v2/usage","Usage pro endpoint requires explicit selection");
    rejects([] { translation_detail::parseUsageResponse(R"({"character_count":-1,"character_limit":500000})"); },"Negative usage is invalid");
    rejects([] { translation_detail::parseUsageResponse(R"({"character_count":5.5,"character_limit":500000})"); },"Fractional usage is invalid");
    rejects([] { translation_detail::parseUsageResponse(R"({"character_count":18446744073709551615,"character_limit":500000})"); },"Overflowing usage is invalid");
    rejects([] { translation_detail::parseUsageResponse(R"({"error":"denied"})"); },"Usage errors must not appear as zero quota");
    config.provider = L"google-free";
    rejects([&] { translation_detail::buildUsageRequest(config); },"Usage cannot fall back to a different provider");
    std::atomic_bool cancel{true};
    rejects([&] { queryDeepLUsage(config,&cancel); },"Pre-cancelled usage cannot send");
}
void ocrParsing() {
    auto result = translation_detail::parseOcrJson(R"({"version":1,"ok":true,"width":400,"height":200,"text":"one","lines":[{"text":"one","x":10,"y":20,"width":80,"height":15}]})");
    require(result.width == 400 && result.lines.size() == 1 && result.lines[0].x == 10, "OCR preserves pixel bounds");
    rejects([] { translation_detail::parseOcrJson(R"({"version":1,"ok":false,"error":"identity_required"})"); }, "Identity failure must propagate");
    rejects([] { translation_detail::parseOcrJson(R"({"version":2,"ok":true,"width":1,"height":1,"lines":[]})"); }, "Unknown OCR protocol");
    rejects([] { translation_detail::parseOcrJson(R"({"version":1,"ok":true,"width":400,"height":200,"lines":[{"text":"a","x":-2,"y":0,"width":5,"height":5}]})"); }, "Invalid OCR bounds");
    for (const auto* malformed : {
        R"(["private-fixture-payload"])",
        R"({"version":"private-fixture-payload","ok":true})",
        R"({"version":1,"ok":"private-fixture-payload"})",
        R"({"version":1,"ok":false,"error":{"private-fixture-payload":true}})",
        R"({"version":1,"ok":false,"error":"image_invalid","stage":{"private-fixture-payload":true}})"}) {
        bool safe = false;
        try { translation_detail::parseOcrJson(malformed); }
        catch (const std::exception& error) { safe = std::string(error.what()).find("private-fixture-payload") == std::string::npos; }
        require(safe,"Malformed OCR metadata must produce a safe protocol error without raw worker content");
    }
    rejects([] { runOcr(L"Z:/missing/DeskOCR.exe",L"Z:/missing/image.png"); }, "Missing worker must fail in host");
    bool inputStage = false;
    try { translation_detail::parseOcrJson(R"({"version":1,"ok":false,"error":"image_invalid","stage":"open_image","hresult":"0x80070002"})"); }
    catch (const std::exception& error) { const std::string message = error.what(); inputStage = message.find("无法打开") != std::string::npos && message.find("0x80070002") != std::string::npos; }
    require(inputStage,"OCR input-open failure must retain its safe Windows error code and stage");
    bool decodeStage = false;
    try { translation_detail::parseOcrJson(R"({"version":1,"ok":false,"error":"image_invalid","stage":"decode_image","hresult":"0x88982F50"})"); }
    catch (const std::exception& error) { const std::string message = error.what(); decodeStage = message.find("解码") != std::string::npos; }
    require(decodeStage,"OCR decode failure must be distinct from input-open failure");
    bool safeMetadata = false;
    try { translation_detail::parseOcrJson(R"({"version":1,"ok":false,"error":"image_invalid","stage":"open_image","hresult":"private-file-or-text"})"); }
    catch (const std::exception& error) { safeMetadata = std::string(error.what()).find("private-file-or-text") == std::string::npos; }
    require(safeMetadata,"Worker diagnostic fields must not leak arbitrary strings");
    wchar_t executable[32768]{};
    require(GetModuleFileNameW(nullptr,executable,32768) != 0,"Child fixture executable path");
    const auto image = std::filesystem::temp_directory_path() / (L"desk-ocr-test-" + std::to_wstring(GetCurrentProcessId()) + L".png");
    { std::ofstream fixture(image,std::ios::binary); fixture << "synthetic"; }
    bool languageError = false;
    try { runOcr(executable,image); }
    catch (const std::exception& error) { languageError = std::string(error.what()).find("language resources") != std::string::npos; }
    std::filesystem::remove(image);
    require(languageError,"A failing child worker must propagate its structured language error");
}
void ocrWordJoining() {
    using desk::ocr_detail::Word;
    require(desk::ocr_detail::joinWords({Word{L"你",0,20,30},Word{L"好",23,20,30},Word{L"世",46,20,30},Word{L"界",69,20,30}}) == L"你好世界","Adjacent Chinese OCR words must join without artificial spaces");
    require(desk::ocr_detail::joinWords({Word{L"Hello",0,80,30},Word{L"DeskFlow",90,130,30},Word{L"2026",230,80,30}}) == L"Hello DeskFlow 2026","Latin word spaces must remain");
    require(desk::ocr_detail::joinWords({Word{L"中文",0,45,30},Word{L"English",52,100,30}}) == L"中文 English","Mixed scripts must not merge Latin words into Chinese");
    require(desk::ocr_detail::joinWords({Word{L"你好",0,45,30},Word{L"世界",110,45,30}}) == L"你好 世界","Wide Chinese gaps must preserve separate visual groups");
    require(desk::ocr_detail::joinWords({Word{L"你好",0,45,30},Word{L"，",49,20,30},Word{L"世界",73,45,30},Word{L"！",122,20,30}}) == L"你好，世界！","Chinese punctuation must stay attached");
}
void ocrTileOwnership() {
    const auto tiles = desk::ocr_detail::verticalTiles(30000,10000);
    require(tiles.size() == 4,"Long OCR image must produce bounded overlapping tiles");
    require(tiles.front().y == 0 && tiles.back().y + tiles.back().height == 30000,"Tile bounds must cover the complete image");
    for (size_t i = 0; i < tiles.size(); ++i) {
        require(tiles[i].height <= 10000 && tiles[i].ownerStart >= tiles[i].y && tiles[i].ownerEnd <= tiles[i].y + tiles[i].height,"Ownership must remain inside engine-sized crops");
        if (i) require(tiles[i - 1].ownerEnd == tiles[i].ownerStart,"Adjacent ownership ranges must meet without gaps");
    }
    for (double center : {0.5,9935.99,9936.0,15000.0,19807.99,19808.0,29999.5}) {
        int owners = 0; for (const auto& tile : tiles) if (center >= tile.ownerStart && center < tile.ownerEnd) ++owners;
        require(owners == 1,"Every boundary line center must have exactly one owner");
    }
    require(desk::ocr_detail::verticalTiles(240,10000).size() == 1,"Normal image remains one OCR request");
    const auto grid=desk::ocr_detail::imageTiles(12000,2800,2600);
    require(grid.size()>1&&grid.size()<=64,"Wide OCR images must use bounded two-dimensional tiles");
    for(const auto& tile:grid)require(tile.width<=2600&&tile.height<=2600&&
        static_cast<uint64_t>(tile.width)*tile.height<=8ULL*1024*1024,"OCR tiles must obey engine and pixel budgets");
    for(double x:{0.5,2535.99,2536.0,11999.5})for(double y:{0.5,2535.99,2536.0,2799.5}){
        int owners=0;for(const auto& tile:grid)if(x>=tile.left&&x<tile.right&&y>=tile.top&&y<tile.bottom)++owners;
        require(owners==1,"Wide/tall OCR ownership must cover each boundary point exactly once");
    }
    rejects([] { desk::ocr_detail::verticalTiles(30000,128); },"Tile count must have a finite resource budget");
}
void overlay() {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 200; info.bmiHeader.biHeight = -100;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP source = CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    require(source != nullptr,"Synthetic bitmap allocation");
    auto* colors = static_cast<unsigned*>(pixels);
    for (int i = 0; i < 20000; ++i) colors[i] = 0xff444444;
    OcrResult ocr; ocr.width = 200; ocr.height = 100;
    ocr.lines.push_back({L"original",20,20,150,30});
    HBITMAP result = renderTranslation(source,ocr,{L"Translated text"});
    require(result && result != source,"Translation must return a separate bitmap");
    DIBSECTION dib{}; require(GetObjectW(result,sizeof(dib),&dib) != 0,"Translated bitmap metadata");
    auto* translated = static_cast<unsigned*>(dib.dsBm.bmBits);
    require(translated[30*200+25] != colors[30*200+25],"Translation adds a readable background");
    require(colors[30*200+25] == 0xff444444,"Source remains unchanged");
    require(translated[0] == colors[0],"Background context remains outside translated region");
    rejects([&] { renderTranslation(source,ocr,{}); },"Mismatched translations must fail");
    DeleteObject(result); DeleteObject(source);
}
void scaledOverlay() {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 240; info.bmiHeader.biHeight = -120;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* storage = nullptr;
    HBITMAP source = CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&storage,nullptr,0);
    require(source != nullptr,"Scaled fixture allocation");
    auto* pixels = static_cast<unsigned*>(storage);
    for (int y = 0; y < 120; ++y) for (int x = 0; x < 240; ++x)
        pixels[y * 240 + x] = 0xff000000 | ((x * 7u) & 255) | (((y * 9u) & 255) << 8) | 0x00440000;
    // The worker reports half-size OCR coordinates. Bright source ink must be
    // replaced only inside its mapped rectangle in the full-size screenshot.
    for (int y = 40; y < 80; ++y) for (int x = 40; x < 200; ++x) pixels[y * 240 + x] = 0xffff0000;
    OcrResult ocr; ocr.width = 120; ocr.height = 60;
    ocr.lines.push_back({L"source",20,20,80,20});
    HBITMAP result = nullptr;
    try { result = renderTranslation(source,ocr,{L"Translated image"}); }
    catch (...) { DeleteObject(source); throw; }
    DIBSECTION dib{}; require(GetObjectW(result,sizeof(dib),&dib) != 0,"Scaled output metadata");
    require(dib.dsBm.bmWidth == 240 && dib.dsBm.bmHeight == 120,"Translation retains original screenshot resolution");
    auto* output = static_cast<unsigned*>(dib.dsBm.bmBits);
    unsigned glyphs = 0, changed = 0;
    for (int y = 0; y < 120; ++y) for (int x = 0; x < 240; ++x) {
        const auto at = y * 240 + x;
        if (x >= 40 && x < 200 && y >= 40 && y < 80) {
            require(output[at] != 0xffff0000,"Original OCR text pixels must be erased");
            changed += output[at] != pixels[at];
            glyphs += (output[at] & 255) < 100 && ((output[at] >> 8) & 255) < 100 && ((output[at] >> 16) & 255) < 100;
            require(pixels[at] == 0xffff0000,"Borrowed original text pixels stay intact");
        } else require(output[at] == pixels[at],"Pixels outside the scaled OCR rectangle must remain unchanged");
    }
    require(changed == 160 * 40 && glyphs > 30,"Translated glyphs must visibly replace source OCR pixels");
    DeleteObject(result); DeleteObject(source);
}
struct LocalProvider {
    // WinHTTP can retain asynchronous socket work after a request returns.
    // Keep Winsock alive until process exit rather than between fixtures.
    struct WinsockRuntime {
        WinsockRuntime() { WSADATA startup{}; require(WSAStartup(MAKEWORD(2,2),&startup) == 0,"Loopback network initialization"); }
        ~WinsockRuntime() { WSACleanup(); }
    };
    SOCKET listener = INVALID_SOCKET;
    std::thread serving;
    unsigned short port = 0;
    struct RequestSnapshot { std::string startLine, headers, body; };
    std::promise<RequestSnapshot> captured;
    std::shared_future<RequestSnapshot> completedCapture{captured.get_future().share()};
    explicit LocalProvider(std::string body, int status = 200, bool delay = false) {
        static WinsockRuntime runtime;
        listener = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        require(listener != INVALID_SOCKET,"Loopback provider socket");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) == 0 && listen(listener,1) == 0,"Loopback provider bind");
        int length = sizeof(address); require(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length) == 0,"Loopback provider port");
        port = ntohs(address.sin_port);
        serving = std::thread([this,body = std::move(body),status,delay] {
            const SOCKET client = accept(listener,nullptr,nullptr);
            if (client == INVALID_SOCKET) {
                captured.set_exception(std::make_exception_ptr(std::runtime_error("Synthetic provider accept failed.")));
                return;
            }
            const DWORD receiveTimeout = 10000;
            setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&receiveTimeout),sizeof(receiveTimeout));
            char chunk[4096]{};
            bool published = false;
            try {
                std::string wire;
                auto receive = [&] {
                    const int received = recv(client,chunk,sizeof(chunk),0);
                    if (received <= 0) throw std::runtime_error("Synthetic request headers or body are incomplete.");
                    if (wire.size() + received > 1024 * 1024) throw std::runtime_error("Synthetic request exceeded fixture limit.");
                    wire.append(chunk,received);
                };
                while (wire.find("\r\n\r\n") == std::string::npos) receive();
                const size_t split = wire.find("\r\n\r\n");
                const size_t firstLine = wire.find("\r\n");
                if (firstLine == std::string::npos || firstLine > split) throw std::runtime_error("Synthetic request line is invalid.");
                const auto startLine = wire.substr(0,firstLine);
                if (!startLine.ends_with(" HTTP/1.1")) throw std::runtime_error("Synthetic HTTP request line is incomplete or invalid.");
                std::string headerNames = wire.substr(firstLine + 2,split - firstLine - 2);
                std::transform(headerNames.begin(),headerNames.end(),headerNames.begin(),[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                size_t expected = 0;
                const size_t contentHeader = headerNames.find("content-length:");
                if (contentHeader != std::string::npos) expected = std::stoull(headerNames.substr(contentHeader + 15));
                if (expected > 1024 * 1024) throw std::runtime_error("Synthetic request body exceeded fixture limit.");
                while (wire.size() - split - 4 < expected) receive();
                RequestSnapshot snapshot{startLine,wire.substr(firstLine + 2,split - firstLine - 2),wire.substr(split + 4,expected)};
                // Publish once, before responding. Tests observe an immutable,
                // fully received request, never a partially filled wire buffer.
                captured.set_value(std::move(snapshot)); published = true;
                if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const std::string response = "HTTP/1.1 " + std::to_string(status) + " OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                size_t sent = 0;
                while (sent < response.size()) {
                    const int count = send(client,response.data() + sent,static_cast<int>(response.size() - sent),0);
                    if (count <= 0) break;
                    sent += count;
                }
                const DWORD drainTimeout = 2000;
                setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&drainTimeout),sizeof(drainTimeout));
                // Drain the peer before closing; abrupt server teardown can
                // race WinHTTP's asynchronous response setup on Windows.
                while (recv(client,chunk,sizeof(chunk),0) > 0) {}
            } catch (...) {
                if (!published) captured.set_exception(std::current_exception());
            }
            closesocket(client);
        });
    }
    ~LocalProvider() { if (listener != INVALID_SOCKET) closesocket(listener); if (serving.joinable()) serving.join(); }
    RequestSnapshot requestSnapshot() {
        require(completedCapture.wait_for(std::chrono::seconds(15)) == std::future_status::ready,"Synthetic request capture timed out");
        return completedCapture.get();
    }
    TranslationConfig config() const {
        TranslationConfig value; value.provider = L"libre"; value.libreUrl = L"http://127.0.0.1:" + std::to_wstring(port); return value;
    }
};
struct ImageFixture {
    std::filesystem::path folder, worker;
    HBITMAP source = nullptr;
    explicit ImageFixture(const wchar_t* mode = L"success") {
        folder = std::filesystem::temp_directory_path() / (L"desk-image-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + mode);
        std::filesystem::create_directories(folder);
        wchar_t executable[32768]{};
        require(GetModuleFileNameW(nullptr,executable,32768) != 0,"Image fixture executable path");
        worker = folder / (std::wstring(L"image-worker-") + mode + L".exe");
        std::filesystem::copy_file(executable,worker,std::filesystem::copy_options::overwrite_existing);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 240; info.bmiHeader.biHeight = -120;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        source = CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        require(source != nullptr,"Pipeline fixture allocation");
        std::fill_n(static_cast<unsigned*>(pixels),240 * 120,0xff445566);
    }
    ~ImageFixture() {
        if (source) DeleteObject(source);
        std::error_code ignored;
        std::filesystem::remove(worker,ignored);
        std::filesystem::remove(folder,ignored);
    }
    void requireClean() const {
        for (const auto& item : std::filesystem::directory_iterator(folder))
            require(item.path() == worker,"Pipeline must remove every temporary screenshot on success, error and cancellation");
    }
};
void imagePipeline() {
    ImageFixture fixture;
    LocalProvider provider(R"({"translatedText":["Translated image"]})");
    std::atomic_bool cancel{false};
    const auto before = imageToolReservedBytes();
    HBITMAP output = translateImage(fixture.source,fixture.worker,provider.config(),cancel,fixture.folder);
    require(output != nullptr && output != fixture.source,"Pipeline returns an independently owned translated bitmap");
    const auto request = json::parse(provider.requestSnapshot().body);
    require(request["q"] == json::array({"Synthetic source"}),"Pipeline must send OCR text to the selected provider");
    DIBSECTION dib{}; require(GetObjectW(output,sizeof(dib),&dib) != 0,"Pipeline output pixels");
    const auto* pixels = static_cast<unsigned*>(dib.dsBm.bmBits);
    unsigned glyphs = 0;
    for (int y = 40; y < 80; ++y) for (int x = 40; x < 200; ++x)
        glyphs += (pixels[y * 240 + x] & 255) < 100 && ((pixels[y * 240 + x] >> 8) & 255) < 100 && ((pixels[y * 240 + x] >> 16) & 255) < 100;
    require(glyphs > 30,"Pipeline output contains visibly drawn provider translation glyphs");
    require(pixels[0] == 0xff445566,"Pipeline preserves screenshot context");
    HBITMAP expected = renderTranslation(fixture.source,translation_detail::parseOcrJson(R"({"version":1,"ok":true,"width":120,"height":60,"lines":[{"text":"Synthetic source","x":20,"y":20,"width":80,"height":20}]})"),{L"Translated image"});
    DIBSECTION reference{}; GetObjectW(expected,sizeof(reference),&reference);
    require(std::equal(pixels,pixels + 240 * 120,static_cast<unsigned*>(reference.dsBm.bmBits)),"Rendered pixels must use provider translation rather than original OCR text");
    DeleteObject(expected); DeleteObject(output);
    require(imageToolReservedBytes() == before,"Pipeline releases transient encoder reservation");
    fixture.requireClean();
}
void imagePipelineErrors() {
    std::atomic_bool cancel{true};
    ImageFixture success;
    rejects([&] { translateImage(success.source,success.worker,TranslationConfig{},cancel,success.folder); },"Cancelled image request cannot start OCR or provider requests");
    success.requireClean();
    cancel = false;
    ImageFixture blank(L"empty");
    bool empty = false;
    try { translateImage(blank.source,blank.worker,TranslationConfig{},cancel,blank.folder); }
    catch (const std::exception& error) { empty = std::string(error.what()).find("没有识别到可翻译文字") != std::string::npos; }
    require(empty,"No OCR lines must be an explicit no-text translation error");
    blank.requireClean();
    {
        LocalProvider provider(R"({"translatedText":[]})");
        rejects([&] { translateImage(success.source,success.worker,provider.config(),cancel,success.folder); },"Provider failure cannot fall back to an OCR-only bitmap");
        success.requireClean();
    }
    const auto before = imageToolReservedBytes();
    auto occupied = image_tools_detail::reserveMemory(image_tools_detail::imageWorkingBytes);
    require(occupied != nullptr,"Synthetic global image budget exhaustion");
    rejects([&] { translateImage(success.source,success.worker,TranslationConfig{},cancel,success.folder); },"Encoder must honor the shared global image memory budget");
    occupied.reset();
    require(imageToolReservedBytes() == before,"Failed image pipeline releases memory");
    success.requireClean();
}
void imagePipelineCancellation() {
    {
        ImageFixture fixture(L"cancel");
        std::atomic_bool cancel{false};
        std::thread cancellation([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel = true; });
        bool cancelled = false;
        const auto started = std::chrono::steady_clock::now();
        try { translateImage(fixture.source,fixture.worker,TranslationConfig{},cancel,fixture.folder); }
        catch (const std::exception& error) { cancelled = std::string(error.what()).find("cancelled") != std::string::npos; }
        cancellation.join();
        require(cancelled,"Image pipeline cancellation must terminate an active OCR child");
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),"Cancelled image OCR returns promptly");
        fixture.requireClean();
    }
    ImageFixture fixture;
    LocalProvider provider(R"({"translatedText":["Delayed translation"]})",200,true);
    std::atomic_bool cancel{false};
    std::thread cancellation([&] { provider.requestSnapshot(); cancel = true; });
    bool cancelled = false;
    const auto started = std::chrono::steady_clock::now();
    try { translateImage(fixture.source,fixture.worker,provider.config(),cancel,fixture.folder); }
    catch (const std::exception& error) { cancelled = std::string(error.what()).find("cancelled") != std::string::npos; }
    cancellation.join();
    require(cancelled,"Image pipeline cancellation must reach an active provider request");
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),"Cancelled image translation returns promptly");
    fixture.requireClean();
}
void loopbackNetwork() {
    {
        LocalProvider provider(R"({"translatedText":["translated one","translated two"]})");
        auto output = translateLines({L"source one",L"source two"},provider.config());
        require(output == std::vector<std::wstring>{L"translated one",L"translated two"},"Real WinHTTP must parse a bounded local provider response");
        const auto body = json::parse(provider.requestSnapshot().body);
        require(body["q"] == json::array({"source one","source two"}),"Only OCR text must enter provider payload");
    }
    {
        LocalProvider provider(R"({"character_count":42,"character_limit":500000})");
        TranslationConfig config; config.provider = L"deepl"; config.deeplKey = L"synthetic-local-only";
        auto request = translation_detail::buildUsageRequest(config);
        request.url = L"http://127.0.0.1:" + std::to_wstring(provider.port) + L"/v2/usage";
        const auto usage = translation_detail::parseUsageResponse(translation_detail::executeRequest(request));
        require(usage.used == 42 && usage.limit == 500000,"Usage request shares the real bounded HTTP transport");
        const auto snapshot = provider.requestSnapshot();
        require(snapshot.startLine.starts_with("GET /v2/usage "),"Usage query uses GET for the usage endpoint");
        require(snapshot.body.empty(),"Usage query sends no screenshot text");
    }
    {
        LocalProvider provider(R"({"error":"limited"})",429);
        bool rateLimit = false;
        try { translateLines({L"sample"},provider.config()); }
        catch (const std::exception& error) { rateLimit = std::string(error.what()).find("rate limit") != std::string::npos; }
        require(rateLimit,"HTTP429 must remain distinct and cannot trigger fallback");
    }
    {
        LocalProvider provider(R"({"translatedText":["late"]})",200,true);
        std::atomic_bool cancel{false};
        std::thread cancellation([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel = true; });
        const auto started = std::chrono::steady_clock::now();
        bool cancelled = false;
        try { translateLines({L"sample"},provider.config(),&cancel); }
        catch (const std::exception& error) { cancelled = std::string(error.what()).find("cancelled") != std::string::npos; }
        cancellation.join();
        require(cancelled,"In-flight WinHTTP must honour cancellation");
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(1),"Cancellation must release the caller promptly");
    }
}
void incompleteLoopbackRequest() {
    LocalProvider provider(R"({"character_count":42,"character_limit":500000})");
    const SOCKET client = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    require(client != INVALID_SOCKET,"Incomplete-request probe socket");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(provider.port);
    require(connect(client,reinterpret_cast<sockaddr*>(&address),sizeof(address)) == 0,"Incomplete-request probe connection");
    const DWORD timeout = 5000; setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    const std::string partial = "unfinished synthetic HTTP request";
    send(client,partial.data(),static_cast<int>(partial.size()),0);
    shutdown(client,SD_SEND);
    char response[512]{};
    const int received = recv(client,response,sizeof(response),0);
    closesocket(client); provider.serving.join();
    require(received <= 0,"Synthetic provider must reject incomplete request headers before returning a successful response");
    rejects([&] { provider.requestSnapshot(); },"Incomplete request must produce a rejected capture rather than an empty successful snapshot");
}
void ocrCancellation() {
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    const auto image=std::filesystem::temp_directory_path()/(L"desk-ocr-cancel-"+std::to_wstring(GetCurrentProcessId())+L".bin");
    {std::ofstream file(image);file<<"synthetic cancellation fixture";}
    std::atomic_bool cancelled{false};
    std::thread trigger([&]{std::this_thread::sleep_for(std::chrono::milliseconds(150));cancelled=true;});
    auto start=std::chrono::steady_clock::now();bool honored=false;
    try{runOcr(executable,image,&cancelled);}catch(const std::exception& error){honored=std::string(error.what()).find("cancelled")!=std::string::npos;}
    trigger.join();std::filesystem::remove(image);
    require(honored,"running OCR child must honor caller cancellation");
    require(std::chrono::steady_clock::now()-start<std::chrono::seconds(1),"cancelled OCR must release the caller within one second");
}
}
int wmain(int argc, wchar_t** argv) {
    // A real child process fixture exercises the worker protocol without OCR models.
    if (argc == 5 && std::wstring(argv[1]) == L"--input" && std::wstring(argv[3]) == L"--output") {
        const auto fixture = std::filesystem::path(argv[0]).stem().wstring();
        if (fixture.starts_with(L"image-worker-")) {
            if (fixture == L"image-worker-cancel") Sleep(5000);
            std::ifstream input(std::filesystem::path(argv[2]),std::ios::binary);
            std::array<unsigned char,8> signature{};
            input.read(reinterpret_cast<char*>(signature.data()),signature.size());
            if (signature != std::array<unsigned char,8>{137,80,78,71,13,10,26,10}) return 3;
            std::ofstream output(std::filesystem::path(argv[4]),std::ios::binary);
            if (fixture == L"image-worker-empty") output << R"({"version":1,"ok":true,"width":120,"height":60,"text":"","lines":[]})";
            else output << R"({"version":1,"ok":true,"width":120,"height":60,"text":"Synthetic source","lines":[{"text":"Synthetic source","x":20,"y":20,"width":80,"height":20}]})";
            return 0;
        }
        if(std::filesystem::path(argv[2]).filename().wstring().starts_with(L"desk-ocr-cancel-"))Sleep(5000);
        std::ofstream output(std::filesystem::path(argv[4]),std::ios::binary);
        output << R"({"version":1,"ok":false,"error":"language_missing"})";
        return 2;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--ocr-worker") {
        const auto image = std::filesystem::temp_directory_path() / L"desk-ocr-identity-test.png";
        { std::ofstream fixture(image,std::ios::binary); fixture << "synthetic"; }
        bool identityError = false;
        try { runOcr(argv[2],image); }
        catch (const std::exception& error) { identityError = std::string(error.what()).find("package identity") != std::string::npos; }
        std::filesystem::remove(image);
        if (!identityError) { std::cerr << "FAIL unpackaged worker must report package identity requirement\n"; return 1; }
        std::cout << "PASS unpackaged worker identity error\n"; return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--live-google-free") {
        try {
            TranslationConfig config;
            const auto result = translateLines({L"Hello"},config);
            require(result.size() == 1 && !result.front().empty() && result.front() != L"Hello","Live experimental Google must translate the public synthetic sample into the default Chinese target");
            std::cout << "PASS experimental Google translated one synthetic line\n"; return 0;
        } catch (const std::exception& error) { std::cerr << "External connectivity check: " << error.what() << '\n'; return 1; }
    }
    int failures = 0;
    for (const auto& [name,test] : std::vector<std::pair<const char*,std::function<void()>>>{{"response parsing",parsing},{"request validation",requests},{"DeepL usage",usageParsing},{"OCR protocol",ocrParsing},{"OCR word joining",ocrWordJoining},{"OCR tile ownership",ocrTileOwnership},{"synthetic overlay",overlay},{"scaled OCR overlay",scaledOverlay},{"image translation pipeline",imagePipeline},{"image pipeline failures and memory budget",imagePipelineErrors},{"image pipeline network cancellation",imagePipelineCancellation},{"loopback provider and cancellation",loopbackNetwork},{"incomplete loopback request rejection",incompleteLoopbackRequest},{"OCR cancellation",ocrCancellation}}) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    return failures == 0 ? 0 : 1;
}
