#pragma once
#include "backup_engine.h"
#include <filesystem>
#include <string>
#include <vector>

namespace qgsbackup {
std::wstring utf8ToWide(const std::string& value);
std::string wideToUtf8(const std::wstring& value);
std::filesystem::path executableDirectory();
std::wstring normalizedPathKey(const std::filesystem::path& path);
bool pathsEquivalent(const std::filesystem::path& a, const std::filesystem::path& b);
std::wstring timestampNow();
std::wstring md5Prefix(const std::filesystem::path& path);
std::vector<std::wstring> loadSkipPatterns(const std::filesystem::path& file);
bool matchesAnyPattern(const std::filesystem::path& path, const std::vector<std::wstring>& patterns);
bool copyFileStreaming(const std::filesystem::path& source, const std::filesystem::path& destination,
                       const BackupCallbacks& callbacks, std::uint64_t& bytesCopied);
std::filesystem::path makeUniqueTempDirectory();

class ScopedTempDirectory {
public:
    ScopedTempDirectory();
    ~ScopedTempDirectory();
    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};
}
