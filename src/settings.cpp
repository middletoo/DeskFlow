#include "settings.hpp"
#include "common.hpp"
#include "json.hpp"
#include <fstream>
#include <algorithm>
namespace desk {
Settings loadSettings(const std::filesystem::path &path) {
    Settings s;
    if (!std::filesystem::exists(path))
        return s;
    if (std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("设置文件超过大小限制，原文件已保留");
    std::ifstream file(path);
    nlohmann::json j;
    file >> j;
    s.autoStart = j.value("autoStart", false);
    s.elevatedIndex = j.value("elevatedIndex", true);
    s.dark = j.value("dark", false);
    s.maximumEntryMiB = j.value("maximumEntryMiB", 32u);
    s.imageQuotaGiB = j.value("imageQuotaGiB", 5u);
    if (s.maximumEntryMiB < 1 || s.maximumEntryMiB > 128 || s.imageQuotaGiB < 1 ||
        s.imageQuotaGiB > 1024)
        throw std::runtime_error("存储预算超出范围，原设置文件已保留");
    s.excludedApps = wide(j.value("excludedApps", utf8(s.excludedApps)));
    auto &t = s.translation;
    t.provider = wide(j.value("provider", "google-free"));
    t.target = wide(j.value("target", "zh-CN"));
    t.libreUrl = wide(j.value("libreUrl", "http://localhost:5000"));
    t.proxy = wide(j.value("proxy", ""));
    t.deeplFree = j.value("deeplFree", true);
    t.deeplKey = unprotectSecret(j.value("deeplKeyProtected", ""));
    t.googleKey = unprotectSecret(j.value("googleKeyProtected", ""));
    if (j.contains("hotkeys") && j["hotkeys"].is_array() && j["hotkeys"].size() == 3)
        for (int i = 0; i < 3; i++) {
            auto key = j["hotkeys"][i].value("key", 0u),
                 mods = j["hotkeys"][i].value("modifiers", 0u);
            if (key == 0 || key > 254 || mods & ~(MOD_ALT | MOD_CONTROL | MOD_SHIFT))
                throw std::runtime_error("设置中的快捷键无效，原文件已保留");
            s.hotkeys[i] = {key, mods};
        }
    // Upgrade only the complete old default set; keep deliberate custom keys.
    if(s.hotkeys[0].key==VK_SPACE&&s.hotkeys[1].key=='V'&&s.hotkeys[2].key=='A'&&
       std::all_of(s.hotkeys.begin(),s.hotkeys.end(),[](auto key){return key.modifiers==(MOD_CONTROL|MOD_ALT);}))
        s.hotkeys={{{'Q',MOD_ALT},{'W',MOD_ALT},{'S',MOD_ALT}}};
    return s;
}
void saveSettings(const std::filesystem::path &path, const Settings &s) {
    nlohmann::json j = {{"version", 1},
                        {"autoStart", s.autoStart},
                        {"elevatedIndex", s.elevatedIndex},
                        {"dark", s.dark},
                        {"maximumEntryMiB", s.maximumEntryMiB},
                        {"imageQuotaGiB", s.imageQuotaGiB},
                        {"excludedApps", utf8(s.excludedApps)},
                        {"provider", utf8(s.translation.provider)},
                        {"target", utf8(s.translation.target)},
                        {"deeplFree", s.translation.deeplFree},
                        {"libreUrl", utf8(s.translation.libreUrl)},
                        {"proxy", utf8(s.translation.proxy)},
                        {"deeplKeyProtected", protectSecret(s.translation.deeplKey)},
                        {"googleKeyProtected", protectSecret(s.translation.googleKey)}};
    j["hotkeys"] = nlohmann::json::array();
    for (auto key : s.hotkeys)
        j["hotkeys"].push_back({{"key", key.key}, {"modifiers", key.modifiers}});
    auto temporary = path;
    temporary += L".pending";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << j.dump(2);
        file.flush();
        if (!file)
            throw std::runtime_error("设置保存失败");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("设置提交失败，旧设置仍保留");
}
std::wstring hotkeyText(Hotkey key) {
    std::wstring s;
    if (key.modifiers & MOD_CONTROL)
        s += L"Ctrl + ";
    if (key.modifiers & MOD_ALT)
        s += L"Alt + ";
    if (key.modifiers & MOD_SHIFT)
        s += L"Shift + ";
    wchar_t name[64]{};
    LONG scan = (LONG)MapVirtualKeyW(key.key, MAPVK_VK_TO_VSC) << 16;
    GetKeyNameTextW(scan, name, 64);
    return s + name;
}
} // namespace desk
