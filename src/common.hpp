#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>
#include <stdexcept>
#include "sqlite3.h"
namespace desk {
std::string utf8(const std::wstring &value);
std::wstring wide(const std::string &value);
std::wstring userError(const std::exception &error);
std::filesystem::path dataDirectory();
std::string sha256(const void *data, size_t size);
std::string protectSecret(const std::wstring &value);
std::wstring unprotectSecret(const std::string &value);
class Database {
  public:
    sqlite3 *handle = nullptr;
    explicit Database(const std::filesystem::path &path);
    ~Database();
    Database(const Database &) = delete;
    void exec(const char *sql);
};
class Statement {
  public:
    sqlite3_stmt *handle = nullptr;
    Statement(Database &db, const char *sql);
    ~Statement();
    void bind(int index, const std::string &value);
    void bind(int index, int64_t value);
    bool step();
    int64_t integer(int column) const;
    std::string text(int column) const;
};
} // namespace desk
