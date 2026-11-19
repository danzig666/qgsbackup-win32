#include "backup_engine.h"
#include "filesystem_utils.h"
#include "qgis_project.h"
#include "zip_utils.h"
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
namespace qgsbackup {

namespace {
struct RunState {
    const BackupOptions& options;
    const BackupCallbacks& callbacks;
    BackupResult result;
    std::vector<std::wstring> skipPatterns;
    fs::path layers;
    std::wstring destinationKey;
    std::unordered_map<std::wstring, std::wstring> seen;        // source key -> "layers/..." reference
    std::unordered_map<std::wstring, std::wstring> names;       // output name key -> owning source/parent key
    std::unordered_map<std::wstring, std::wstring> copiedFiles; // source file key -> target file key

    void log(LogLevel level, const std::wstring& message) {
        if (level == LogLevel::Verbose && !options.verbose) return;
        if (level == LogLevel::Warning) ++result.warnings;
        if (callbacks.onLog) callbacks.onLog(level, message);
    }
    bool cancelled() const { return callbacks.isCancelled && callbacks.isCancelled(); }
    void progress(BackupStage stage, std::size_t current = 0, std::size_t total = 0, const std::wstring& source = {}) {
        if (callbacks.onProgress) callbacks.onProgress({current, total, result.bytesCopied, source, stage});
    }
};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); }); return value;
}

bool copyOne(RunState& state, const fs::path& source, const fs::path& destination) {
    std::uint64_t before = state.result.bytesCopied;
    if (!copyFileStreaming(source, destination, state.callbacks, state.result.bytesCopied)) {
        if (state.cancelled()) return false;
        state.log(LogLevel::Warning, L"Unable to copy:\r\n" + source.native() + L"\r\nto:\r\n" + destination.native());
        return false;
    }
    ++state.result.filesCopied;
    state.log(LogLevel::Verbose, L"Copied " + source.native() + L" (" + humanReadableSize(state.result.bytesCopied - before) + L")");
    return true;
}

// Copies a file once per run, even when several datasources share sidecars.
bool copyOnce(RunState& state, const fs::path& source, const fs::path& target) {
    const auto sourceKey = normalizedPathKey(source), targetKey = normalizedPathKey(target);
    auto previous = state.copiedFiles.find(sourceKey);
    if (previous != state.copiedFiles.end() && previous->second == targetKey) return true;
    if (!copyOne(state, source, target)) return false;
    state.copiedFiles[sourceKey] = targetKey; return true;
}

// Sidecars either share the datasource's basename (roads.dbf, roads.prj for roads.shp) or extend
// its complete filename (photo.tif.aux.xml, photo.tif.ovr, roads.shp.xml). Returns the sidecar's
// output name for the given datasource output name, or nothing when the file is unrelated.
std::optional<std::wstring> associatedTargetName(const fs::path& candidate, const fs::path& source, const fs::path& destinationName) {
    if (lower(candidate.stem().native()) == lower(source.stem().native()))
        return destinationName.stem().native() + candidate.extension().native();
    const auto name = candidate.filename().native(), prefix = source.filename().native() + L".";
    if (name.size() > prefix.size() && lower(name.substr(0, prefix.size())) == lower(prefix))
        return destinationName.native() + name.substr(source.filename().native().size());
    return std::nullopt;
}

std::optional<std::wstring> copyAssociatedFiles(RunState& state, const fs::path& source,
                                                const fs::path& layers, const fs::path& destinationName) {
    std::error_code ec; fs::create_directories(layers, ec);
    if (ec) { state.log(LogLevel::Warning, L"Unable to create layers folder: " + layers.native()); return std::nullopt; }
    bool primaryCopied = false, allCopied = true;
    for (fs::directory_iterator it(source.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        auto targetName = associatedTargetName(it->path(), source, destinationName);
        if (!targetName) continue;
        if (!pathsEquivalent(it->path(), source) && matchesAnyPattern(it->path(), state.skipPatterns)) {
            state.log(LogLevel::Verbose, L"Skipped associated file by pattern: " + it->path().native()); continue;
        }
        bool copied = copyOnce(state, it->path(), layers / *targetName);
        if (pathsEquivalent(it->path(), source)) primaryCopied = copied;
        if (!copied) allCopied = false;
        if (!copied && state.cancelled()) return std::nullopt;
    }
    if (ec) state.log(LogLevel::Warning, L"Unable to enumerate associated files for: " + source.native());
    return primaryCopied && allCopied ? std::optional<std::wstring>((fs::path(L"layers") / destinationName).generic_wstring()) : std::nullopt;
}

std::optional<std::wstring> copyDirectory(RunState& state, const fs::path& source,
                                          const fs::path& layers, const fs::path& destinationName) {
    // An existing folder from an earlier backup into the same destination is refreshed, not reused as is.
    fs::path targetRoot = layers / destinationName; std::error_code ec;
    fs::create_directories(targetRoot, ec);
    if (ec) { state.log(LogLevel::Warning, L"Unable to create directory: " + targetRoot.native()); return std::nullopt; }
    // When the backup is written inside the datasource folder, never copy the backup into itself.
    const auto sourceKey = normalizedPathKey(source);
    const bool destinationInside = state.destinationKey.size() > sourceKey.size() &&
        state.destinationKey.compare(0, sourceKey.size(), sourceKey) == 0 &&
        (state.destinationKey[sourceKey.size()] == L'\\' || state.destinationKey[sourceKey.size()] == L'/');
    for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (state.cancelled()) return std::nullopt;
        if (destinationInside && it->is_directory(ec) && normalizedPathKey(it->path()) == state.destinationKey) {
            state.log(LogLevel::Verbose, L"Not copying the backup destination into itself: " + it->path().native());
            it.disable_recursion_pending(); continue;
        }
        auto relative = fs::relative(it->path(), source, ec); if (ec) break;
        auto target = targetRoot / relative;
        if (it->is_directory(ec)) fs::create_directories(target, ec);
        else if (it->is_regular_file(ec) && !copyOne(state, it->path(), target) && state.cancelled()) return std::nullopt;
        if (ec) break;
    }
    if (ec) { state.log(LogLevel::Warning, L"Directory copy was incomplete: " + source.native() + L" (" + utf8ToWide(ec.message()) + L")"); return std::nullopt; }
    return (fs::path(L"layers") / destinationName).generic_wstring();
}

// QGIS stores files attached to a .qgs project in "<project basename>_attachments.zip" beside it.
// The archive is part of the project itself, so it is always copied, even if a skip pattern matches.
bool isProjectAttachmentsArchive(const fs::path& file, const fs::path& project) {
    return lower(file.filename().native()) == lower(project.stem().native() + L"_attachments.zip");
}

bool copyProjectAssociated(RunState& state, const fs::path& project, const fs::path& destination) {
    std::error_code ec;
    for (fs::directory_iterator it(project.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || pathsEquivalent(it->path(), project)) continue;
        const bool attachments = isProjectAttachmentsArchive(it->path(), project);
        if (!attachments && lower(it->path().stem().native()) != lower(project.stem().native())) continue;
        if (!attachments && matchesAnyPattern(it->path(), state.skipPatterns)) { state.log(LogLevel::Verbose, L"Skipped project-associated file: " + it->path().native()); continue; }
        if (!copyOne(state, it->path(), destination / it->path().filename()) && state.cancelled()) return false;
    }
    if (ec) state.log(LogLevel::Warning, L"Unable to enumerate project-associated files: " + utf8ToWide(ec.message()));
    return !state.cancelled();
}

std::wstring uniqueName(const fs::path& source) {
    return source.stem().native() + L"_" + md5Prefix(source) + source.extension().native();
}

// Copies one existing local datasource (file plus sidecars, or a directory) into layers/ and
// returns its project reference ("layers/..."), or nothing if it could not be copied completely.
std::optional<std::wstring> backupLocal(RunState& state, const fs::path& source) {
    std::error_code ec;
    const auto key = normalizedPathKey(source);
    if (auto existing = state.seen.find(key); existing != state.seen.end()) return existing->second;
    const bool directory = fs::is_directory(source, ec);
    if (!directory && !fs::is_regular_file(source, ec)) return std::nullopt;
    // Files are written as <stem>.* together with their sidecars, so files from different folders
    // that share a basename (roads.shp, roads.gpkg) must not share an output stem. Files from the
    // same folder share their sidecars and can safely share it.
    fs::path destinationName = source.filename();
    auto nameKeyOf = [&](const fs::path& name) { return directory ? L"d:" + lower(name.native()) : L"f:" + lower(name.stem().native()); };
    const auto owner = directory ? key : normalizedPathKey(source.parent_path());
    std::wstring nameKey = nameKeyOf(destinationName);
    auto collision = state.names.find(nameKey);
    if (collision != state.names.end() && collision->second != owner) {
        destinationName = uniqueName(source); nameKey = nameKeyOf(destinationName);
        state.log(LogLevel::Verbose, L"Filename collision: " + source.filename().native() + L" -> " + destinationName.native());
    }
    state.names[nameKey] = owner;
    auto relative = directory ? copyDirectory(state, source, state.layers, destinationName)
                              : copyAssociatedFiles(state, source, state.layers, destinationName);
    if (relative) state.seen[key] = *relative;
    return relative;
}

bool isSupportedProject(const fs::path& p) {
    auto ext = lower(p.extension().native()); return ext == L".qgs" || ext == L".qgz";
}

bool looksLikeRemoteProvider(const std::wstring& value) {
    auto v = lower(value);
    return v.find(L"url=http:") != std::wstring::npos || v.find(L"url=https:") != std::wstring::npos ||
           v.find(L"type=xyz") != std::wstring::npos || v.find(L"type=wms") != std::wstring::npos ||
           v.rfind(L"wms:", 0) == 0 || v.rfind(L"wfs:", 0) == 0 || v.rfind(L"xyz:", 0) == 0;
}

bool hasConnectionKey(const std::wstring& value, const wchar_t* key) {
    const auto v = lower(value);
    return v.rfind(key, 0) == 0 || v.find(std::wstring(L" ") + key) != std::wstring::npos;
}

// QgsDataSourceUri-style provider strings: dbname='...' host=... table="..." (geom), url='...' typename=...
bool looksLikeConnectionString(const std::wstring& value) {
    for (const wchar_t* key : {L"dbname=", L"service=", L"host=", L"url=", L"table=", L"authcfg=", L"typename=", L"layers=", L"contextualwmslegend="})
        if (hasConnectionKey(value, key)) return true;
    return false;
}

// Location of the dbname value inside a connection string, unescaped (QgsDataSourceUri escapes \ and ').
struct DbNameValue { std::size_t begin{}, end{}; bool quoted{}; std::wstring value; };

std::optional<DbNameValue> findDbName(const std::wstring& connection) {
    const auto v = lower(connection);
    std::size_t key = v.rfind(L"dbname=", 0) == 0 ? 0 : v.find(L" dbname=");
    if (key == std::wstring::npos) return std::nullopt;
    if (key) ++key;
    DbNameValue result; result.begin = key + 7;
    if (result.begin < connection.size() && connection[result.begin] == L'\'') {
        result.quoted = true; ++result.begin;
        for (std::size_t i = result.begin; i < connection.size(); ++i) {
            if (connection[i] == L'\\' && i + 1 < connection.size()) { result.value.push_back(connection[++i]); continue; }
            if (connection[i] == L'\'') { result.end = i; return result; }
            result.value.push_back(connection[i]);
        }
        return std::nullopt;
    }
    result.end = connection.find(L' ', result.begin); if (result.end == std::wstring::npos) result.end = connection.size();
    result.value = connection.substr(result.begin, result.end - result.begin);
    return result;
}

std::wstring replaceDbName(const std::wstring& connection, const DbNameValue& db, const std::wstring& value) {
    std::wstring escaped;
    for (wchar_t c : value) { if (c == L'\\' || c == L'\'') escaped.push_back(L'\\'); escaped.push_back(c); }
    if (!db.quoted) escaped = L"'" + escaped + L"'";
    return connection.substr(0, db.begin) + escaped + connection.substr(db.end);
}

bool isVirtualSource(const std::wstring& value) {
    auto v = lower(value);
    return v.rfind(L"memory?", 0) == 0 || v.rfind(L"?query", 0) == 0;
}

bool isEmbeddedSource(const std::wstring& value) {
    auto v = lower(value);
    return v.rfind(L"base64:", 0) == 0 || v.rfind(L"data:", 0) == 0;
}

std::wstring progressDescription(const DataSourceReference& ref) {
    if (isEmbeddedSource(ref.base)) return L"embedded data";
    if (isVirtualSource(ref.original)) return L"virtual layer";
    if (hasNonFileScheme(ref.base)) return L"remote resource";
    if (looksLikeRemoteProvider(ref.original)) return L"remote layer";
    if (looksLikeConnectionString(ref.original)) return L"database or service layer";
    auto name = fs::path(ref.base).filename().native();
    return name.empty() ? L"local resource" : name;
}

std::wstring abbreviated(const std::wstring& value, std::size_t limit = 180) {
    return value.size() <= limit ? value : value.substr(0, limit) + L"...";
}

std::optional<fs::path> chooseQgs(const fs::path& extracted, const fs::path& archive, std::wstring& error) {
    std::vector<fs::path> candidates; std::error_code ec;
    for (fs::recursive_directory_iterator it(extracted, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && lower(it->path().extension().native()) == L".qgs") candidates.push_back(it->path());
    if (candidates.empty()) { error = L"No .qgs project found inside QGZ."; return std::nullopt; }
    if (candidates.size() == 1) return candidates.front();
    std::vector<fs::path> matching;
    for (const auto& p : candidates) if (lower(p.stem().native()) == lower(archive.stem().native())) matching.push_back(p);
    if (matching.size() == 1) return matching.front();
    error = L"The QGZ contains multiple .qgs files and the logical project is ambiguous."; return std::nullopt;
}
}

std::wstring humanReadableSize(std::uint64_t bytes) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double size = static_cast<double>(bytes); int unit = 0; while (size >= 1024.0 && unit < 5) { size /= 1024.0; ++unit; }
    std::wostringstream out; out << std::fixed << std::setprecision(2) << size << L" " << units[unit]; return out.str();
}

const wchar_t* stageName(BackupStage stage) {
    switch (stage) {
    case BackupStage::OpeningProject: return L"Opening project"; case BackupStage::ExtractingQgz: return L"Extracting QGZ";
    case BackupStage::ReadingProject: return L"Reading QGIS project"; case BackupStage::ScanningResources: return L"Scanning resources";
    case BackupStage::CopyingFiles: return L"Copying files"; case BackupStage::UpdatingProject: return L"Updating project";
    case BackupStage::CreatingQgz: return L"Creating QGZ"; case BackupStage::Finishing: return L"Finishing";
    } return L"Working";
}

BackupResult runBackup(const BackupOptions& options, const BackupCallbacks& callbacks) {
    const auto started = std::chrono::steady_clock::now(); RunState state{options, callbacks};
    state.result.error = BackupError::Unknown;
    auto finish = [&](BackupStatus status, BackupError error, const std::wstring& message) {
        state.result.status = status; state.result.error = error; state.result.message = message;
        state.result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        return state.result;
    };
    try {
        state.progress(BackupStage::OpeningProject);
        std::error_code ec; fs::path project = fs::absolute(options.projectFile, ec).lexically_normal();
        if (ec || !fs::is_regular_file(project, ec) || !isSupportedProject(project))
            return finish(BackupStatus::Failure, BackupError::InvalidProject, L"The selected project must be an existing .qgs or .qgz file.\r\n" + project.native());
        fs::path destination = fs::absolute(options.destination, ec).lexically_normal();
        if (ec || destination.empty()) return finish(BackupStatus::Failure, BackupError::InvalidDestination, L"The backup destination is invalid.");
        if (options.timestamped) destination /= project.stem().native() + L"_" + timestampNow();
        fs::create_directories(destination, ec);
        if (ec || !fs::is_directory(destination, ec)) return finish(BackupStatus::Failure, BackupError::InvalidDestination, L"Unable to create destination:\r\n" + destination.native());
        if (pathsEquivalent(project, destination / project.filename()))
            return finish(BackupStatus::Failure, BackupError::InvalidDestination, L"The destination would overwrite the original project. Choose another folder.");
        state.result.destination = destination;
        state.log(LogLevel::Info, L"Project: " + project.native()); state.log(LogLevel::Info, L"Destination: " + destination.native());
        if (options.useSkipFile) {
            state.skipPatterns = loadSkipPatterns(options.skipFile);
            state.log(LogLevel::Info, options.skipFile.empty() || !fs::exists(options.skipFile) ? L"Skip file not found; continuing with zero patterns." :
                      L"Loaded " + std::to_wstring(state.skipPatterns.size()) + L" skip patterns from " + options.skipFile.native());
        } else state.log(LogLevel::Verbose, L"Skip file disabled.");

        const bool qgz = lower(project.extension().native()) == L".qgz";
        std::unique_ptr<ScopedTempDirectory> temporary; fs::path qgsFile = project;
        if (qgz) {
            state.progress(BackupStage::ExtractingQgz); temporary = std::make_unique<ScopedTempDirectory>(); std::wstring error;
            if (!extractZipSafely(project, temporary->path(), error)) return finish(BackupStatus::Failure, BackupError::ZipReadFailure, error);
            auto chosen = chooseQgs(temporary->path(), project, error); if (!chosen) return finish(BackupStatus::Failure, BackupError::ZipReadFailure, error); qgsFile = *chosen;
        }
        if (!copyProjectAssociated(state, project, destination)) return finish(BackupStatus::Cancelled, BackupError::Cancelled, L"Backup cancelled.");

        state.progress(BackupStage::ReadingProject); QgisProject xml; std::wstring xmlError;
        if (!xml.load(qgsFile, xmlError)) return finish(BackupStatus::Failure, BackupError::XmlReadFailure, xmlError);
        auto refs = xml.references(); state.log(LogLevel::Info, L"Found " + std::to_wstring(refs.size()) + L" unique datasource references.");
        state.progress(BackupStage::ScanningResources, 0, refs.size());

        std::unordered_map<std::wstring, std::wstring> replacements;
        std::size_t remotePreserved = 0, embeddedPreserved = 0, virtualPreserved = 0;
        fs::path projectDirectory = project.parent_path(); state.layers = destination / L"layers";
        state.destinationKey = normalizedPathKey(destination);
        auto resolveLocal = [&](const fs::path& p) { return p.is_absolute() ? p : (projectDirectory / p).lexically_normal(); };
        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (state.cancelled()) return finish(BackupStatus::Cancelled, BackupError::Cancelled, L"Backup cancelled.");
            const auto& ref = refs[i]; state.progress(BackupStage::CopyingFiles, i + 1, refs.size(), progressDescription(ref));
            if (isVirtualSource(ref.original)) { ++virtualPreserved; state.log(LogLevel::Verbose, L"Left virtual layer unchanged."); continue; }
            const bool fileUri = isFileUri(ref.base);
            if (isEmbeddedSource(ref.base)) {
                ++embeddedPreserved;
                state.log(LogLevel::Verbose, L"Left embedded resource unchanged (" + std::to_wstring(ref.original.size()) + L" characters).");
                continue;
            }
            if (hasNonFileScheme(ref.base) || looksLikeRemoteProvider(ref.original)) {
                ++remotePreserved;
                state.log(LogLevel::Verbose, L"Left remote datasource unchanged: " + abbreviated(ref.original));
                continue;
            }
            fs::path source = fileUri ? fileUriToPath(ref.base) : fs::path(ref.base);
            if (source.empty()) continue;
            source = resolveLocal(source);
            if (!fileUri && !fs::exists(source, ec) && looksLikeConnectionString(ref.original)) {
                // Database/service connection. A local database file named by dbname (SpatiaLite)
                // is backed up; server connections are preserved unchanged.
                auto db = findDbName(ref.original);
                fs::path dbFile = db && !db->value.empty() && !hasConnectionKey(ref.original, L"host=") && !hasConnectionKey(ref.original, L"service=")
                    ? resolveLocal(fs::path(db->value)) : fs::path();
                if (dbFile.empty() || !fs::is_regular_file(dbFile, ec)) {
                    ++remotePreserved;
                    state.log(LogLevel::Verbose, L"Left database or service datasource unchanged: " + abbreviated(ref.original));
                    continue;
                }
                if (matchesAnyPattern(dbFile, state.skipPatterns)) {
                    replacements[ref.original] = replaceDbName(ref.original, *db, dbFile.generic_wstring());
                    state.log(LogLevel::Verbose, L"Skipped by pattern; reference kept at original database: " + dbFile.native()); continue;
                }
                auto relative = backupLocal(state, dbFile);
                if (!relative) { if (state.cancelled()) return finish(BackupStatus::Cancelled, BackupError::Cancelled, L"Backup cancelled."); continue; }
                replacements[ref.original] = replaceDbName(ref.original, *db, L"./" + *relative);
                continue;
            }
            if (!fs::exists(source, ec)) { state.log(LogLevel::Warning, L"Source does not exist:\r\n" + source.native()); continue; }
            if (pathsEquivalent(source, projectDirectory)) { state.log(LogLevel::Verbose, L"Skipped project directory: " + source.native()); continue; }
            if (matchesAnyPattern(source, state.skipPatterns)) {
                replacements[ref.original] = fileUri ? makeAbsoluteFileUri(source) + ref.suffix : source.generic_wstring() + ref.suffix;
                state.log(LogLevel::Verbose, L"Skipped by pattern; reference kept at original source: " + source.native()); continue;
            }
            auto relative = backupLocal(state, source);
            if (!relative) { if (state.cancelled()) return finish(BackupStatus::Cancelled, BackupError::Cancelled, L"Backup cancelled."); continue; }
            replacements[ref.original] = fileUri ? makeRelativeFileUri(*relative) + ref.suffix : *relative + ref.suffix;
        }

        if (remotePreserved || embeddedPreserved || virtualPreserved) {
            state.log(LogLevel::Info, L"Preserved unchanged: " + std::to_wstring(remotePreserved) + L" remote/database, " +
                      std::to_wstring(embeddedPreserved) + L" embedded, " + std::to_wstring(virtualPreserved) + L" virtual references.");
        }

        state.progress(BackupStage::UpdatingProject, refs.size(), refs.size()); xml.applyReplacements(replacements);
        if (qgz) {
            if (!xml.save(qgsFile, xmlError)) return finish(BackupStatus::Failure, BackupError::XmlWriteFailure, xmlError);
            state.progress(BackupStage::CreatingQgz, refs.size(), refs.size());
            if (!createZipFromDirectory(temporary->path(), destination / project.filename(), xmlError)) return finish(BackupStatus::Failure, BackupError::ZipWriteFailure, xmlError);
        } else if (!xml.save(destination / project.filename(), xmlError)) return finish(BackupStatus::Failure, BackupError::XmlWriteFailure, xmlError);
        state.progress(BackupStage::Finishing, refs.size(), refs.size());
        auto status = state.result.warnings ? BackupStatus::SuccessWithWarnings : BackupStatus::Success;
        return finish(status, BackupError::None, state.result.warnings ? L"Backup completed with warnings." : L"Backup completed successfully.");
    } catch (const std::exception& ex) {
        return finish(BackupStatus::Failure, BackupError::Unknown, L"Unexpected error: " + utf8ToWide(ex.what()));
    } catch (...) { return finish(BackupStatus::Failure, BackupError::Unknown, L"Unexpected error."); }
}

} // namespace qgsbackup
