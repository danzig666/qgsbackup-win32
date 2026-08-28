#include "filesystem_utils.h"
#include "wildcard.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;
namespace qgsbackup {

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    UINT codePage = CP_UTF8; DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) { codePage = CP_ACP; flags = 0; n = MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()), nullptr, 0); }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}

fs::path executableDirectory() {
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(n);
    return fs::path(buffer).parent_path();
}

std::wstring normalizedPathKey(const fs::path& path) {
    std::error_code ec;
    auto p = fs::weakly_canonical(path, ec);
    if (ec) p = fs::absolute(path, ec).lexically_normal();
    std::wstring key = p.native();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return std::towlower(c); });
    return key;
}

bool pathsEquivalent(const fs::path& a, const fs::path& b) { return normalizedPathKey(a) == normalizedPathKey(b); }

std::wstring timestampNow() {
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t buf[32]{};
    swprintf_s(buf, L"%04u-%02u-%02u_%02u_%02u_%02u", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring md5Prefix(const fs::path& path) {
    const std::string bytes = wideToUtf8(path.native());
    BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE hash{};
    DWORD objectSize{}; ULONG got{};
    std::vector<UCHAR> object, digest(16);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("MD5 unavailable");
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &got, 0);
    object.resize(objectSize);
    if (BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); throw std::runtime_error("MD5 failed");
    }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    std::wostringstream ss; ss << std::hex << std::setfill(L'0');
    for (int i = 0; i < 4; ++i) ss << std::setw(2) << static_cast<unsigned>(digest[i]);
    return ss.str();
}

std::vector<std::wstring> loadSkipPatterns(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::string content((std::istreambuf_iterator<char>(in)), {});
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xef && static_cast<unsigned char>(content[1]) == 0xbb && static_cast<unsigned char>(content[2]) == 0xbf) content.erase(0, 3);
    std::vector<std::wstring> result; std::istringstream lines(content); std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto first = line.find_first_not_of(" \t"); auto last = line.find_last_not_of(" \t");
        if (first != std::string::npos) result.push_back(utf8ToWide(line.substr(first, last - first + 1)));
    }
    return result;
}

bool matchesAnyPattern(const fs::path& path, const std::vector<std::wstring>& patterns) {
    const auto name = path.filename().native();
    return std::any_of(patterns.begin(), patterns.end(), [&](const auto& p) { return wildcardMatchInsensitive(name, p); });
}

bool copyFileStreaming(const fs::path& source, const fs::path& destination,
                       const BackupCallbacks& callbacks, std::uint64_t& bytesCopied) {
    std::ifstream in(source, std::ios::binary); if (!in) return false;
    fs::create_directories(destination.parent_path());
    DWORD existingAttributes = GetFileAttributesW(destination.c_str());
    if (existingAttributes != INVALID_FILE_ATTRIBUTES && (existingAttributes & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(destination.c_str(), existingAttributes & ~FILE_ATTRIBUTE_READONLY);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc); if (!out) return false;
    std::vector<char> buffer(4 * 1024 * 1024);
    while (in) {
        if (callbacks.isCancelled && callbacks.isCancelled()) return false;
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto count = in.gcount();
        if (count > 0) { out.write(buffer.data(), count); if (!out) return false; bytesCopied += static_cast<std::uint64_t>(count); }
    }
    out.close();
    std::error_code ec; fs::last_write_time(destination, fs::last_write_time(source, ec), ec);
    DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(destination.c_str(), attributes);
    return true;
}

fs::path makeUniqueTempDirectory() {
    wchar_t temp[MAX_PATH + 1]{}; if (!GetTempPathW(MAX_PATH, temp)) throw std::runtime_error("GetTempPath failed");
    GUID id{}; if (CoCreateGuid(&id) != S_OK) throw std::runtime_error("CoCreateGuid failed");
    wchar_t guid[64]{}; StringFromGUID2(id, guid, 64);
    fs::path p = fs::path(temp) / L"QgsBackup" / guid;
    fs::create_directories(p); return p;
}

ScopedTempDirectory::ScopedTempDirectory() : path_(makeUniqueTempDirectory()) {}
ScopedTempDirectory::~ScopedTempDirectory() { std::error_code ec; fs::remove_all(path_, ec); }

} // namespace qgsbackup
