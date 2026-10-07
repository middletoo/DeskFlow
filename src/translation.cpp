#include "translation.hpp"
#include "image_tools.hpp"
#include "json.hpp"
#include <winhttp.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace desk {
namespace {
using nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr size_t maxResponse = 4 * 1024 * 1024;
constexpr size_t maxOcrOutput = 16 * 1024 * 1024;
void checkCancelled(std::atomic_bool* cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Translation cancelled.");
}
bool hasText(std::wstring_view text) {
    return std::any_of(text.begin(),text.end(),[](wchar_t ch) { return !std::iswspace(ch); });
}
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if (!size) throw std::runtime_error("Text contains invalid Unicode.");
    std::string result(size,'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr);
    return result;
}
std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if (!size) throw std::runtime_error("Provider returned invalid Unicode.");
    std::wstring result(size,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size);
    return result;
}
std::string encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') result += static_cast<char>(ch);
        else { result += '%'; result += hex[ch >> 4]; result += hex[ch & 15]; }
    }
    return result;
}
void validateToken(std::wstring_view token, const char* emptyMessage) {
    if (token.empty()) throw std::runtime_error(emptyMessage);
    if (token.size() > 4096 || token.find_first_of(L"\r\n") != std::wstring_view::npos || token.find(L'\0') != std::wstring_view::npos)
        throw std::runtime_error("Configuration contains an invalid header value.");
}
std::string language(std::wstring_view value, bool deepl) {
    if (value.empty() || value.size() > 32 || !std::all_of(value.begin(),value.end(),[](wchar_t ch) { return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-'; }))
        throw std::runtime_error("Target language is invalid.");
    std::string result = utf8(value);
    if (deepl) {
        std::transform(result.begin(),result.end(),result.begin(),[](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (result == "ZH-CN" || result == "ZH-HANS") result = "ZH";
        if (result == "ZH-TW") result = "ZH-HANT";
    }
    return result;
}
std::wstring decodeEntities(std::wstring text) {
    const std::array<std::pair<std::wstring_view,std::wstring_view>,6> entities{{{L"&quot;",L"\""},{L"&#39;",L"'"},{L"&#x27;",L"'"},{L"&lt;",L"<"},{L"&gt;",L">"},{L"&amp;",L"&"}}};
    for (const auto& [from,to] : entities) {
        size_t pos = 0;
        while ((pos = text.find(from,pos)) != std::wstring::npos) { text.replace(pos,from.size(),to); pos += to.size(); }
    }
    return text;
}
struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET handle = nullptr) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
struct HttpState {
    std::atomic_int refs{1};
    Handle event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    std::atomic<DWORD> status{0}, error{0}, bytes{0};
    std::array<char,16384> buffer{};
    std::string body;
    void release() { if (refs.fetch_sub(1) == 1) delete this; }
};
void CALLBACK httpCallback(HINTERNET, DWORD_PTR context, DWORD status, void* data, DWORD length) {
    auto* state = reinterpret_cast<HttpState*>(context);
    if (!state) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { state->release(); return; }
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
        state->error = static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError;
    else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) state->bytes = length;
    state->status.store(status,std::memory_order_release);
    SetEvent(state->event.value);
}
[[noreturn]] void networkError(DWORD code) {
    if (code == ERROR_WINHTTP_TIMEOUT) throw std::runtime_error("Translation request timed out.");
    if (code == ERROR_WINHTTP_CANNOT_CONNECT || code == ERROR_WINHTTP_NAME_NOT_RESOLVED)
        throw std::runtime_error("Translation provider is unreachable; check network and proxy settings.");
    if (code == ERROR_WINHTTP_SECURE_FAILURE) throw std::runtime_error("Translation TLS certificate validation failed.");
    throw std::runtime_error("Translation network request failed (Windows error " + std::to_string(code) + ").");
}
std::string performRequest(const translation_detail::Request& input, const std::wstring& proxy, std::atomic_bool* cancel, Clock::time_point deadline) {
    checkCancelled(cancel);
    URL_COMPONENTS url{}; url.dwStructSize = sizeof(url);
    url.dwHostNameLength = url.dwUrlPathLength = url.dwExtraInfoLength = url.dwUserNameLength = url.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(input.url.c_str(),static_cast<DWORD>(input.url.size()),0,&url) || (url.nScheme != INTERNET_SCHEME_HTTPS && url.nScheme != INTERNET_SCHEME_HTTP) || url.dwUserNameLength || url.dwPasswordLength)
        throw std::runtime_error("Translation service URL is invalid.");
    std::wstring host(url.lpszHostName,url.dwHostNameLength), path(url.lpszUrlPath,url.dwUrlPathLength);
    if (url.dwExtraInfoLength) path.append(url.lpszExtraInfo,url.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    if (path.find(L'#') != std::wstring::npos) throw std::runtime_error("Translation service URL cannot contain a fragment.");
    InternetHandle session(WinHttpOpen(L"DeskFlow/0.1",proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(),WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC));
    if (!session.value) networkError(GetLastError());
    if (!WinHttpSetTimeouts(session.value,6000,6000,10000,15000)) networkError(GetLastError());
    InternetHandle connection(WinHttpConnect(session.value,host.c_str(),url.nPort,0));
    if (!connection.value) networkError(GetLastError());
    auto* state = new HttpState;
    std::unique_ptr<HttpState,void(*)(HttpState*)> owner(state,[](HttpState* item) { item->release(); });
    if (!state->event.value) throw std::runtime_error("Cannot allocate translation completion event.");
    state->body = input.body;
    InternetHandle request(WinHttpOpenRequest(connection.value,input.method.c_str(),path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request.value) networkError(GetLastError());
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects))) networkError(GetLastError());
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state);
    if (!WinHttpSetOption(request.value,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))) networkError(GetLastError());
    if (WinHttpSetStatusCallback(request.value,httpCallback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,0) == WINHTTP_INVALID_STATUS_CALLBACK)
        networkError(GetLastError());
    state->refs.fetch_add(1); // The handle owns a reference until its final HANDLE_CLOSING callback.
    auto awaitStatus = [&](DWORD expected) {
        for (;;) {
            checkCancelled(cancel);
            if (Clock::now() >= deadline) throw std::runtime_error("Translation request timed out.");
            const DWORD wait = WaitForSingleObject(state->event.value,100);
            if (wait == WAIT_FAILED) throw std::runtime_error("Cannot wait for translation request.");
            if (wait != WAIT_OBJECT_0) continue;
            const DWORD status = state->status.load(std::memory_order_acquire);
            if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) networkError(state->error.load());
            if (status == expected) return;
        }
    };
    if (!WinHttpSendRequest(request.value,input.headers.c_str(),static_cast<DWORD>(input.headers.size()),state->body.empty() ? WINHTTP_NO_REQUEST_DATA : state->body.data(),static_cast<DWORD>(state->body.size()),static_cast<DWORD>(state->body.size()),context)) networkError(GetLastError());
    awaitStatus(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    if (!WinHttpReceiveResponse(request.value,nullptr)) networkError(GetLastError());
    awaitStatus(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&statusSize,WINHTTP_NO_HEADER_INDEX)) networkError(GetLastError());
    if (status < 200 || status >= 300) throw std::runtime_error(translation_detail::statusError(status));
    std::string result;
    for (;;) {
        checkCancelled(cancel);
        if (!WinHttpReadData(request.value,state->buffer.data(),static_cast<DWORD>(state->buffer.size()),nullptr)) networkError(GetLastError());
        awaitStatus(WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
        const DWORD received = state->bytes.load();
        if (!received) break;
        if (result.size() + received > maxResponse) throw std::runtime_error("Translation response exceeded the size limit.");
        result.append(state->buffer.data(),received);
    }
    return result;
}
std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') result.append(slashes * 2 + 1,L'\\'); else result.append(slashes,L'\\');
        slashes = 0; result += ch;
    }
    result.append(slashes * 2,L'\\'); result += L'\"'; return result;
}
struct TempOutput {
    std::filesystem::path path;
    ~TempOutput() { std::error_code error; std::filesystem::remove(path,error); }
};
}

namespace translation_detail {
Request buildUsageRequest(const TranslationConfig& config) {
    if (config.provider != L"deepl") throw std::runtime_error("Usage query is available for the selected DeepL provider only.");
    validateToken(config.deeplKey,"DeepL API key is required.");
    Request request;
    request.method = L"GET";
    request.url = config.deeplFree ? L"https://api-free.deepl.com/v2/usage" : L"https://api.deepl.com/v2/usage";
    request.headers = L"Accept: application/json\r\nAuthorization: DeepL-Auth-Key " + config.deeplKey + L"\r\n";
    return request;
}
DeepLUsage parseUsageResponse(const std::string& body) {
    if (body.size() > maxResponse) throw std::runtime_error("DeepL usage response exceeded the size limit.");
    try {
        const auto response = json::parse(body);
        const auto& used = response.at("character_count");
        const auto& limit = response.at("character_limit");
        if (!used.is_number_integer() || !limit.is_number_integer()) throw std::runtime_error("type");
        DeepLUsage result{used.get<int64_t>(),limit.get<int64_t>()};
        if (result.used < 0 || result.limit < 0) throw std::runtime_error("range");
        return result;
    } catch (const std::exception&) { throw std::runtime_error("DeepL returned an invalid usage response."); }
}
std::string executeRequest(const Request& request, const std::wstring& proxy, std::atomic_bool* cancel) {
    return performRequest(request,proxy,cancel,Clock::now() + std::chrono::seconds(45));
}
Request buildRequest(const std::vector<std::wstring>& lines, const TranslationConfig& config) {
    if (lines.empty() || lines.size() > 50) throw std::runtime_error("Translation request must contain between 1 and 50 lines.");
    size_t total = 0; std::vector<std::string> encoded;
    for (const auto& line : lines) { auto text = utf8(line); total += text.size(); encoded.push_back(std::move(text)); }
    if (total > 100 * 1024) throw std::runtime_error("Translation text exceeded the request size limit.");
    const auto target = language(config.target,config.provider == L"deepl");
    Request result;
    result.headers = L"Content-Type: application/json; charset=utf-8\r\n";
    if (config.provider == L"deepl") {
        validateToken(config.deeplKey,"DeepL API key is required.");
        result.url = config.deeplFree ? L"https://api-free.deepl.com/v2/translate" : L"https://api.deepl.com/v2/translate";
        result.headers += L"Authorization: DeepL-Auth-Key " + config.deeplKey + L"\r\n";
        result.body = json{{"text",encoded},{"target_lang",target}}.dump();
    } else if (config.provider == L"google") {
        validateToken(config.googleKey,"Google Cloud Translation API key is required.");
        result.url = L"https://translation.googleapis.com/language/translate/v2";
        result.headers += L"X-Goog-Api-Key: " + config.googleKey + L"\r\n";
        result.body = json{{"q",encoded},{"target",target},{"format","text"}}.dump();
    } else if (config.provider == L"libre") {
        if (config.libreUrl.empty()) throw std::runtime_error("LibreTranslate service URL is required.");
        if (!(config.libreUrl.starts_with(L"https://") || config.libreUrl.starts_with(L"http://")) || config.libreUrl.find_first_of(L"\r\n?#") != std::wstring::npos || config.libreUrl.find(L'@') != std::wstring::npos)
            throw std::runtime_error("LibreTranslate URL must be an HTTP(S) service address without credentials or a query.");
        result.url = config.libreUrl;
        while (!result.url.empty() && result.url.back() == L'/') result.url.pop_back();
        if (!result.url.ends_with(L"/translate")) result.url += L"/translate";
        result.body = json{{"q",encoded},{"source","auto"},{"target",target.substr(0,target.find('-'))},{"format","text"}}.dump();
    } else if (config.provider == L"google-free") {
        if (lines.size() != 1) throw std::runtime_error("Experimental Google adapter accepts one OCR line per request.");
        result.method = L"GET";
        result.url = L"https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=" + wide(encode(target)) + L"&dt=t&q=" + wide(encode(encoded.front()));
        result.headers = L"Accept: application/json\r\n";
        if (result.url.size() > 16384) throw std::runtime_error("OCR line exceeded the experimental Google URL limit; select a smaller region or another provider.");
    } else throw std::runtime_error("Unknown translation provider. Select a configured provider.");
    if (result.body.size() > 128 * 1024) throw std::runtime_error("Encoded translation request exceeded the provider size limit.");
    return result;
}
std::vector<std::wstring> parseResponse(const std::wstring& provider, const std::string& body, size_t expectedLines) {
    if (body.size() > maxResponse) throw std::runtime_error("Translation response exceeded the size limit.");
    std::vector<std::wstring> result;
    try {
        const auto data = json::parse(body);
        if (provider == L"google-free") {
            if (expectedLines != 1 || !data.is_array() || data.empty() || !data[0].is_array()) throw std::runtime_error("shape");
            std::wstring combined;
            for (const auto& segment : data[0]) {
                if (!segment.is_array() || segment.empty() || !segment[0].is_string()) throw std::runtime_error("segment");
                combined += wide(segment[0].get<std::string>());
            }
            result.push_back(std::move(combined));
        } else if (provider == L"google") {
            for (const auto& item : data.at("data").at("translations")) result.push_back(decodeEntities(wide(item.at("translatedText").get<std::string>())));
        } else if (provider == L"deepl") {
            for (const auto& item : data.at("translations")) result.push_back(wide(item.at("text").get<std::string>()));
        } else if (provider == L"libre") {
            const auto& text = data.at("translatedText");
            if (text.is_string()) result.push_back(wide(text.get<std::string>()));
            else if (text.is_array()) for (const auto& item : text) result.push_back(wide(item.get<std::string>()));
            else throw std::runtime_error("shape");
        } else throw std::runtime_error("provider");
        if (result.size() != expectedLines || std::any_of(result.begin(),result.end(),[](const auto& line) { return !hasText(line); })) throw std::runtime_error("line count");
    } catch (const std::exception&) {
        throw std::runtime_error("Translation provider returned an invalid response or a different number of lines.");
    }
    return result;
}
std::string statusError(unsigned status) {
    if (status == 401 || status == 403) return "Translation authentication failed; check the selected provider API key and permissions.";
    if (status == 429) return "Translation rate limit reached; wait before retrying.";
    if (status == 456 || status == 402) return "Translation quota exhausted; check provider usage and billing.";
    if (status == 413 || status == 414) return "Translation provider rejected the request size.";
    if (status >= 500) return "Translation provider is temporarily unavailable (HTTP " + std::to_string(status) + ").";
    return "Translation provider rejected the request (HTTP " + std::to_string(status) + ").";
}
OcrResult parseOcrJson(const std::string& body) {
    if (body.size() > maxOcrOutput) throw std::runtime_error("OCR output exceeded the size limit.");
    json data;
    try { data = json::parse(body); } catch (...) { throw std::runtime_error("OCR worker returned malformed output."); }
    if (data.value("version",0) != 1) throw std::runtime_error("OCR worker protocol version is unsupported.");
    if (!data.value("ok",false)) {
        const auto error = data.value("error",std::string{});
        std::string safeCode;
        if (data.contains("hresult") && data["hresult"].is_string()) {
            const auto candidate = data["hresult"].get<std::string>();
            if (candidate.size() == 10 && candidate.starts_with("0x") && std::all_of(candidate.begin() + 2,candidate.end(),[](unsigned char ch) { return std::isxdigit(ch) != 0; })) safeCode = "（Windows " + candidate + "）";
        }
        const auto stage = data.value("stage",std::string{});
        if (error == "identity_required") throw std::runtime_error("Local OCR requires package identity. Install the signed DeskFlow development MSIX package using the documented explicit certificate trust steps.");
        if (error == "language_missing") throw std::runtime_error("Windows OCR language resources are missing. In Windows Settings > Time & language > Language & region, add Chinese or English and install its optional language features.");
        if (error == "image_invalid") {
            if (stage == "open_image" || stage == "open_stream") throw std::runtime_error("OCR 输入图片无法打开" + safeCode + "，请重新截图。");
            if (stage == "image_dimensions") throw std::runtime_error("OCR 图片尺寸超出支持范围，请缩小选区。");
            throw std::runtime_error("OCR 图片解码失败" + safeCode + "，请重新截图。");
        }
        if (error == "engine_failed") throw std::runtime_error("Windows OCR 识别引擎执行失败" + safeCode + "，请重试并检查 Windows 语言资源。");
        throw std::runtime_error("Local OCR worker failed. Check package identity and Windows OCR language resources.");
    }
    OcrResult result;
    try {
        result.width = data.at("width").get<int>(); result.height = data.at("height").get<int>();
        if (result.width <= 0 || result.height <= 0 || result.width > 32768 || result.height > 32768) throw std::runtime_error("dimensions");
        result.text = wide(data.value("text",std::string{}));
        const auto& lines = data.at("lines");
        if (!lines.is_array() || lines.size() > 10000) throw std::runtime_error("lines");
        for (const auto& item : lines) {
            OcrLine line{wide(item.at("text").get<std::string>()),item.at("x").get<float>(),item.at("y").get<float>(),item.at("width").get<float>(),item.at("height").get<float>()};
            if (!std::isfinite(line.x) || !std::isfinite(line.y) || !std::isfinite(line.width) || !std::isfinite(line.height) || line.x < 0 || line.y < 0 || line.width < 0 || line.height < 0 || line.x + line.width > result.width + 1 || line.y + line.height > result.height + 1) throw std::runtime_error("bounds");
            result.lines.push_back(std::move(line));
        }
    } catch (...) { throw std::runtime_error("OCR worker returned invalid dimensions or line bounds."); }
    return result;
}
}

DeepLUsage queryDeepLUsage(const TranslationConfig& config, std::atomic_bool* cancel) {
    checkCancelled(cancel);
    auto request = translation_detail::buildUsageRequest(config);
    const auto response = translation_detail::executeRequest(request,config.proxy,cancel);
    checkCancelled(cancel);
    return translation_detail::parseUsageResponse(response);
}

OcrResult runOcr(const std::filesystem::path& workerExe, const std::filesystem::path& image,
                 const std::atomic_bool* cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Local OCR cancelled.");
    if (!std::filesystem::is_regular_file(workerExe)) throw std::runtime_error("DeskOCR.exe is missing. Rebuild or reinstall DeskFlow.");
    if (!std::filesystem::is_regular_file(image)) throw std::runtime_error("OCR input image is missing.");
    wchar_t folder[MAX_PATH + 1]{}, output[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH,folder) || !GetTempFileNameW(folder,L"docr",0,output)) throw std::runtime_error("Cannot create local OCR result file.");
    TempOutput temp{output};
    Handle job(CreateJobObjectW(nullptr,nullptr));
    if (!job.value) throw std::runtime_error("Cannot isolate OCR worker.");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = 512ULL * 1024 * 1024;
    if (!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) throw std::runtime_error("Cannot limit OCR worker resources.");
    auto executable = std::filesystem::absolute(workerExe).wstring();
    std::wstring command = quoteArgument(executable) + L" --input " + quoteArgument(std::filesystem::absolute(image).wstring()) + L" --output " + quoteArgument(temp.path.wstring());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,nullptr,&startup,&process))
        throw std::runtime_error("Cannot start isolated local OCR worker (Windows error " + std::to_string(GetLastError()) + ").");
    Handle processHandle(process.hProcess), threadHandle(process.hThread);
    if (!AssignProcessToJobObject(job.value,process.hProcess)) { TerminateProcess(process.hProcess,2); throw std::runtime_error("Cannot attach OCR worker to its resource limit."); }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) throw std::runtime_error("Cannot resume local OCR worker.");
    DWORD waited = WAIT_TIMEOUT;
    const auto deadline = GetTickCount64() + 30000;
    while (waited == WAIT_TIMEOUT) {
        if (cancel && cancel->load()) {
            TerminateJobObject(job.value,4);WaitForSingleObject(process.hProcess,2000);
            throw std::runtime_error("Local OCR cancelled.");
        }
        auto now = GetTickCount64();
        if (now >= deadline) {
            TerminateJobObject(job.value,3);WaitForSingleObject(process.hProcess,2000);
            throw std::runtime_error("Local OCR timed out; select a smaller region and retry.");
        }
        waited = WaitForSingleObject(process.hProcess,(DWORD)std::min<ULONGLONG>(50,deadline-now));
    }
    if (waited != WAIT_OBJECT_0) throw std::runtime_error("Cannot wait for local OCR worker.");
    if (cancel && cancel->load()) throw std::runtime_error("Local OCR cancelled.");
    DWORD exit = 0; GetExitCodeProcess(process.hProcess,&exit);
    std::error_code error;
    const auto size = std::filesystem::file_size(temp.path,error);
    if (error || size == 0 || size > maxOcrOutput) throw std::runtime_error("OCR worker ended without a valid result (exit " + std::to_string(exit) + ").");
    std::ifstream stream(temp.path,std::ios::binary);
    std::string body(static_cast<size_t>(size),'\0');
    if (!stream.read(body.data(),static_cast<std::streamsize>(body.size()))) throw std::runtime_error("Cannot read local OCR result.");
    auto result = translation_detail::parseOcrJson(body);
    if (exit != 0) throw std::runtime_error("OCR worker exited with an error.");
    return result;
}
std::vector<std::wstring> translateLines(const std::vector<std::wstring>& lines, const TranslationConfig& config, std::atomic_bool* cancel) {
    checkCancelled(cancel);
    if (lines.empty()) return {};
    if (lines.size() > 2000) throw std::runtime_error("Too many OCR lines; select a smaller region.");
    size_t bytes = 0;
    for (const auto& line : lines) bytes += utf8(line).size();
    if (bytes > 100 * 1024) throw std::runtime_error("OCR text exceeded the translation limit; select a smaller region.");
    std::vector<std::wstring> result(lines.size());
    const auto overallDeadline = Clock::now() + std::chrono::seconds(120);
    const size_t batchSize = config.provider == L"google-free" ? 1 : 50;
    for (size_t begin = 0; begin < lines.size(); begin += batchSize) {
        checkCancelled(cancel);
        if (Clock::now() >= overallDeadline) throw std::runtime_error("Translation request timed out.");
        const size_t end = std::min(lines.size(),begin + batchSize);
        std::vector<std::wstring> batch;
        std::vector<size_t> indices;
        for (size_t i = begin; i < end; ++i) if (!lines[i].empty()) { batch.push_back(lines[i]); indices.push_back(i); }
        if (batch.empty()) continue;
        auto request = translation_detail::buildRequest(batch,config);
        const auto body = performRequest(request,config.proxy,cancel,std::min(overallDeadline,Clock::now() + std::chrono::seconds(45)));
        auto parsed = translation_detail::parseResponse(config.provider,body,batch.size());
        for (size_t i = 0; i < indices.size(); ++i) result[indices[i]] = std::move(parsed[i]);
    }
    checkCancelled(cancel);
    return result;
}
HBITMAP renderTranslation(HBITMAP original, const OcrResult& ocr, const std::vector<std::wstring>& translated) {
    BITMAP source{};
    if (!original || !GetObjectW(original,sizeof(source),&source) || source.bmWidth <= 0 || source.bmHeight <= 0)
        throw std::runtime_error("Translation source bitmap is invalid.");
    if (translated.size() != ocr.lines.size() || ocr.width <= 0 || ocr.height <= 0 || ocr.width > 32768 || ocr.height > 32768)
        throw std::runtime_error("Translation line count or OCR dimensions are invalid.");
    if (ocr.lines.empty()) throw std::runtime_error("选区中没有识别到可翻译文字，请重新选择包含文字的区域。");
    for (size_t i = 0; i < ocr.lines.size(); ++i) {
        const auto& line = ocr.lines[i];
        if (!hasText(translated[i])) throw std::runtime_error("Translation provider returned empty translated text.");
        if (!std::isfinite(line.x) || !std::isfinite(line.y) || !std::isfinite(line.width) || !std::isfinite(line.height) ||
            line.x < 0 || line.y < 0 || line.width <= 0 || line.height <= 0 || line.x + line.width > ocr.width + 1 || line.y + line.height > ocr.height + 1)
            throw std::runtime_error("Translation OCR line bounds are invalid.");
    }
    if (static_cast<unsigned long long>(source.bmWidth) * source.bmHeight > 64ULL * 1024 * 1024)
        throw std::runtime_error("Translation image exceeded the pixel limit.");
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = source.bmWidth; info.bmiHeader.biHeight = -source.bmHeight;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP result = CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    HDC sourceDc = CreateCompatibleDC(nullptr), targetDc = CreateCompatibleDC(nullptr);
    if (!result || !sourceDc || !targetDc) {
        if (result) DeleteObject(result); if (sourceDc) DeleteDC(sourceDc); if (targetDc) DeleteDC(targetDc);
        throw std::runtime_error("Cannot allocate translated image.");
    }
    const auto oldSource = SelectObject(sourceDc,original), oldTarget = SelectObject(targetDc,result);
    if (!BitBlt(targetDc,0,0,source.bmWidth,source.bmHeight,sourceDc,0,0,SRCCOPY)) {
        SelectObject(sourceDc,oldSource); SelectObject(targetDc,oldTarget); DeleteDC(sourceDc); DeleteDC(targetDc); DeleteObject(result);
        throw std::runtime_error("Cannot copy the translation image.");
    }
    SetBkMode(targetDc,TRANSPARENT); SetTextColor(targetDc,RGB(24,29,36));
    const double scaleX = static_cast<double>(source.bmWidth) / ocr.width;
    const double scaleY = static_cast<double>(source.bmHeight) / ocr.height;
    for (size_t i = 0; i < ocr.lines.size(); ++i) {
        const auto& line = ocr.lines[i];
        RECT rect{std::clamp(static_cast<LONG>(std::floor(line.x * scaleX)),0L,source.bmWidth - 1),
            std::clamp(static_cast<LONG>(std::floor(line.y * scaleY)),0L,source.bmHeight - 1),
            std::clamp(static_cast<LONG>(std::ceil((line.x + line.width) * scaleX)),1L,source.bmWidth),
            std::clamp(static_cast<LONG>(std::ceil((line.y + line.height) * scaleY)),1L,source.bmHeight)};
        // Keep replacement inside the OCR rectangle. Expanding to accommodate
        // longer translated prose would erase surrounding screenshot content.
        const LONG insetX = rect.right - rect.left >= 8 ? 2 : 0;
        const LONG insetY = rect.bottom - rect.top >= 8 ? 1 : 0;
        const int minimumFont = std::max(1,std::min(6,static_cast<int>(rect.bottom - rect.top)));
        int fontSize = std::clamp(static_cast<int>(line.height * scaleY * 0.78),minimumFont,72);
        HFONT font = nullptr; HGDIOBJ oldFont = nullptr;
        for (; fontSize >= minimumFont; --fontSize) {
            font = CreateFontW(-fontSize,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            if (!font) break;
            oldFont = SelectObject(targetDc,font);
            RECT measured{0,0,std::max(1L,rect.right - rect.left - 2 * insetX),0};
            DrawTextW(targetDc,translated[i].c_str(),static_cast<int>(translated[i].size()),&measured,DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            if (measured.bottom + 2 * insetY <= rect.bottom - rect.top || fontSize == minimumFont) break;
            SelectObject(targetDc,oldFont); DeleteObject(font); font = nullptr;
        }
        HBRUSH background = CreateSolidBrush(RGB(247,249,252)); FillRect(targetDc,&rect,background); DeleteObject(background);
        RECT textRect{rect.left + insetX,rect.top + insetY,rect.right - insetX,rect.bottom - insetY};
        DrawTextW(targetDc,translated[i].c_str(),static_cast<int>(translated[i].size()),&textRect,DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS);
        if (font) { SelectObject(targetDc,oldFont); DeleteObject(font); }
    }
    GdiFlush();
    // GDI text operations clear the alpha channel; exported PNGs must stay opaque.
    auto* colors = static_cast<unsigned*>(pixels);
    for (size_t i = 0, count = static_cast<size_t>(source.bmWidth) * source.bmHeight; i < count; ++i) colors[i] |= 0xff000000;
    SelectObject(sourceDc,oldSource); SelectObject(targetDc,oldTarget); DeleteDC(sourceDc); DeleteDC(targetDc);
    return result;
}
HBITMAP translateImage(HBITMAP borrowedSource, const std::filesystem::path& workerExe,
    const TranslationConfig& config, std::atomic_bool& cancel,
    const std::filesystem::path& tempDirectory) {
    checkCancelled(&cancel);
    BITMAP dimensions{};
    if (!borrowedSource || !GetObjectW(borrowedSource,sizeof(dimensions),&dimensions) || dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0)
        throw std::runtime_error("Translation source bitmap is invalid.");
    const auto pixels = static_cast<std::uint64_t>(dimensions.bmWidth) * dimensions.bmHeight;
    if (pixels > imagePixelBudget) throw std::runtime_error("Translation image exceeded the pixel limit; select a smaller region.");
    std::error_code directoryError;
    if (tempDirectory.empty() || !std::filesystem::is_directory(tempDirectory,directoryError) || directoryError)
        throw std::runtime_error("Translation temporary directory is unavailable.");
    // WIC copies the borrowed bitmap and converts it during PNG encoding. The
    // caller's bitmap reservations remain live, and this additional lease is
    // released before starting the isolated OCR process or network request.
    auto encodingMemory = image_tools_detail::reserveMemory(pixels * sizeof(DWORD) * 2);
    if (!encodingMemory) throw std::runtime_error("Translation PNG encoding exceeded the shared image memory budget.");
    GUID unique{}; wchar_t suffix[40]{};
    if (FAILED(CoCreateGuid(&unique)) || !StringFromGUID2(unique,suffix,40))
        throw std::runtime_error("Cannot create translation temporary image name.");
    TempOutput temporary;
    auto tempPath = tempDirectory / (std::wstring(L"desk-translate-") + suffix + L".png");
    {
        // Reserve a unique destination before WIC writes its adjacent staging
        // file. Both paths resolve within the caller's physical directory.
        Handle file(CreateFileW(tempPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr));
        if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create translation temporary image.");
        temporary.path = std::move(tempPath);
    }
    checkCancelled(&cancel);
    if (!saveBitmapPng(borrowedSource,temporary.path)) throw std::runtime_error("截图翻译临时图片保存失败，请检查目录权限和磁盘空间。");
    encodingMemory.reset();
    checkCancelled(&cancel);
    auto ocr = runOcr(workerExe,temporary.path,&cancel);
    // Screenshot files need not remain on disk during provider requests.
    std::error_code ignored;
    std::filesystem::remove(temporary.path,ignored);
    checkCancelled(&cancel);
    std::vector<std::wstring> sourceLines;
    OcrResult visible; visible.width = ocr.width; visible.height = ocr.height;
    for (auto& line : ocr.lines) {
        if (!hasText(line.text)) continue;
        if (line.width <= 0 || line.height <= 0) throw std::runtime_error("Translation OCR line bounds are invalid.");
        sourceLines.push_back(line.text); visible.lines.push_back(std::move(line));
    }
    if (sourceLines.empty()) throw std::runtime_error("选区中没有识别到可翻译文字，请重新选择包含文字的区域。");
    const auto translated = translateLines(sourceLines,config,&cancel);
    checkCancelled(&cancel);
    HBITMAP result = renderTranslation(borrowedSource,visible,translated);
    if (cancel.load()) { DeleteObject(result); throw std::runtime_error("Translation cancelled."); }
    return result;
}
}
