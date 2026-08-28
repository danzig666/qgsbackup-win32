#include "zip_utils.h"
#include "filesystem_utils.h"
#include "miniz.h"
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;
namespace qgsbackup {

static size_t writeFileCallback(void* opaque, mz_uint64 offset, const void* buffer, size_t count) {
    FILE* file = static_cast<FILE*>(opaque);
    if (_fseeki64(file, static_cast<__int64>(offset), SEEK_SET)) return 0;
    return fwrite(buffer, 1, count, file);
}

static bool safeArchiveName(const fs::path& relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) return false;
    for (const auto& part : relative) if (part == L"..") return false;
    return true;
}

bool extractZipSafely(const fs::path& archive, const fs::path& destination, std::wstring& error) {
    FILE* input{}; if (_wfopen_s(&input, archive.c_str(), L"rb")) { error = L"Unable to open QGZ: " + archive.native(); return false; }
    _fseeki64(input, 0, SEEK_END); auto size = static_cast<mz_uint64>(_ftelli64(input)); _fseeki64(input, 0, SEEK_SET);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, input, size, 0)) { fclose(input); error = L"Invalid QGZ archive: " + archive.native(); return false; }
    bool ok = true;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip) && ok; ++i) {
        mz_zip_archive_file_stat stat{}; if (!mz_zip_reader_file_stat(&zip, i, &stat)) { ok = false; break; }
        fs::path relative = fs::u8path(stat.m_filename).lexically_normal();
        if (!safeArchiveName(relative)) { error = L"Unsafe path in QGZ: " + relative.native(); ok = false; break; }
        fs::path output = destination / relative;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) { std::error_code ec; fs::create_directories(output, ec); if (ec) ok = false; continue; }
        std::error_code ec; fs::create_directories(output.parent_path(), ec); if (ec) { ok = false; break; }
        FILE* target{}; if (_wfopen_s(&target, output.c_str(), L"wb")) { ok = false; break; }
        ok = mz_zip_reader_extract_to_callback(&zip, i, writeFileCallback, target, 0) != 0;
        fclose(target);
    }
    if (!ok && error.empty()) error = L"Unable to extract QGZ archive: " + archive.native();
    mz_zip_reader_end(&zip); fclose(input); return ok;
}

bool createZipFromDirectory(const fs::path& directory, const fs::path& archive, std::wstring& error) {
    fs::path temporary = archive; temporary += L".tmp";
    FILE* output{}; if (_wfopen_s(&output, temporary.c_str(), L"w+b")) { error = L"Unable to create QGZ: " + temporary.native(); return false; }
    mz_zip_archive zip{}; bool ok = mz_zip_writer_init_cfile(&zip, output, 0) != 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(directory, ec), end; ok && !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        auto relative = fs::relative(it->path(), directory, ec); if (ec) { ok = false; break; }
        std::string archiveName = wideToUtf8(relative.generic_wstring());
        FILE* input{}; if (_wfopen_s(&input, it->path().c_str(), L"rb")) { ok = false; break; }
        auto fileSize = static_cast<mz_uint64>(it->file_size(ec));
        ok = !ec && mz_zip_writer_add_cfile(&zip, archiveName.c_str(), input, fileSize, nullptr, nullptr, 0, MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0) != 0;
        fclose(input);
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&zip) != 0;
    mz_zip_writer_end(&zip); fclose(output);
    if (!ok || ec) { fs::remove(temporary, ec); error = L"Unable to write QGZ: " + archive.native(); return false; }
    fs::remove(archive, ec); ec.clear(); fs::rename(temporary, archive, ec);
    if (ec) { fs::remove(temporary); error = L"Unable to commit QGZ: " + archive.native(); return false; }
    return true;
}

} // namespace qgsbackup
