#include "backup_engine.h"
#include "filesystem_utils.h"
#include "localization.h"
#include "qgis_project.h"
#include "wildcard.h"
#include "zip_utils.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
namespace qgsbackup {

namespace {
struct RunState {
    const BackupOptions& options;
    const BackupCallbacks& callbacks;
    BackupResult result;
    std::vector<std::wstring> skipPatterns;
    BackupProgress last;
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
        last = {current, total, 0, 0, source, stage}; reportBytes();
    }
    // Re-sends the last progress with updated byte counters; called while large files stream.
    void reportBytes() {
        if (!callbacks.onProgress) return;
        last.bytesCopied = result.bytesCopied; last.bytesTotal = std::max(result.bytesTotal, result.bytesCopied);
        callbacks.onProgress(last);
    }
};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); }); return value;
}

bool copyOne(RunState& state, const fs::path& source, const fs::path& destination) {
    std::uint64_t before = state.result.bytesCopied;
    if (!copyFileStreaming(source, destination, state.callbacks, state.result.bytesCopied, [&state] { state.reportBytes(); })) {
        if (state.cancelled()) return false;
        state.log(LogLevel::Warning, tr(L"Unable to copy:\r\n", L"Nem sikerült másolni:\r\n") + source.native() + tr(L"\r\nto:\r\n", L"\r\nide:\r\n") + destination.native());
        return false;
    }
    ++state.result.filesCopied; state.reportBytes();
    state.log(LogLevel::Verbose, tr(L"Copied ", L"Átmásolva: ") + source.native() + L" (" + humanReadableSize(state.result.bytesCopied - before) + L")");
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
    if (ec) { state.log(LogLevel::Warning, tr(L"Unable to create layers folder: ", L"Nem sikerült létrehozni a layers mappát: ") + layers.native()); return std::nullopt; }
    bool primaryCopied = false, allCopied = true;
    for (fs::directory_iterator it(source.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        auto targetName = associatedTargetName(it->path(), source, destinationName);
        if (!targetName) continue;
        if (!pathsEquivalent(it->path(), source) && matchesAnyPattern(it->path(), state.skipPatterns)) {
            state.log(LogLevel::Verbose, tr(L"Skipped associated file by pattern: ", L"Kihagyott kapcsolódó fájl (minta alapján): ") + it->path().native()); continue;
        }
        bool copied = copyOnce(state, it->path(), layers / *targetName);
        if (pathsEquivalent(it->path(), source)) primaryCopied = copied;
        if (!copied) allCopied = false;
        if (!copied && state.cancelled()) return std::nullopt;
    }
    if (ec) state.log(LogLevel::Warning, tr(L"Unable to enumerate associated files for: ", L"Nem sikerült felsorolni a kapcsolódó fájlokat: ") + source.native());
    return primaryCopied && allCopied ? std::optional<std::wstring>((fs::path(L"layers") / destinationName).generic_wstring()) : std::nullopt;
}

bool isBackupDestination(const RunState& state, const fs::directory_entry& entry) {
    std::error_code ec;
    return !state.destinationKey.empty() && entry.is_directory(ec) && normalizedPathKey(entry.path()) == state.destinationKey;
}

std::optional<std::wstring> copyDirectory(RunState& state, const fs::path& source,
                                          const fs::path& layers, const fs::path& destinationName) {
    // An existing folder from an earlier backup into the same destination is refreshed, not reused as is.
    fs::path targetRoot = layers / destinationName; std::error_code ec;
    fs::create_directories(targetRoot, ec);
    if (ec) { state.log(LogLevel::Warning, tr(L"Unable to create directory: ", L"Nem sikerült létrehozni a mappát: ") + targetRoot.native()); return std::nullopt; }
    for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (state.cancelled()) return std::nullopt;
        // When the backup is written inside the datasource folder, never copy the backup into itself.
        if (isBackupDestination(state, *it)) {
            state.log(LogLevel::Verbose, tr(L"Not copying the backup destination into itself: ", L"A mentési cél nem másolódik önmagába: ") + it->path().native());
            it.disable_recursion_pending(); continue;
        }
        auto relative = fs::relative(it->path(), source, ec); if (ec) break;
        auto target = targetRoot / relative;
        if (it->is_directory(ec)) fs::create_directories(target, ec);
        else if (it->is_regular_file(ec) && !copyOne(state, it->path(), target) && state.cancelled()) return std::nullopt;
        if (ec) break;
    }
    if (ec) { state.log(LogLevel::Warning, tr(L"Directory copy was incomplete: ", L"A mappa másolása hiányos: ") + source.native() + L" (" + utf8ToWide(ec.message()) + L")"); return std::nullopt; }
    return (fs::path(L"layers") / destinationName).generic_wstring();
}

// QGIS stores files attached to a .qgs project in "<project basename>_attachments.zip" beside it.
// The archive is part of the project itself, so it is always copied, even if a skip pattern matches.
bool isProjectAttachmentsArchive(const fs::path& file, const fs::path& project) {
    return lower(file.filename().native()) == lower(project.stem().native() + L"_attachments.zip");
}

// Files beside the project that belong to it: same basename (.qgd, .qgs~) or the attachments archive.
enum class ProjectFile { Unrelated, Copy, Skipped };
ProjectFile classifyProjectFile(const RunState& state, const fs::path& file, const fs::path& project) {
    if (pathsEquivalent(file, project)) return ProjectFile::Unrelated;
    if (isProjectAttachmentsArchive(file, project)) return ProjectFile::Copy;
    if (lower(file.stem().native()) != lower(project.stem().native())) return ProjectFile::Unrelated;
    return matchesAnyPattern(file, state.skipPatterns) ? ProjectFile::Skipped : ProjectFile::Copy;
}

bool copyProjectAssociated(RunState& state, const fs::path& project, const fs::path& destination) {
    std::error_code ec;
    for (fs::directory_iterator it(project.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto kind = classifyProjectFile(state, it->path(), project);
        if (kind == ProjectFile::Unrelated) continue;
        if (kind == ProjectFile::Skipped) { state.log(LogLevel::Verbose, tr(L"Skipped project-associated file: ", L"Kihagyott projekthez tartozó fájl: ") + it->path().native()); continue; }
        if (!copyOne(state, it->path(), destination / it->path().filename()) && state.cancelled()) return false;
    }
    if (ec) state.log(LogLevel::Warning, tr(L"Unable to enumerate project-associated files: ", L"Nem sikerült felsorolni a projekthez tartozó fájlokat: ") + utf8ToWide(ec.message()));
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
        state.log(LogLevel::Verbose, tr(L"Filename collision: ", L"Fájlnév-ütközés: ") + source.filename().native() + L" -> " + destinationName.native());
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
    if (v[key] == L' ') ++key;
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
    if (isEmbeddedSource(ref.base)) return tr(L"embedded data", L"beágyazott adat");
    if (isVirtualSource(ref.original)) return tr(L"virtual layer", L"virtuális réteg");
    if (hasNonFileScheme(ref.base)) return tr(L"remote resource", L"távoli erőforrás");
    if (looksLikeRemoteProvider(ref.original)) return tr(L"remote layer", L"távoli réteg");
    if (looksLikeConnectionString(ref.original)) return tr(L"database or service layer", L"adatbázis- vagy szolgáltatásréteg");
    auto name = fs::path(ref.base).filename().native();
    return name.empty() ? tr(L"local resource", L"helyi erőforrás") : name;
}

std::wstring cancelledMessage() { return tr(L"Backup cancelled.", L"A mentés megszakítva."); }

std::wstring abbreviated(const std::wstring& value, std::size_t limit = 180) {
    return value.size() <= limit ? value : value.substr(0, limit) + L"...";
}

std::optional<fs::path> chooseQgs(const fs::path& extracted, const fs::path& archive, std::wstring& error) {
    std::vector<fs::path> candidates; std::error_code ec;
    for (fs::recursive_directory_iterator it(extracted, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && lower(it->path().extension().native()) == L".qgs") candidates.push_back(it->path());
    if (candidates.empty()) { error = tr(L"No .qgs project found inside QGZ.", L"A QGZ fájl nem tartalmaz .qgs projektet."); return std::nullopt; }
    if (candidates.size() == 1) return candidates.front();
    std::vector<fs::path> matching;
    for (const auto& p : candidates) if (lower(p.stem().native()) == lower(archive.stem().native())) matching.push_back(p);
    if (matching.size() == 1) return matching.front();
    error = tr(L"The QGZ contains multiple .qgs files and the logical project is ambiguous.", L"A QGZ több .qgs fájlt tartalmaz, nem egyértelmű, melyik a projekt."); return std::nullopt;
}

// GDAL virtual file system path to a layer inside an archive: /vsizip/<archive>/<path inside>,
// or /vsizip/{<archive>}/<path inside>. /vsigzip/ has no inner path.
struct ArchiveReference { std::wstring prefix, inner; bool braces{false}; };

bool startsWithInsensitive(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() && lower(value.substr(0, prefix.size())) == prefix;
}

bool isArchiveFile(const fs::path& file) {
    const auto name = lower(file.filename().native());
    for (const wchar_t* ext : {L".zip", L".kmz", L".7z", L".rar", L".tar", L".tgz", L".gz"})
        if (name.size() > wcslen(ext) && name.compare(name.size() - wcslen(ext), wcslen(ext), ext) == 0) return true;
    return false;
}

// Skip patterns for a datasource. For an archive that is itself the datasource, extension-only
// patterns such as *.zip do not apply (they are meant for stray archives beside layers); named
// patterns still do, and patterns are also matched against the layer inside the archive.
bool isSkippedDatasource(const RunState& state, const fs::path& source, const std::wstring& inner = {}) {
    if (!inner.empty() && matchesAnyPattern(fs::path(inner).filename(), state.skipPatterns)) return true;
    if (!isArchiveFile(source)) return matchesAnyPattern(source, state.skipPatterns);
    const auto name = source.filename().native();
    return std::any_of(state.skipPatterns.begin(), state.skipPatterns.end(), [&](const std::wstring& pattern) {
        const bool extensionOnly = pattern.size() > 2 && pattern.compare(0, 2, L"*.") == 0 &&
                                   pattern.find_first_of(L"*?", 2) == std::wstring::npos;
        return !extensionOnly && wildcardMatchInsensitive(name, pattern);
    });
}

enum class SourceKind { None, Virtual, Embedded, Remote, Database, LocalDatabase, Archive, Local };
struct ResolvedSource {
    SourceKind kind{SourceKind::None}; fs::path path; bool fileUri{false};
    std::optional<DbNameValue> db; std::optional<ArchiveReference> archive;
};

std::wstring archiveReference(const ArchiveReference& archive, const std::wstring& path) {
    return archive.prefix + (archive.braces ? L"{" + path + L"}" : path) + archive.inner;
}

ResolvedSource resolveSource(const DataSourceReference& ref, const fs::path& projectDirectory) {
    if (isVirtualSource(ref.original)) return {SourceKind::Virtual};
    const bool fileUri = isFileUri(ref.base);
    if (isEmbeddedSource(ref.base)) return {SourceKind::Embedded};
    if (hasNonFileScheme(ref.base) || looksLikeRemoteProvider(ref.original)) return {SourceKind::Remote};
    auto resolveLocal = [&](const fs::path& p) { return p.is_absolute() ? p : (projectDirectory / p).lexically_normal(); };
    if (startsWithInsensitive(ref.base, L"/vsi")) {
        std::wstring prefix;
        for (const wchar_t* candidate : {L"/vsizip/", L"/vsigzip/", L"/vsitar/", L"/vsi7z/", L"/vsirar/"})
            if (startsWithInsensitive(ref.base, candidate)) prefix = ref.base.substr(0, wcslen(candidate));
        // Other GDAL virtual file systems (/vsicurl/, /vsis3/, /vsimem/, ...) are not local files.
        if (prefix.empty()) return {SourceKind::Remote};
        const std::wstring rest = ref.base.substr(prefix.size());
        if (startsWithInsensitive(rest, L"/vsi")) return {SourceKind::Remote}; // e.g. /vsizip//vsicurl/...
        std::error_code ec;
        if (!rest.empty() && rest.front() == L'{') {
            const auto close = rest.find(L'}');
            if (close == std::wstring::npos) return {SourceKind::Local, resolveLocal(rest)};
            ArchiveReference archive{prefix, rest.substr(close + 1), true};
            return {SourceKind::Archive, resolveLocal(rest.substr(1, close - 1)), false, std::nullopt, archive};
        }
        // The archive is the shortest leading part of the path that is an existing file.
        for (std::size_t end = rest.find_first_of(L"/\\"); ; end = rest.find_first_of(L"/\\", end + 1)) {
            const auto candidate = rest.substr(0, end);
            if (!candidate.empty() && fs::is_regular_file(resolveLocal(candidate), ec)) {
                ArchiveReference archive{prefix, end == std::wstring::npos ? L"" : rest.substr(end), false};
                return {SourceKind::Archive, resolveLocal(candidate), false, std::nullopt, archive};
            }
            if (end == std::wstring::npos) break;
        }
        return {SourceKind::Local, resolveLocal(rest)}; // reported as missing
    }
    fs::path source = fileUri ? fileUriToPath(ref.base) : fs::path(ref.base);
    if (source.empty()) return {};
    source = resolveLocal(source);
    std::error_code ec;
    if (!fileUri && !fs::exists(source, ec) && looksLikeConnectionString(ref.original)) {
        // Database/service connection. A local database file named by dbname (SpatiaLite) is
        // backed up; server connections are preserved unchanged.
        auto db = findDbName(ref.original);
        if (!db || db->value.empty() || hasConnectionKey(ref.original, L"host=") || hasConnectionKey(ref.original, L"service=")) return {SourceKind::Database};
        fs::path dbFile = resolveLocal(fs::path(db->value));
        if (!fs::is_regular_file(dbFile, ec)) return {SourceKind::Database};
        return {SourceKind::LocalDatabase, dbFile, false, db};
    }
    return {SourceKind::Local, source, fileUri};
}

std::uint64_t sizeOf(const fs::directory_entry& entry) { std::error_code ec; auto size = entry.file_size(ec); return ec ? 0 : size; }

// Mirrors copyAssociatedFiles: the datasource and its sidecars.
std::uint64_t associatedSize(RunState& state, const fs::path& source, std::unordered_set<std::wstring>& counted) {
    std::uint64_t total = 0; std::error_code ec;
    for (fs::directory_iterator it(source.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !associatedTargetName(it->path(), source, source.filename())) continue;
        if (!pathsEquivalent(it->path(), source) && matchesAnyPattern(it->path(), state.skipPatterns)) continue;
        if (counted.insert(normalizedPathKey(it->path())).second) total += sizeOf(*it);
    }
    return total;
}

// Mirrors copyProjectAssociated.
std::uint64_t projectAssociatedSize(RunState& state, const fs::path& project) {
    std::uint64_t total = 0; std::error_code ec;
    for (fs::directory_iterator it(project.parent_path(), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && classifyProjectFile(state, it->path(), project) == ProjectFile::Copy) total += sizeOf(*it);
    return total;
}

std::uint64_t directorySize(RunState& state, const fs::path& source) {
    std::uint64_t total = 0; std::error_code ec;
    for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (state.cancelled()) break;
        if (isBackupDestination(state, *it)) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec)) total += sizeOf(*it);
    }
    return total;
}

// Estimates the bytes the backup will copy so progress can be shown as copied/total.
std::uint64_t estimateTotalBytes(RunState& state, const fs::path& project, const std::vector<DataSourceReference>& refs) {
    std::uint64_t total = projectAssociatedSize(state, project);
    const fs::path projectDirectory = project.parent_path(); std::unordered_set<std::wstring> seen, counted; std::error_code ec;
    for (const auto& ref : refs) {
        if (state.cancelled()) break;
        auto resolved = resolveSource(ref, projectDirectory);
        const bool copyable = resolved.kind == SourceKind::Local || resolved.kind == SourceKind::LocalDatabase || resolved.kind == SourceKind::Archive;
        if (!copyable || !fs::exists(resolved.path, ec) || pathsEquivalent(resolved.path, projectDirectory) ||
            isSkippedDatasource(state, resolved.path, resolved.archive ? resolved.archive->inner : std::wstring()) ||
            !seen.insert(normalizedPathKey(resolved.path)).second) continue;
        if (fs::is_directory(resolved.path, ec)) total += directorySize(state, resolved.path);
        else if (fs::is_regular_file(resolved.path, ec)) total += associatedSize(state, resolved.path, counted);
    }
    return total;
}
}

std::wstring humanReadableSize(std::uint64_t bytes) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double size = static_cast<double>(bytes); int unit = 0; while (size >= 1024.0 && unit < 5) { size /= 1024.0; ++unit; }
    std::wostringstream out; out << std::fixed << std::setprecision(2) << size; auto text = out.str();
    if (currentLanguage() == Language::Hungarian) std::replace(text.begin(), text.end(), L'.', L',');
    return text + L" " + units[unit];
}

const wchar_t* stageName(BackupStage stage) {
    switch (stage) {
    case BackupStage::OpeningProject: return tr(L"Opening project", L"Projekt megnyitása"); case BackupStage::ExtractingQgz: return tr(L"Extracting QGZ", L"QGZ kibontása");
    case BackupStage::ReadingProject: return tr(L"Reading QGIS project", L"QGIS projekt beolvasása"); case BackupStage::ScanningResources: return tr(L"Scanning resources", L"Erőforrások felmérése");
    case BackupStage::CopyingFiles: return tr(L"Copying files", L"Fájlok másolása"); case BackupStage::UpdatingProject: return tr(L"Updating project", L"Projekt frissítése");
    case BackupStage::CreatingQgz: return tr(L"Creating QGZ", L"QGZ létrehozása"); case BackupStage::Finishing: return tr(L"Finishing", L"Befejezés");
    } return tr(L"Working", L"Folyamatban");
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
            return finish(BackupStatus::Failure, BackupError::InvalidProject, tr(L"The selected project must be an existing .qgs or .qgz file.\r\n", L"A kiválasztott projektnek létező .qgs vagy .qgz fájlnak kell lennie.\r\n") + project.native());
        fs::path destination = fs::absolute(options.destination, ec).lexically_normal();
        if (ec || destination.empty()) return finish(BackupStatus::Failure, BackupError::InvalidDestination, tr(L"The backup destination is invalid.", L"A mentési cél érvénytelen."));
        if (options.timestamped) destination /= project.stem().native() + L"_" + timestampNow();
        fs::create_directories(destination, ec);
        if (ec || !fs::is_directory(destination, ec)) return finish(BackupStatus::Failure, BackupError::InvalidDestination, tr(L"Unable to create destination:\r\n", L"Nem sikerült létrehozni a célmappát:\r\n") + destination.native());
        if (pathsEquivalent(project, destination / project.filename()))
            return finish(BackupStatus::Failure, BackupError::InvalidDestination, tr(L"The destination would overwrite the original project. Choose another folder.", L"A cél felülírná az eredeti projektet. Válasszon másik mappát."));
        state.result.destination = destination;
        state.layers = destination / L"layers"; state.destinationKey = normalizedPathKey(destination);
        state.log(LogLevel::Info, tr(L"Project: ", L"Projekt: ") + project.native()); state.log(LogLevel::Info, tr(L"Destination: ", L"Cél: ") + destination.native());
        if (options.useSkipFile) {
            state.skipPatterns = loadSkipPatterns(options.skipFile);
            state.log(LogLevel::Info, options.skipFile.empty() || !fs::exists(options.skipFile) ? std::wstring(tr(L"Skip file not found; continuing with zero patterns.", L"A kihagyási lista nem található, minták nélkül folytatódik.")) :
                      tr(L"Loaded ", L"Betöltve ") + std::to_wstring(state.skipPatterns.size()) + tr(L" skip patterns from ", L" kihagyási minta innen: ") + options.skipFile.native());
        } else state.log(LogLevel::Verbose, tr(L"Skip file disabled.", L"Kihagyási lista kikapcsolva."));

        const bool qgz = lower(project.extension().native()) == L".qgz";
        std::unique_ptr<ScopedTempDirectory> temporary; fs::path qgsFile = project;
        if (qgz) {
            state.progress(BackupStage::ExtractingQgz); temporary = std::make_unique<ScopedTempDirectory>(); std::wstring error;
            if (!extractZipSafely(project, temporary->path(), error)) return finish(BackupStatus::Failure, BackupError::ZipReadFailure, error);
            auto chosen = chooseQgs(temporary->path(), project, error); if (!chosen) return finish(BackupStatus::Failure, BackupError::ZipReadFailure, error); qgsFile = *chosen;
        }
        state.progress(BackupStage::ReadingProject); QgisProject xml; std::wstring xmlError;
        if (!xml.load(qgsFile, xmlError)) return finish(BackupStatus::Failure, BackupError::XmlReadFailure, xmlError);
        auto refs = xml.references(); state.log(LogLevel::Info, tr(L"Found ", L"") + std::to_wstring(refs.size()) + tr(L" unique datasource references.", L" egyedi adatforrás-hivatkozás található."));
        state.progress(BackupStage::ScanningResources, 0, refs.size());
        state.result.bytesTotal = estimateTotalBytes(state, project, refs);
        if (state.cancelled()) return finish(BackupStatus::Cancelled, BackupError::Cancelled, cancelledMessage());
        state.log(LogLevel::Info, tr(L"Data to copy: ", L"Másolandó adat: ") + humanReadableSize(state.result.bytesTotal));
        state.progress(BackupStage::CopyingFiles, 0, refs.size());
        if (!copyProjectAssociated(state, project, destination)) return finish(BackupStatus::Cancelled, BackupError::Cancelled, cancelledMessage());

        std::unordered_map<std::wstring, std::wstring> replacements;
        std::size_t remotePreserved = 0, embeddedPreserved = 0, virtualPreserved = 0;
        fs::path projectDirectory = project.parent_path();
        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (state.cancelled()) return finish(BackupStatus::Cancelled, BackupError::Cancelled, cancelledMessage());
            const auto& ref = refs[i]; state.progress(BackupStage::CopyingFiles, i + 1, refs.size(), progressDescription(ref));
            const auto resolved = resolveSource(ref, projectDirectory);
            if (resolved.kind == SourceKind::Virtual) { ++virtualPreserved; state.log(LogLevel::Verbose, tr(L"Left virtual layer unchanged.", L"Virtuális réteg változatlanul hagyva.")); continue; }
            if (resolved.kind == SourceKind::Embedded) {
                ++embeddedPreserved;
                state.log(LogLevel::Verbose, tr(L"Left embedded resource unchanged (", L"Beágyazott erőforrás változatlanul hagyva (") + std::to_wstring(ref.original.size()) + tr(L" characters).", L" karakter)."));
                continue;
            }
            if (resolved.kind == SourceKind::Remote) {
                ++remotePreserved;
                state.log(LogLevel::Verbose, tr(L"Left remote datasource unchanged: ", L"Távoli adatforrás változatlanul hagyva: ") + abbreviated(ref.original));
                continue;
            }
            if (resolved.kind == SourceKind::Database) {
                ++remotePreserved;
                state.log(LogLevel::Verbose, tr(L"Left database or service datasource unchanged: ", L"Adatbázis- vagy szolgáltatás-adatforrás változatlanul hagyva: ") + abbreviated(ref.original));
                continue;
            }
            if (resolved.kind == SourceKind::LocalDatabase) {
                if (isSkippedDatasource(state, resolved.path)) {
                    replacements[ref.original] = replaceDbName(ref.original, *resolved.db, resolved.path.generic_wstring());
                    state.log(LogLevel::Verbose, tr(L"Skipped by pattern; reference kept at original source: ", L"Minta alapján kihagyva; a hivatkozás az eredeti forrásra mutat: ") + resolved.path.native()); continue;
                }
                auto relative = backupLocal(state, resolved.path);
                if (!relative) { if (state.cancelled()) return finish(BackupStatus::Cancelled, BackupError::Cancelled, cancelledMessage()); continue; }
                replacements[ref.original] = replaceDbName(ref.original, *resolved.db, L"./" + *relative);
                continue;
            }
            if (resolved.kind == SourceKind::Archive) {
                const auto& archive = *resolved.archive;
                if (isSkippedDatasource(state, resolved.path, archive.inner)) {
                    replacements[ref.original] = archiveReference(archive, resolved.path.generic_wstring()) + ref.suffix;
                    state.log(LogLevel::Verbose, tr(L"Skipped by pattern; reference kept at original source: ", L"Minta alapján kihagyva; a hivatkozás az eredeti forrásra mutat: ") + resolved.path.native()); continue;
                }
                auto relative = backupLocal(state, resolved.path);
                if (!relative) { if (state.cancelled()) return finish(BackupStatus::Cancelled, BackupError::Cancelled, cancelledMessage()); continue; }
                // "./" makes QGIS resolve the path against the project without requiring it to exist
                // as a file, which a path inside an archive never does.
                replacements[ref.original] = archiveReference(archive, L"./" + *relative) + ref.suffix;
                continue;
            }
            if (resolved.kind != SourceKind::Local) continue;
            const fs::path& source = resolved.path; const bool fileUri = resolved.fileUri;
            if (!fs::exists(source, ec)) { state.log(LogLevel::Warning, tr(L"Source does not exist:\r\n", L"A forrás nem létezik:\r\n") + source.native()); continue; }
            if (pathsEquivalent(source, projectDirectory)) { state.log(LogLevel::Verbose, tr(L"Skipped project directory: ", L"Projektmappa kihagyva: ") + source.native()); continue; }
            if (isSkippedDatasource(state, source)) {
                replacements[ref.original] = fileUri ? makeAbsoluteFileUri(source) + ref.suffix : source.generic_wstring() + ref.suffix;
                state.log(LogLevel::Verbose, tr(L"Skipped by pattern; reference kept at original source: ", L"Minta alapján kihagyva; a hivatkozás az eredeti forrásra mutat: ") + source.native()); continue;
            }
            auto relative = backupLocal(state, source);
            if (!relative) { if (state.cancelled()) return finish(BackupStatus::Cancelled, BackupError::Cancelled, cancelledMessage()); continue; }
            replacements[ref.original] = fileUri ? makeRelativeFileUri(*relative) + ref.suffix : *relative + ref.suffix;
        }

        if (remotePreserved || embeddedPreserved || virtualPreserved) {
            state.log(LogLevel::Info, tr(L"Preserved unchanged: ", L"Változatlanul megtartva: ") + std::to_wstring(remotePreserved) + tr(L" remote/database, ", L" távoli/adatbázis, ") +
                      std::to_wstring(embeddedPreserved) + tr(L" embedded, ", L" beágyazott, ") + std::to_wstring(virtualPreserved) + tr(L" virtual references.", L" virtuális hivatkozás."));
        }

        state.progress(BackupStage::UpdatingProject, refs.size(), refs.size()); xml.applyReplacements(replacements);
        if (qgz) {
            if (!xml.save(qgsFile, xmlError)) return finish(BackupStatus::Failure, BackupError::XmlWriteFailure, xmlError);
            state.progress(BackupStage::CreatingQgz, refs.size(), refs.size());
            if (!createZipFromDirectory(temporary->path(), destination / project.filename(), xmlError)) return finish(BackupStatus::Failure, BackupError::ZipWriteFailure, xmlError);
        } else if (!xml.save(destination / project.filename(), xmlError)) return finish(BackupStatus::Failure, BackupError::XmlWriteFailure, xmlError);
        state.progress(BackupStage::Finishing, refs.size(), refs.size());
        auto status = state.result.warnings ? BackupStatus::SuccessWithWarnings : BackupStatus::Success;
        return finish(status, BackupError::None, state.result.warnings ? tr(L"Backup completed with warnings.", L"A mentés figyelmeztetésekkel elkészült.") : tr(L"Backup completed successfully.", L"A mentés sikeresen elkészült."));
    } catch (const std::exception& ex) {
        return finish(BackupStatus::Failure, BackupError::Unknown, tr(L"Unexpected error: ", L"Váratlan hiba: ") + utf8ToWide(ex.what()));
    } catch (...) { return finish(BackupStatus::Failure, BackupError::Unknown, tr(L"Unexpected error.", L"Váratlan hiba.")); }
}

} // namespace qgsbackup
