#include "localization.h"
#include <windows.h>
#include <atomic>

namespace qgsbackup {
namespace {
std::atomic<Language> language{Language::English};
constexpr GEOID HungaryGeoId = 109;
}

Language currentLanguage() { return language.load(); }
void setLanguage(Language value) { language = value; }

bool windowsRegionIsHungary() {
    // GetUserDefaultGeoName is available from Windows 10 1709; fall back to the nation GEOID.
    using GetGeoName = int(WINAPI*)(LPWSTR, int);
    if (auto getGeoName = reinterpret_cast<GetGeoName>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetUserDefaultGeoName"))) {
        wchar_t name[16]{};
        if (getGeoName(name, 16) > 0) return _wcsicmp(name, L"HU") == 0;
    }
    return GetUserGeoID(GEOCLASS_NATION) == HungaryGeoId;
}

} // namespace qgsbackup
