#include "cli.h"
#include "gui.h"
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {
std::wstring quoteArgument(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++backslashes; continue; }
        if (character == L'\"') quoted.append(backslashes * 2 + 1, L'\\');
        else quoted.append(backslashes, L'\\');
        backslashes = 0; quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\'); quoted.push_back(L'\"');
    return quoted;
}

int launchDetachedGui(const std::filesystem::path& initialProject = {}) {
    std::wstring executable(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length == executable.size()) return 1;
    executable.resize(length);
    std::wstring commandLine = quoteArgument(executable) + L" --gui-child";
    if (!initialProject.empty()) commandLine += L" " + quoteArgument(initialProject.native());
    std::vector<wchar_t> writable(commandLine.begin(), commandLine.end()); writable.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process)) return 1;
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 1) return launchDetachedGui();
    if (std::wstring(argv[1]) == L"--gui-child") return qgsbackup::runGui(argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    if (std::wstring(argv[1]) == L"--gui") return launchDetachedGui(argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    if (argc == 2) {
        std::filesystem::path candidate(argv[1]); auto ext = candidate.extension().wstring();
        if (_wcsicmp(ext.c_str(), L".qgs") == 0 || _wcsicmp(ext.c_str(), L".qgz") == 0) return launchDetachedGui(candidate);
    }
    return qgsbackup::runCli(argc, argv);
}
