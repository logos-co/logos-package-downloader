#include <gtest/gtest.h>
#include "package_downloader_lib.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

TEST(DownloaderTest, ResolveDependenciesReturnsInputOnEmptyPackageList) {
    // When the catalog can't be fetched, resolveDependencies should
    // return the requested names as-is.
    PackageDownloaderLib dl;

    std::vector<std::string> names = {"pkg_a", "pkg_b"};
    std::string result = dl.resolveDependencies("__nonexistent_release_tag__", names);
    json resolved = json::parse(result);

    ASSERT_TRUE(resolved.is_array());
    EXPECT_EQ(resolved.size(), 2);
    EXPECT_EQ(resolved[0], "pkg_a");
    EXPECT_EQ(resolved[1], "pkg_b");
}

TEST(DownloaderTest, DownloadPackageReturnsEmptyForNonExistent) {
    PackageDownloaderLib dl;

    std::string result = dl.downloadPackage("__nonexistent_release_tag__", "nonexistent_package_xyz");
    EXPECT_TRUE(result.empty());
}

TEST(DownloaderTest, DownloadFileFailsForInvalidUrl) {
    PackageDownloaderLib dl;

    fs::path tempDir = fs::temp_directory_path() / ("lgpd_test_" + std::to_string(std::rand()));
    fs::create_directories(tempDir);
    fs::path dest = tempDir / "test_file.lgx";

    bool ok = dl.downloadFile("http://127.0.0.1:1/nonexistent", dest.string());
    EXPECT_FALSE(ok);

    std::error_code ec;
    fs::remove_all(tempDir, ec);
}

TEST(DownloaderTest, GetPackagesReturnsJsonArray) {
    PackageDownloaderLib dl;
    // Even if fetch fails, should return valid JSON array
    std::string result = dl.getPackages("__nonexistent_release_tag__");
    json packages = json::parse(result);
    EXPECT_TRUE(packages.is_array());
}

TEST(DownloaderTest, GetCategoriesReturnsJsonArray) {
    PackageDownloaderLib dl;

    std::string result = dl.getCategories("__nonexistent_release_tag__");
    json cats = json::parse(result);
    EXPECT_TRUE(cats.is_array());
    // Should at least contain "All"
    ASSERT_FALSE(cats.empty());
    EXPECT_EQ(cats[0], "All");
}

TEST(DownloaderTest, GetPackagesByCategoryReturnsJsonArray) {
    PackageDownloaderLib dl;

    std::string result = dl.getPackages("__nonexistent_release_tag__", "networking");
    json packages = json::parse(result);
    EXPECT_TRUE(packages.is_array());
}

TEST(DownloaderTest, EmptyReleaseTagResolvesToLatest) {
    PackageDownloaderLib dl;
    // Passing empty string should behave identically to "latest"
    // Both should return valid JSON arrays (we just verify no crash / valid structure)
    std::string result1 = dl.getPackages("");
    std::string result2 = dl.getPackages("latest");
    json p1 = json::parse(result1);
    json p2 = json::parse(result2);
    EXPECT_TRUE(p1.is_array());
    EXPECT_TRUE(p2.is_array());
    EXPECT_EQ(p1.size(), p2.size());
}

TEST(DownloaderTest, GetReleasesReturnsJsonArray) {
    PackageDownloaderLib dl;

    // Hits the GitHub API; if offline / rate-limited the lib falls back to
    // an empty array, so we only assert structure: must always be valid
    // JSON, must always be an array, must never throw.
    std::string result = dl.getReleases();
    json releases;
    ASSERT_NO_THROW(releases = json::parse(result));
    EXPECT_TRUE(releases.is_array());

    // If we got any entries, each must have a tag_name field.
    for (const auto& rel : releases) {
        EXPECT_TRUE(rel.contains("tag_name"));
    }
}
