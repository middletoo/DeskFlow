#include "settings.hpp"
#include "common.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
int main() {
    auto path = std::filesystem::temp_directory_path() /
                (L"DeskFlow-settings-test-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    try {
        desk::Settings s;
        if(s.hotkeys[0].key!='Q'||s.hotkeys[1].key!='W'||s.hotkeys[2].key!='S'||s.hotkeys[0].modifiers!=MOD_ALT||s.hotkeys[1].modifiers!=MOD_ALT||s.hotkeys[2].modifiers!=MOD_ALT)throw std::runtime_error("Alt Q/W/S defaults incorrect");
        s.translation.deeplKey = L"test-secret-key:fx";
        s.hotkeys[0].key = 'F';
        s.elevatedIndex = true;
        s.maximumEntryMiB = 64;
        s.imageQuotaGiB = 20;
        desk::saveSettings(path, s);
        std::ifstream f(path);
        std::string data((std::istreambuf_iterator<char>(f)), {});
        f.close();
        if (data.find("test-secret-key") != std::string::npos)
            throw std::runtime_error("API key leaked in settings");
        auto read = desk::loadSettings(path);
        if (read.translation.deeplKey != s.translation.deeplKey)
            throw std::runtime_error("DPAPI round-trip failed");
        if (read.hotkeys[0].key != 'F')
            throw std::runtime_error("hotkey configuration not preserved");
        if (!read.elevatedIndex)
            throw std::runtime_error("administrator indexing preference must survive restart");
        if (read.maximumEntryMiB != 64 || read.imageQuotaGiB != 20)
            throw std::runtime_error("storage budgets not preserved");
        s.hotkeys={{{VK_SPACE,MOD_CONTROL|MOD_ALT},{'V',MOD_CONTROL|MOD_ALT},{'A',MOD_CONTROL|MOD_ALT}}};desk::saveSettings(path,s);auto upgraded=desk::loadSettings(path);
        if(upgraded.hotkeys[0].key!='Q'||upgraded.hotkeys[1].key!='W'||upgraded.hotkeys[2].key!='S'||upgraded.translation.deeplKey!=s.translation.deeplKey)throw std::runtime_error("old defaults must migrate without losing encrypted configuration");
        wchar_t message[] = L"磁盘空间不足";
        int n = WideCharToMultiByte(CP_ACP, 0, message, -1, nullptr, 0, nullptr, nullptr);
        std::string ansi(n, 0);
        WideCharToMultiByte(CP_ACP, 0, message, -1, ansi.data(), n, nullptr, nullptr);
        ansi.resize(n-1);
        if (desk::userError(std::runtime_error(ansi)) != message)
            throw std::runtime_error("localized Windows errors must not throw or lose their text");
        std::filesystem::remove(path);
        std::cout << "PASS settings, encrypted API keys and hotkeys\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << "\n";
        return 1;
    }
}
