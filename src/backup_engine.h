#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace qgsbackup {

enum class LogLevel { Info, Warning, Error, Verbose };
enum class BackupStage { OpeningProject, ExtractingQgz, ReadingProject, ScanningResources,
                         CopyingFiles, UpdatingProject, CreatingQgz, Finishing };
enum class BackupStatus { Success, SuccessWithWarnings, Failure, Cancelled };
enum class BackupError { None, InvalidProject, InvalidDestination, XmlReadFailure,
                         XmlWriteFailure, ZipReadFailure, ZipWriteFailure, FileCopyFailure,
                         AccessDenied, Cancelled, Unknown };

struct BackupProgress {
    std::size_t currentItem{};
    std::size_t totalItems{};
    std::uint64_t bytesCopied{};
    std::wstring currentSource;
    BackupStage stage{BackupStage::OpeningProject};
};

struct BackupOptions {
    std::filesystem::path projectFile;
    std::filesystem::path destination;
    std::filesystem::path skipFile;
    bool timestamped{false};
    bool useSkipFile{true};
    bool verbose{false};
};

struct BackupCallbacks {
    std::function<void(const BackupProgress&)> onProgress;
    std::function<void(LogLevel, const std::wstring&)> onLog;
    std::function<bool()> isCancelled;
};

struct BackupResult {
    BackupStatus status{BackupStatus::Failure};
    BackupError error{BackupError::Unknown};
    std::filesystem::path destination;
    std::uint64_t bytesCopied{};
    std::uint64_t filesCopied{};
    std::uint64_t warnings{};
    std::chrono::milliseconds elapsed{};
    std::wstring message;
};

BackupResult runBackup(const BackupOptions& options, const BackupCallbacks& callbacks = {});
std::wstring humanReadableSize(std::uint64_t bytes);
const wchar_t* stageName(BackupStage stage);

} // namespace qgsbackup
