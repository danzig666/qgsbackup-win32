#pragma once
#include <filesystem>
#include <string>

namespace qgsbackup {
bool extractZipSafely(const std::filesystem::path& archive, const std::filesystem::path& destination, std::wstring& error);
bool createZipFromDirectory(const std::filesystem::path& directory, const std::filesystem::path& archive, std::wstring& error);
}
