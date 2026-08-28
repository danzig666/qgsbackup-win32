#pragma once
#include <string_view>

namespace qgsbackup {
bool wildcardMatchInsensitive(std::wstring_view text, std::wstring_view pattern);
}
