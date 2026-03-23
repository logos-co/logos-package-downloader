#include <gtest/gtest.h>
#include "package_downloader_lib.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

TEST(DownloaderTest, DefaultReleaseIsLatest) {
    PackageDownloaderLib dl;
    EXPECT_EQ(dl.release(), "latest");
}

TEST(DownloaderTest, SetRelease) {
    PackageDownloaderLib dl;
    dl.setRelease("v2.0.0");
    EXPECT_EQ(dl.release(), "v2.0.0");
}

TEST(DownloaderTest, SetEmptyReleaseFallsBackToLatest) {
    PackageDownloaderLib dl;
    dl.setRelease("");
    EXPECT_EQ(dl.release(), "latest");
}

TEST(DownloaderTest, ResolveDependenciesReturnsInputOnEmptyPackageList) {
    // When the catalog can't be fetched, resolveDependencies should
    // return the requested names as-is.
    PackageDownloaderLib dl;
    // Use a non-existent release to force catalog fetch failure
    dl.setRelease("__nonexistent_release_tag__");

    std::vector<std::string> names = {"pkg_a", "pkg_b"};
    std::string result = dl.resolveDependencies(names);
    json resolved = json::parse(result);

    ASSERT_TRUE(resolved.is_array());
    EXPECT_EQ(resolved.size(), 2);
    EXPECT_EQ(resolved[0], "pkg_a");
    EXPECT_EQ(resolved[1], "pkg_b");
}

TEST(DownloaderTest, DownloadPackageReturnsEmptyForNonExistent) {
    PackageDownloaderLib dl;
    dl.setRelease("__nonexistent_release_tag__");

    std::string result = dl.downloadPackage("nonexistent_package_xyz");
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
    dl.setRelease("__nonexistent_release_tag__");

    std::string result = dl.getPackages();
    json packages = json::parse(result);
    EXPECT_TRUE(packages.is_array());
}

TEST(DownloaderTest, GetCategoriesReturnsJsonArray) {
    PackageDownloaderLib dl;
    dl.setRelease("__nonexistent_release_tag__");

    std::string result = dl.getCategories();
    json cats = json::parse(result);
    EXPECT_TRUE(cats.is_array());
    // Should at least contain "All"
    ASSERT_FALSE(cats.empty());
    EXPECT_EQ(cats[0], "All");
}

TEST(DownloaderTest, GetPackagesByCategoryReturnsJsonArray) {
    PackageDownloaderLib dl;
    dl.setRelease("__nonexistent_release_tag__");

    std::string result = dl.getPackages("networking");
    json packages = json::parse(result);
    EXPECT_TRUE(packages.is_array());
}
