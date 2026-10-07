#include <Windows.h>
#include <shlobj.h>
#include <appmodel.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "json.hpp"

using Json = nlohmann::json;
static std::string utf8(const std::wstring& value) {
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    std::string result(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), n, nullptr, nullptr);
    return result;
}
static std::wstring known(DWORD flags, HANDLE token = nullptr) {
    PWSTR path = nullptr;
    HRESULT status = SHGetKnownFolderPath(FOLDERID_LocalAppData, flags, token, &path);
    if (FAILED(status)) return L"HRESULT:" + std::to_wstring((DWORD)status);
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}
static std::wstring packageName(bool family) {
    UINT32 length = 0;
    LONG status = family ? GetCurrentPackageFamilyName(&length, nullptr) : GetCurrentPackageFullName(&length, nullptr);
    if (status != ERROR_INSUFFICIENT_BUFFER) return L"ERROR:" + std::to_wstring(status);
    std::wstring result(length, 0);
    status = family ? GetCurrentPackageFamilyName(&length, result.data()) : GetCurrentPackageFullName(&length, result.data());
    if (status != ERROR_SUCCESS) return L"ERROR:" + std::to_wstring(status);
    result.resize(length - 1);
    return result;
}
static Json probe(const std::filesystem::path& directory, const char* name, DWORD flags) {
    Json result{{"name", name}, {"directory", utf8(directory.wstring())}, {"moveFlags", flags}};
    try {
        std::filesystem::create_directories(directory);
        const auto temporary = directory / (std::wstring(name, name + strlen(name)) + L".pending");
        const auto object = directory / (std::wstring(name, name + strlen(name)) + L".object");
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE) { result["createError"] = GetLastError(); return result; }
        wchar_t physical[32768]{};
        DWORD physicalLength = GetFinalPathNameByHandleW(file, physical, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        result["physicalTemporary"] = physicalLength && physicalLength < 32768 ? utf8(physical) : "unavailable";
        const char synthetic[] = "DeskFlow isolated synthetic storage probe; no clipboard data";
        DWORD written = 0;
        bool flushed = WriteFile(file, synthetic, sizeof(synthetic), &written, nullptr) &&
                       written == sizeof(synthetic) && FlushFileBuffers(file);
        result["writeComplete"] = flushed;
        CloseHandle(file);
        result["logicalTemporaryAttributes"] = GetFileAttributesW(temporary.c_str());
        SetLastError(ERROR_SUCCESS);
        BOOL moved = MoveFileExW(temporary.c_str(), object.c_str(), flags);
        DWORD moveError = moved ? ERROR_SUCCESS : GetLastError();
        result["moveSucceeded"] = moved != FALSE;
        result["moveError"] = moveError;
        if (!moved && physicalLength && physicalLength < 32768) {
            const auto actualSource = std::filesystem::path(physical);
            const auto actualTarget = actualSource.parent_path() / (std::wstring(name, name + strlen(name)) + L".physical-object");
            SetLastError(ERROR_SUCCESS);
            BOOL direct = MoveFileExW(actualSource.c_str(), actualTarget.c_str(), flags);
            result["physicalMoveSucceeded"] = direct != FALSE;
            result["physicalMoveError"] = direct ? ERROR_SUCCESS : GetLastError();
        }
    } catch (const std::exception& error) { result["exception"] = error.what(); }
    return result;
}
static int runProbe(const std::filesystem::path& output) {
    Json report{{"pid", GetCurrentProcessId()}, {"packageFullName", utf8(packageName(false))},
                {"packageFamilyName", utf8(packageName(true))}, {"localAppDataDefault", utf8(known(0))},
                {"localAppDataForceRedirect", utf8(known(KF_FLAG_FORCE_APP_DATA_REDIRECTION))},
                {"localAppDataFilterTarget", utf8(known(KF_FLAG_RETURN_FILTER_REDIRECTION_TARGET))},
                {"localAppDataNoPackageRedirect", utf8(known(KF_FLAG_NO_PACKAGE_REDIRECTION))}};
    const auto suffix = L"DeskFlow-StorageProbe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto logical = std::filesystem::path(known(0)) / suffix;
    Json probes = Json::array();
    probes.push_back(probe(logical, "logical-write-through", MOVEFILE_WRITE_THROUGH));
    probes.push_back(probe(logical, "logical-zero-flags", 0));
    probes.push_back(probe(std::filesystem::path(known(KF_FLAG_FORCE_APP_DATA_REDIRECTION)) / suffix,
                           "forced-folder-write-through", MOVEFILE_WRITE_THROUGH));
    probes.push_back(probe(std::filesystem::path(known(KF_FLAG_RETURN_FILTER_REDIRECTION_TARGET)) / suffix,
                           "filter-target-write-through", MOVEFILE_WRITE_THROUGH));
    HANDLE host = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 41056), token = nullptr;
    if (host && OpenProcessToken(host, TOKEN_QUERY | TOKEN_IMPERSONATE, &token)) {
        report["hostTokenDefaultFolder"] = utf8(known(0, token));
        report["hostTokenFilterTarget"] = utf8(known(KF_FLAG_RETURN_FILTER_REDIRECTION_TARGET, token));
        auto folder = known(KF_FLAG_RETURN_FILTER_REDIRECTION_TARGET, token);
        if (folder.rfind(L"HRESULT:", 0) != 0)
            probes.push_back(probe(std::filesystem::path(folder) / suffix, "host-token-filter-target", MOVEFILE_WRITE_THROUGH));
        CloseHandle(token);
    }
    if (host) CloseHandle(host);
    auto explicitDeskFlow = std::filesystem::path(known(KF_FLAG_NO_PACKAGE_REDIRECTION)) / L"Packages" /
        L"DeskFlow.Desktop_2j779qedymw2p" / L"LocalCache" / L"Local" / suffix;
    probes.push_back(probe(explicitDeskFlow, "explicit-deskflow-cache", MOVEFILE_WRITE_THROUGH));
    const auto family = packageName(true);
    if (family.rfind(L"ERROR:", 0) != 0) {
        auto direct = std::filesystem::path(known(KF_FLAG_NO_PACKAGE_REDIRECTION)) / L"Packages" / family /
                      L"LocalCache" / L"Local" / suffix;
        probes.push_back(probe(direct, "explicit-package-cache", MOVEFILE_WRITE_THROUGH));
    }
    report["probes"] = std::move(probes);
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file << report.dump(2);
    file.close();
    std::cout << report.dump(2) << '\n';
    return file.good() ? 0 : 1;
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc == 3 && std::wstring(argv[1]) == L"--probe") return runProbe(argv[2]);
    if (argc != 5 || std::wstring(argv[1]) != L"--parent-pid" || std::wstring(argv[3]) != L"--output") return 64;
    DWORD pid = std::stoul(argv[2]);
    HANDLE parent = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, pid);
    if (!parent) { std::cerr << "OpenProcess error=" << GetLastError() << '\n'; return 1; }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 2, 0, &bytes)) return 2;
    DWORD policy = PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_DISABLE_PROCESS_TREE;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof(parent), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY, &policy, sizeof(policy), nullptr, nullptr)) {
        std::cerr << "UpdateProcThreadAttribute error=" << GetLastError() << '\n'; return 3;
    }
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --probe \"" + argv[4] + L"\"";
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &process);
    DWORD error = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(parent);
    if (!created) { std::cerr << "CreateProcess error=" << error << '\n'; return 4; }
    CloseHandle(process.hThread);
    DWORD wait = WaitForSingleObject(process.hProcess, 15000), code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
