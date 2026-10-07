#pragma once
#include <windows.h>
#include <objidl.h>
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
namespace desk {
HRESULT makeFileDataObject(const std::vector<std::wstring>& paths, IDataObject** output);
// All Shell/OLE functions are called on the host STA. Windows owns its normal
// confirmation and third-party commands; no selected file is opened during preview.
HRESULT copyFiles(const std::vector<std::wstring>& paths, bool cut = false);
HRESULT dragFiles(const std::vector<std::wstring>& paths);
HRESULT showFileContextMenu(HWND owner, const std::vector<std::wstring>& paths, POINT screen,
                            bool* renameRequested = nullptr);
HRESULT showFileProperties(HWND owner, const std::vector<std::wstring>& paths);
HRESULT renameFile(HWND owner, const std::wstring& path, const std::wstring& newName);
HRESULT recycleFiles(HWND owner, const std::vector<std::wstring>& paths);
HRESULT openFileLocation(HWND owner, const std::wstring& path);
struct FilePreview {
    HBITMAP bitmap = nullptr; // caller owns bitmap
    std::wstring text;
};
FilePreview loadFilePreview(const std::filesystem::path& path,
                           const std::atomic_int* version = nullptr, int expected = 0);
HBITMAP loadImageBitmap(const std::filesystem::path& path); // <=32MP, caller owns
std::wstring fileSizeLabel(uint64_t bytes);
}
