#include "index_launch.hpp"
#include <system_error>
#include <vector>

namespace desk {
IndexExecutable::IndexExecutable(const std::filesystem::path& installedFile) {
    file_ = CreateFileW(installedFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "Open DeskIndex");
    try {
        auto flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
        DWORD length = GetFinalPathNameByHandleW(file_, nullptr, 0, flags);
        if (!length)
            throw std::system_error(GetLastError(), std::system_category(), "Resolve DeskIndex");
        std::vector<wchar_t> buffer(length + 1);
        DWORD copied = GetFinalPathNameByHandleW(file_, buffer.data(),
                                                static_cast<DWORD>(buffer.size()), flags);
        if (!copied || copied >= buffer.size())
            throw std::system_error(copied ? ERROR_INSUFFICIENT_BUFFER : GetLastError(),
                                    std::system_category(), "Resolve DeskIndex");
        std::wstring physical(buffer.data(), copied);
        // ShellExecute accepts the ordinary DOS/UNC form. Preserve extended
        // syntax only for paths that exceed the ordinary Win32 length limit.
        if (physical.size() < MAX_PATH) {
            if (physical.starts_with(L"\\\\?\\UNC\\"))
                physical = L"\\\\" + physical.substr(8);
            else if (physical.starts_with(L"\\\\?\\"))
                physical.erase(0, 4);
        }
        path_ = std::move(physical);
    } catch (...) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        throw;
    }
}
IndexExecutable::~IndexExecutable() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}
ProcessElevation indexProcessElevation(HANDLE process) {
    HANDLE token = nullptr;
    if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token))
        return ProcessElevation::Unknown;
    TOKEN_ELEVATION elevation{};
    DWORD length = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                       sizeof(elevation), &length) != FALSE;
    CloseHandle(token);
    if (!ok) return ProcessElevation::Unknown;
    return elevation.TokenIsElevated ? ProcessElevation::Elevated : ProcessElevation::Standard;
}
}
