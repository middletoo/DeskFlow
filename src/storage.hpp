#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <optional>

namespace desk {
std::optional<std::filesystem::path> configuredDataDirectory();
void configureDataDirectory(const std::filesystem::path& directory);
std::filesystem::path physicalDirectory(const std::filesystem::path& directory);
struct StorageMigration {bool changed=false;uint64_t files=0,bytes=0;};
// Source remains intact. Target must be empty; optionally commit its selector
// after copying and validating the databases.
StorageMigration migrateDataDirectory(const std::filesystem::path& source,
    const std::filesystem::path& target,
    const std::function<void(uint64_t,uint64_t)>& progress={},bool commitSelector=false);
}
