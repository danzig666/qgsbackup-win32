#include "cli.h"
#include "backup_engine.h"
#include "filesystem_utils.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <iomanip>
#include <iostream>
#include <vector>

namespace qgsbackup {
namespace {
void usage() {
    std::wcout << L"QGIS Project Backup 1.0.2\n\n"
        L"Usage:\n  qgsbackup [options] <project.qgs|project.qgz> <destination> [options]\n\n"
        L"Options:\n  -d, --date          Create a timestamped destination subfolder\n"
        L"  --verbose           Show detailed logging\n"
        L"  -ns, --no-skip-file Ignore qgsbackup_skip.txt\n"
        L"  --gui               Open the graphical interface\n"
        L"  -h, --help          Show this help\n"
        L"  --version           Show the version\n"
        L"  --                  Treat all following arguments as paths\n\n"
        L"Backup options may appear before, between, or after the two paths.\n"
        L"Quotes surrounding paths are accepted and are not part of the path.\n";
}

std::wstring stripBoundaryQuotes(std::wstring value) {
    // The CRT normally consumes command-line quotes. Some shells and wrappers,
    // however, leave one at an argument boundary (notably a quoted Windows path
    // ending in backslashes). Such a quote can never be part of a valid project
    // extension or destination directory as entered by this CLI.
    while (!value.empty() && value.front() == L'"') value.erase(value.begin());
    while (!value.empty() && value.back() == L'"') value.pop_back();
    return value;
}
int exitCode(const BackupResult& result) {
    if (result.status == BackupStatus::Success || result.status == BackupStatus::SuccessWithWarnings) return 0;
    if (result.status == BackupStatus::Cancelled) return 6;
    switch (result.error) { case BackupError::InvalidProject: return 3; case BackupError::InvalidDestination: return 4;
    case BackupError::XmlReadFailure: case BackupError::XmlWriteFailure: case BackupError::ZipReadFailure: case BackupError::ZipWriteFailure: return 5;
    default: return 1; }
}
}

int runCli(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT); _setmode(_fileno(stderr), _O_U8TEXT);
    BackupOptions options; options.skipFile = executableDirectory() / L"qgsbackup_skip.txt";
    std::vector<std::wstring> paths;
    bool optionsEnded = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (!optionsEnded && arg == L"--") optionsEnded = true;
        else if (!optionsEnded && (arg == L"-d" || arg == L"--date")) options.timestamped = true;
        else if (!optionsEnded && arg == L"--verbose") options.verbose = true;
        else if (!optionsEnded && (arg == L"-ns" || arg == L"--no-skip-file")) options.useSkipFile = false;
        else if (!optionsEnded && (arg == L"-h" || arg == L"--help")) { usage(); return 0; }
        else if (!optionsEnded && arg == L"--version") { std::wcout << L"qgsbackup 1.0.2\n"; return 0; }
        else if (!optionsEnded && !arg.empty() && arg.front() == L'-') {
            std::wcerr << L"Unknown option: " << arg << L"\n"; return 2;
        } else {
            paths.push_back(stripBoundaryQuotes(std::move(arg)));
        }
    }
    if (paths.size() != 2) {
        std::wcerr << L"Error: exactly one project and one destination are required.\n\n";
        usage(); return 2;
    }
    options.projectFile = paths[0]; options.destination = paths[1];
    BackupCallbacks callbacks;
    callbacks.onLog = [&](LogLevel level, const std::wstring& message) {
        if (level == LogLevel::Verbose && !options.verbose) return;
        const wchar_t* prefix = level == LogLevel::Warning ? L"Warning: " : level == LogLevel::Error ? L"Error: " : level == LogLevel::Verbose ? L"[verbose] " : L"";
        (level == LogLevel::Error ? std::wcerr : std::wcout) << prefix << message << L"\n";
    };
    callbacks.onProgress = [&](const BackupProgress& p) {
        if (p.totalItems) std::wcout << L"[" << p.currentItem << L"/" << p.totalItems << L"] ";
        std::wcout << stageName(p.stage);
        if (!p.currentSource.empty()) std::wcout << L": " << p.currentSource;
        std::wcout << L" — " << humanReadableSize(p.bytesCopied) << L" copied\n";
    };
    auto result = runBackup(options, callbacks);
    std::wcout << L"\n" << result.message << L"\nDestination: " << result.destination.native()
               << L"\nFiles copied: " << result.filesCopied << L"\nData copied: " << humanReadableSize(result.bytesCopied)
               << L"\nWarnings: " << result.warnings << L"\nElapsed: " << std::fixed << std::setprecision(1) << (result.elapsed.count() / 1000.0) << L" s\n";
    return exitCode(result);
}
} // namespace qgsbackup
