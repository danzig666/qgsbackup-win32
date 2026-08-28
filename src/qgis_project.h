#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinyxml2 { class XMLDocument; }

namespace qgsbackup {

struct DataSourceReference {
    std::wstring original;
    std::wstring base;
    std::wstring suffix;
};

DataSourceReference splitDataSourceReference(const std::wstring& reference);
bool isFileUri(const std::wstring& value);
bool hasNonFileScheme(const std::wstring& value);
std::filesystem::path fileUriToPath(const std::wstring& uri);
std::wstring makeRelativeFileUri(const std::wstring& relativeReference);
std::wstring makeAbsoluteFileUri(const std::filesystem::path& path);

class QgisProject {
public:
    QgisProject();
    ~QgisProject();
    QgisProject(QgisProject&&) noexcept;
    QgisProject& operator=(QgisProject&&) noexcept;
    bool load(const std::filesystem::path& file, std::wstring& error);
    bool save(const std::filesystem::path& file, std::wstring& error) const;
    std::vector<DataSourceReference> references() const;
    void applyReplacements(const std::unordered_map<std::wstring, std::wstring>& replacements);
private:
    std::unique_ptr<tinyxml2::XMLDocument> document_;
};

} // namespace qgsbackup
