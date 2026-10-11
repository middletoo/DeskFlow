#pragma once
#include <Windows.h>
#include <filesystem>

namespace desk {
// Resolve the physical installed file and keep it read-locked through RunAs.
// WindowsApps can be reached through a redirected logical install directory.
class IndexExecutable {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::filesystem::path path_;

  public:
    explicit IndexExecutable(const std::filesystem::path& installedFile);
    ~IndexExecutable();
    IndexExecutable(const IndexExecutable&) = delete;
    IndexExecutable& operator=(const IndexExecutable&) = delete;
    const std::filesystem::path& path() const { return path_; }
};

enum class ProcessElevation { Unknown, Standard, Elevated };
ProcessElevation indexProcessElevation(HANDLE process);
}
