#include "qgis_project.h"
#include "wildcard.h"
#include <filesystem>
#include <iostream>

using namespace qgsbackup;

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* name) { if (!condition) { std::cerr << "FAILED: " << name << '\n'; ++failures; } };
    auto a = splitDataSourceReference(L"D:/data/test.gpkg|layername=roads");
    check(a.base == L"D:/data/test.gpkg" && a.suffix == L"|layername=roads", "pipe suffix");
    auto b = splitDataSourceReference(L"something.csv?type=csv");
    check(b.base == L"something.csv" && b.suffix == L"?type=csv", "query suffix");
    check(!hasNonFileScheme(L"C:\\GIS\\data.gpkg"), "drive is not URI scheme");
    check(!hasNonFileScheme(L"../data/roads.shp"), "relative path");
    check(hasNonFileScheme(L"https://example.com/a.tif"), "network scheme");
    check(fileUriToPath(L"file:///C:/GIS/My%20Data/file.csv") == std::filesystem::path(L"C:\\GIS\\My Data\\file.csv"), "file URI decode");
    check(wildcardMatchInsensitive(L"ASZ_ORTOFOTO.TIF", L"*ortofoto*.*"), "case insensitive wildcard");
    check(wildcardMatchInsensitive(L"map.tif", L"*.tif"), "extension wildcard");
    check(wildcardMatchInsensitive(L"abc_dxf_GPKG_1.gpkg", L"*dxf_GPKG*"), "mixed case wildcard");
    check(wildcardMatchInsensitive(L"roads.shp", L"roads.?hp"), "question wildcard");
    check(!wildcardMatchInsensitive(L"roads.dbf", L"*.shp"), "wildcard negative");
    check(!hasNonFileScheme(L"C:\\Térképek\\Árvíz\\réteg.gpkg"), "Unicode Windows path");
    if (!failures) std::cout << "All unit tests passed\n";
    return failures ? 1 : 0;
}
