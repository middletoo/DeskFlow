#include "index_launch.hpp"
#include <winioctl.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static bool junction(const fs::path& link, const fs::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) { RemoveDirectoryW(link.c_str()); return false; }
    struct MountPoint {
        DWORD tag; WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
        wchar_t names[1];
    };
    auto substitute = L"\\??\\" + target.wstring(), print = target.wstring();
    std::vector<unsigned char> bytes(offsetof(MountPoint, names) +
                                     (substitute.size() + print.size() + 2) * sizeof(wchar_t));
    auto* point = reinterpret_cast<MountPoint*>(bytes.data());
    point->tag = IO_REPARSE_TAG_MOUNT_POINT;
    point->length = static_cast<WORD>(bytes.size() - 8);
    point->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    point->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    point->printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    memcpy(point->names, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    memcpy(reinterpret_cast<unsigned char*>(point->names) + point->printOffset,
           print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    DWORD returned = 0;
    bool ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, bytes.data(),
                             static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(directory);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok;
}
int main() {
    const auto base = fs::temp_directory_path() / (L"DeskFlow-index-launch-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto physical = base / L"physical", alias = base / L"redirected";
    fs::create_directories(physical);
    std::ofstream(physical / L"DeskIndex.exe", std::ios::binary) << "synthetic; never executed";
    int result = 0;
    try {
        require(junction(alias, physical), "could not create controlled install-directory redirect");
        {
            desk::IndexExecutable executable(alias / L"DeskIndex.exe");
            desk::IndexExecutable direct(physical / L"DeskIndex.exe");
            require(fs::equivalent(executable.path(), physical / L"DeskIndex.exe"), "wrong physical file");
            require(executable.path() == direct.path() &&
                    executable.path().parent_path().filename() == L"physical",
                    "logical redirect was retained");
            HANDLE write = CreateFileW((physical / L"DeskIndex.exe").c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
            require(write == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
                    "launch file can be modified during administrator confirmation");
            require(!DeleteFileW((physical / L"DeskIndex.exe").c_str()), "launch file was removed while locked");
        }
        bool failed = false;
        try { desk::IndexExecutable missing(physical / L"missing.exe"); }
        catch (const std::system_error&) { failed = true; }
        require(failed, "missing helper was silently accepted");
        require(desk::indexProcessElevation(nullptr) == desk::ProcessElevation::Unknown,
                "missing process was reported elevated");
        require(desk::indexProcessElevation(GetCurrentProcess()) != desk::ProcessElevation::Unknown,
                "current process elevation could not be checked");
        std::cout << "Physical launch path, read lock, missing file and token checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; result = 1;
    }
    // Remove the controlled junction itself before cleaning the owned fixture.
    RemoveDirectoryW(alias.c_str());
    fs::remove_all(base);
    return result;
}
