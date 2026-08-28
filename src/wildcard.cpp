#include "wildcard.h"
#include <cwctype>

namespace qgsbackup {
bool wildcardMatchInsensitive(std::wstring_view text, std::wstring_view pattern) {
    std::size_t t = 0, p = 0, star = std::wstring_view::npos, retry = 0;
    auto same = [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || same(text[t], pattern[p]))) { ++t; ++p; }
        else if (p < pattern.size() && pattern[p] == L'*') { star = p++; retry = t; }
        else if (star != std::wstring_view::npos) { p = star + 1; t = ++retry; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}
}
