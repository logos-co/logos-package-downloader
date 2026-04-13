#pragma once

#include <string>
#include <vector>

class PackageDownloaderLib
{
public:
    PackageDownloaderLib();
    ~PackageDownloaderLib();

    // Package catalog (returns JSON strings)
    // releaseTag: GitHub release tag; empty string resolves to "latest"
    std::string getPackages(const std::string& releaseTag);
    std::string getPackages(const std::string& releaseTag, const std::string& category);
    std::string getCategories(const std::string& releaseTag);

    // GitHub releases (returns JSON array string of releases, up to 30 most recent)
    // Each entry: { tag_name, name, published_at, prerelease, html_url }
    std::string getReleases();

    // Dependency resolution (returns JSON array string of resolved names)
    std::string resolveDependencies(const std::string& releaseTag, const std::vector<std::string>& packageNames);

    // Download (synchronous)
    // Returns path to downloaded file, or empty string on error
    // If outputDir is non-empty, downloads to that directory; otherwise uses system temp dir
    std::string downloadPackage(const std::string& releaseTag, const std::string& packageName);
    std::string downloadPackage(const std::string& releaseTag, const std::string& packageName, const std::string& outputDir);
    bool downloadFile(const std::string& url, const std::string& destinationPath);

private:
    static std::string resolveTag(const std::string& releaseTag);
    static std::string downloadBaseUrl(const std::string& releaseTag);
    std::string fetchPackageListFromOnline(const std::string& releaseTag);

    std::string findPackageByName(const std::string& packagesJson, const std::string& packageName);
    std::string filterPackagesByCategory(const std::string& packagesJson, const std::string& category);
    std::string extractCategories(const std::string& packagesJson);
    void resolveDependenciesRecursive(const std::string& packageName, const std::string& allPackagesJson,
                                      std::vector<std::string>& processed, std::vector<std::string>& result);

    // HTTP helper (uses libcurl)
    bool httpGet(const std::string& url, std::string& responseBody);
    bool httpGet(const std::string& url, std::string& responseBody,
                 const std::vector<std::string>& extraHeaders);
};
