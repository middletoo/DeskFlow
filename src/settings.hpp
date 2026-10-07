#pragma once
#include "translation.hpp"
#include <filesystem>
#include <array>
namespace desk {
struct Hotkey {
    UINT key = 0, modifiers = MOD_CONTROL | MOD_ALT;
};
struct Settings {
    TranslationConfig translation;
    bool autoStart = false;
    bool elevatedIndex = false;
    bool dark = false;
    unsigned maximumEntryMiB = 32, imageQuotaGiB = 5;
    std::wstring excludedApps = L"KeePass.exe;1Password.exe;Bitwarden.exe";
    std::array<Hotkey, 3> hotkeys{{{'Q', MOD_ALT}, {'W', MOD_ALT}, {'S', MOD_ALT}}};
};
Settings loadSettings(const std::filesystem::path &);
void saveSettings(const std::filesystem::path &, const Settings &);
std::wstring hotkeyText(Hotkey);
} // namespace desk
