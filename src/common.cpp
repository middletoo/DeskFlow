#include "common.hpp"
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <vector>
#include <array>
namespace desk {
std::string utf8(const std::wstring &value) {
    if (value.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(),
                                nullptr, 0, nullptr, nullptr);
    if (!n)
        throw std::runtime_error("Invalid Unicode text");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), n, nullptr,
                        nullptr);
    return out;
}
std::wstring wide(const std::string &value) {
    if (value.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(),
                                nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8 text");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), n);
    return out;
}
std::filesystem::path dataDirectory() {
    // Resolve the user's own storage; never use the project folder for live history.
    PWSTR path = nullptr;
    // Redirected Win32 writes may reside on another volume. Return the physical
    // target so rename and WinRT readers use the same existing history location.
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,
                                  KF_FLAG_RETURN_FILTER_REDIRECTION_TARGET, nullptr, &path)))
        throw std::runtime_error("LocalAppData unavailable");
    auto result = std::filesystem::path(path) / L"DeskFlow";
    CoTaskMemFree(path);
    std::filesystem::create_directories(result);
    return result;
}
std::string sha256(const void *data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, bytes = 0;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider unavailable");
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objectSize, sizeof(objectSize), &bytes,
                      0);
    std::vector<unsigned char> object(objectSize);
    auto status = BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0);
    if (status >= 0)
        status = BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0);
    if (status >= 0)
        status = BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0);
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0)
        throw std::runtime_error("SHA256 failed");
    const char *hex = "0123456789abcdef";
    std::string out;
    for (auto c : digest) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}
std::string protectSecret(const std::wstring &value) {
    if (value.empty())
        return {};
    DATA_BLOB input{(DWORD)(value.size() * 2), (BYTE *)value.data()}, out{};
    if (!CryptProtectData(&input, L"DeskFlow", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &out))
        throw std::runtime_error("Cannot encrypt API key");
    std::string encoded;
    const char *hex = "0123456789abcdef";
    for (DWORD i = 0; i < out.cbData; i++) {
        encoded += hex[out.pbData[i] >> 4];
        encoded += hex[out.pbData[i] & 15];
    }
    LocalFree(out.pbData);
    return encoded;
}
std::wstring unprotectSecret(const std::string &value) {
    if (value.empty())
        return {};
    if (value.size() % 2)
        return {};
    std::vector<BYTE> bytes;
    auto digit = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < value.size(); i += 2) {
        int a = digit(value[i]), b = digit(value[i + 1]);
        if (a < 0 || b < 0)
            return {};
        bytes.push_back((BYTE)((a << 4) | b));
    }
    DATA_BLOB input{(DWORD)bytes.size(), bytes.data()}, out{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out))
        return {};
    std::wstring result((wchar_t *)out.pbData, out.cbData / 2);
    LocalFree(out.pbData);
    return result;
}
Database::Database(const std::filesystem::path &path) {
    auto name = utf8(path.wstring());
    int rc = sqlite3_open_v2(name.c_str(), &handle,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string e = handle ? sqlite3_errmsg(handle) : "Open database failed";
        if (handle)
            sqlite3_close(handle);
        handle = nullptr;
        throw std::runtime_error(e);
    }
    sqlite3_busy_timeout(handle, 1500);
    try {
        exec("PRAGMA foreign_keys=ON;PRAGMA cache_size=-4096;PRAGMA mmap_size=0;PRAGMA "
             "journal_mode=WAL;PRAGMA synchronous=FULL;");
    } catch (...) {
        sqlite3_close(handle);
        handle = nullptr;
        throw;
    }
}
Database::~Database() {
    if (handle)
        sqlite3_close(handle);
}
std::wstring userError(const std::exception &error) {
    std::string message = error.what();
    if (message.size() > 8192)
        message.resize(8192);
    try {
        return wide(message);
    } catch (...) {
        int n = MultiByteToWideChar(CP_ACP, 0, message.data(), (int)message.size(), nullptr, 0);
        if (n > 0) {
            std::wstring out(n, 0);
            MultiByteToWideChar(CP_ACP, 0, message.data(), (int)message.size(), out.data(), n);
            return out;
        }
        return L"操作未完成，请检查存储空间和权限";
    }
}
void Database::exec(const char *sql) {
    char *e = nullptr;
    if (sqlite3_exec(handle, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        std::string s = e ? e : "SQL failed";
        sqlite3_free(e);
        throw std::runtime_error(s);
    }
}
Statement::Statement(Database &db, const char *sql) {
    if (sqlite3_prepare_v2(db.handle, sql, -1, &handle, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db.handle));
}
Statement::~Statement() {
    if (handle)
        sqlite3_finalize(handle);
}
void Statement::bind(int i, const std::string &v) {
    if (sqlite3_bind_text(handle, i, v.data(), (int)v.size(), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("SQL binding failed");
}
void Statement::bind(int i, int64_t v) {
    if (sqlite3_bind_int64(handle, i, v) != SQLITE_OK)
        throw std::runtime_error("SQL binding failed");
}
bool Statement::step() {
    int r = sqlite3_step(handle);
    if (r == SQLITE_ROW)
        return true;
    if (r == SQLITE_DONE)
        return false;
    throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(handle)));
}
int64_t Statement::integer(int c) const {
    return sqlite3_column_int64(handle, c);
}
std::string Statement::text(int c) const {
    auto p = sqlite3_column_text(handle, c);
    return p ? std::string((const char *)p, sqlite3_column_bytes(handle, c)) : std::string();
}
} // namespace desk
