#pragma once

namespace qgsbackup {

enum class Language { English, Hungarian };

// The process-wide display language. The CLI never changes it, so it stays
// English; the GUI switches to Hungarian when the Windows region is Hungary.
Language currentLanguage();
void setLanguage(Language language);
bool windowsRegionIsHungary();

inline const wchar_t* tr(const wchar_t* english, const wchar_t* hungarian) {
    return currentLanguage() == Language::Hungarian ? hungarian : english;
}

} // namespace qgsbackup
