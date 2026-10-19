#include "qgis_project.h"
#include "filesystem_utils.h"
#include "localization.h"
#include "tinyxml2.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <unordered_set>

namespace fs = std::filesystem;
namespace qgsbackup {

DataSourceReference splitDataSourceReference(const std::wstring& reference) {
    auto pipe = reference.find(L'|'); auto query = reference.find(L'?');
    auto cut = std::min(pipe, query);
    if (pipe == std::wstring::npos) cut = query;
    if (query == std::wstring::npos) cut = pipe;
    if (cut == std::wstring::npos) return {reference, reference, L""};
    return {reference, reference.substr(0, cut), reference.substr(cut)};
}

static bool startsWithInsensitive(const std::wstring& value, const wchar_t* prefix) {
    const std::size_t n = wcslen(prefix); if (value.size() < n) return false;
    return _wcsnicmp(value.c_str(), prefix, n) == 0;
}

bool isFileUri(const std::wstring& value) { return startsWithInsensitive(value, L"file:"); }

bool hasNonFileScheme(const std::wstring& value) {
    if (isFileUri(value)) return false;
    if (value.size() >= 2 && iswalpha(value[0]) && value[1] == L':') return false;
    if (value.rfind(L"\\\\", 0) == 0) return false;
    auto colon = value.find(L':');
    if (colon == std::wstring::npos || colon == 0) return false;
    return std::all_of(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(colon), [](wchar_t c) {
        return iswalnum(c) || c == L'+' || c == L'-' || c == L'.';
    });
}

static std::wstring percentDecode(const std::wstring& input) {
    std::string utf8 = wideToUtf8(input), bytes; bytes.reserve(utf8.size());
    auto hex = [](char c)->int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '%' && i + 2 < utf8.size()) { int a = hex(utf8[i + 1]), b = hex(utf8[i + 2]); if (a >= 0 && b >= 0) { bytes.push_back(static_cast<char>((a << 4) | b)); i += 2; continue; } }
        bytes.push_back(utf8[i]);
    }
    return utf8ToWide(bytes);
}

fs::path fileUriToPath(const std::wstring& uri) {
    std::wstring value = uri.substr(5);
    if (value.rfind(L"///", 0) == 0) value.erase(0, 3);
    else if (value.rfind(L"//", 0) == 0) value = L"\\\\" + value.substr(2);
    if (value.size() >= 3 && value[0] == L'/' && iswalpha(value[1]) && value[2] == L':') value.erase(0, 1);
    value = percentDecode(value);
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return fs::path(value);
}

static std::wstring encodeUriPath(const std::wstring& value, bool allowColon) {
    const std::string utf8 = wideToUtf8(value); std::string encoded;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : utf8) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || (allowColon && c == ':')) encoded.push_back(static_cast<char>(c));
        else { encoded.push_back('%'); encoded.push_back(hex[c >> 4]); encoded.push_back(hex[c & 15]); }
    }
    return utf8ToWide(encoded);
}

std::wstring makeRelativeFileUri(const std::wstring& relativeReference) { return L"file:" + encodeUriPath(relativeReference, false); }

std::wstring makeAbsoluteFileUri(const fs::path& path) {
    std::wstring generic = path.generic_wstring();
    if (generic.rfind(L"//", 0) == 0) return L"file:" + encodeUriPath(generic, true);
    return L"file:///" + encodeUriPath(generic, true);
}

QgisProject::QgisProject() : document_(std::make_unique<tinyxml2::XMLDocument>()) {}
QgisProject::~QgisProject() = default;
QgisProject::QgisProject(QgisProject&&) noexcept = default;
QgisProject& QgisProject::operator=(QgisProject&&) noexcept = default;

bool QgisProject::load(const fs::path& file, std::wstring& error) {
    std::ifstream in(file, std::ios::binary); if (!in) { error = tr(L"Unable to open XML: ", L"Nem sikerült megnyitni az XML-t: ") + file.native(); return false; }
    std::string xml((std::istreambuf_iterator<char>(in)), {});
    auto result = document_->Parse(xml.data(), xml.size());
    if (result != tinyxml2::XML_SUCCESS) { error = tr(L"Malformed QGIS XML: ", L"Hibás QGIS XML: ") + utf8ToWide(document_->ErrorStr()); return false; }
    return true;
}

bool QgisProject::save(const fs::path& file, std::wstring& error) const {
    tinyxml2::XMLPrinter printer(nullptr, false); document_->Print(&printer);
    fs::path temporary = file; temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) { error = tr(L"Unable to create project: ", L"Nem sikerült létrehozni a projektet: ") + temporary.native(); return false; }
    out.write(printer.CStr(), static_cast<std::streamsize>(printer.CStrSize() - 1)); out.close();
    if (!out) { error = tr(L"Unable to write project: ", L"Nem sikerült írni a projektet: ") + temporary.native(); return false; }
    std::error_code ec; fs::remove(file, ec); ec.clear(); fs::rename(temporary, file, ec);
    if (ec) { fs::remove(temporary); error = tr(L"Unable to commit project: ", L"Nem sikerült véglegesíteni a projektet: ") + file.native() + L" (" + utf8ToWide(ec.message()) + L")"; return false; }
    return true;
}

static std::string localName(const char* name) {
    std::string value = name ? name : ""; auto colon = value.find(':'); return colon == std::string::npos ? value : value.substr(colon + 1);
}

static std::wstring trim(std::wstring value) {
    auto first = value.find_first_not_of(L" \t\r\n"); if (first == std::wstring::npos) return {};
    auto last = value.find_last_not_of(L" \t\r\n"); return value.substr(first, last - first + 1);
}

static void visitElements(tinyxml2::XMLElement* element, const std::function<void(tinyxml2::XMLElement*)>& fn) {
    if (!element) return; fn(element);
    for (auto* child = element->FirstChildElement(); child; child = child->NextSiblingElement()) visitElements(child, fn);
}

std::vector<DataSourceReference> QgisProject::references() const {
    std::vector<DataSourceReference> refs; std::unordered_set<std::wstring> seen;
    visitElements(document_->RootElement(), [&](tinyxml2::XMLElement* elem) {
        for (const char* attrName : {"source", "file"}) if (const char* raw = elem->Attribute(attrName)) {
            auto value = trim(utf8ToWide(raw)); if (seen.insert(value).second) refs.push_back(splitDataSourceReference(value));
        }
        if (localName(elem->Name()) == "datasource" && elem->GetText()) {
            auto value = trim(utf8ToWide(elem->GetText())); if (seen.insert(value).second) refs.push_back(splitDataSourceReference(value));
        }
    });
    return refs;
}

void QgisProject::applyReplacements(const std::unordered_map<std::wstring, std::wstring>& replacements) {
    visitElements(document_->RootElement(), [&](tinyxml2::XMLElement* elem) {
        for (const char* attrName : {"source", "file"}) if (const char* raw = elem->Attribute(attrName)) {
            auto it = replacements.find(trim(utf8ToWide(raw))); if (it != replacements.end()) elem->SetAttribute(attrName, wideToUtf8(it->second).c_str());
        }
        if (localName(elem->Name()) == "datasource" && elem->GetText()) {
            auto it = replacements.find(trim(utf8ToWide(elem->GetText()))); if (it != replacements.end()) elem->SetText(wideToUtf8(it->second).c_str());
        }
    });
}

} // namespace qgsbackup
