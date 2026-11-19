#include "cli.h"
#include "backup_engine.h"
#include "filesystem_utils.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

namespace qgsbackup {
namespace {
void usage() {
    std::wcout << L"QGIS Project Backup 1.1.3\n\n"
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
// Progress output. On a console, a single status line is redrawn in place (at most ten times a
// second) and trimmed to the window width. When output is redirected to a file or pipe, a line is
// written when a new datasource or stage starts, and at most every five seconds while a long one
// copies, so logs stay readable.
class ProgressPrinter {
public:
    ProgressPrinter() : out_(GetStdHandle(STD_OUTPUT_HANDLE)) { DWORD mode{}; console_ = GetConsoleMode(out_, &mode) != 0; }

    void update(const BackupProgress& p) {
        const ULONGLONG now = GetTickCount64();
        const bool started = p.currentItem != item_ || p.stage != stage_;
        if (!started && now - lastDraw_ < (console_ ? 100u : 5000u)) return;
        item_ = p.currentItem; stage_ = p.stage; lastDraw_ = now;
        if (console_) draw(format(p, width()));
        else std::wcout << format(p, 0) << L"\n";
    }

    // Removes the status line so that a log message or the summary starts on a clean line.
    void clear() {
        if (!shown_) return;
        std::wcout << L"\r" << std::wstring(shown_, L' ') << L"\r" << std::flush;
        shown_ = 0; lastDraw_ = 0;
    }

private:
    std::size_t width() const {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(out_, &info)) return 79;
        const int columns = info.srWindow.Right - info.srWindow.Left + 1;
        return columns > 20 ? static_cast<std::size_t>(columns - 1) : 79; // the last column would wrap
    }

    static std::wstring format(const BackupProgress& p, std::size_t limit) {
        std::wstring counter;
        if (p.totalItems) counter = L"[" + std::to_wstring(p.currentItem) + L"/" + std::to_wstring(p.totalItems) + L"] ";
        std::wstring name = p.currentSource;
        if (!p.detail.empty()) name += (name.empty() ? L"" : L" > ") + p.detail;
        std::wstring tail = L" — " + humanReadableSize(p.bytesCopied);
        if (p.bytesTotal) tail += L" / " + humanReadableSize(p.bytesTotal) + L" (" + std::to_wstring(std::min<std::uint64_t>(100, p.bytesCopied * 100 / p.bytesTotal)) + L"%)";
        std::wstring head = counter + stageName(p.stage) + (name.empty() ? L"" : L": ");
        if (!limit || head.size() + name.size() + tail.size() <= limit) return head + name + tail;
        // Too wide for the console: drop the stage label, then shorten the middle of the name,
        // keeping more of its end (the file being copied and the count) than of its start.
        if (!name.empty()) head = counter;
        if (head.size() + name.size() + tail.size() > limit) {
            const std::size_t fixed = head.size() + tail.size() + 3;
            const std::size_t room = limit > fixed ? limit - fixed : 0;
            if (room >= 8) name = name.substr(0, room / 3) + L"..." + name.substr(name.size() - (room - room / 3));
            else name.clear();
        }
        std::wstring line = head + name + tail;
        if (line.size() > limit) line.resize(limit);
        return line;
    }

    void draw(const std::wstring& line) {
        std::wcout << L"\r" << line;
        if (line.size() < shown_) std::wcout << std::wstring(shown_ - line.size(), L' ');
        std::wcout << std::flush;
        shown_ = line.size();
    }

    HANDLE out_{}; bool console_{false};
    std::size_t shown_{0}, item_{static_cast<std::size_t>(-1)};
    BackupStage stage_{BackupStage::OpeningProject};
    ULONGLONG lastDraw_{0};
};

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
        else if (!optionsEnded && arg == L"--version") { std::wcout << L"qgsbackup 1.1.3\n"; return 0; }
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
    BackupCallbacks callbacks; ProgressPrinter printer;
    callbacks.onLog = [&](LogLevel level, const std::wstring& message) {
        if (level == LogLevel::Verbose && !options.verbose) return;
        printer.clear();
        const wchar_t* prefix = level == LogLevel::Warning ? L"Warning: " : level == LogLevel::Error ? L"Error: " : level == LogLevel::Verbose ? L"[verbose] " : L"";
        (level == LogLevel::Error ? std::wcerr : std::wcout) << prefix << message << L"\n";
    };
    callbacks.onProgress = [&](const BackupProgress& p) { printer.update(p); };
    auto result = runBackup(options, callbacks);
    printer.clear();
    std::wcout << L"\n" << result.message << L"\nDestination: " << result.destination.native()
               << L"\nFiles copied: " << result.filesCopied << L"\nData copied: " << humanReadableSize(result.bytesCopied)
               << L"\nWarnings: " << result.warnings << L"\nElapsed: " << std::fixed << std::setprecision(1) << (result.elapsed.count() / 1000.0) << L" s\n";
    return exitCode(result);
}
} // namespace qgsbackup
