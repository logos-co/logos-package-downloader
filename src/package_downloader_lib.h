#pragma once

#include <string>
#include <vector>

class PackageDownloaderLib
{
public:
    PackageDownloaderLib();
    ~PackageDownloaderLib();

    // Release tag (defaults to "latest")
    void setRelease(const std::string& releaseTag);
    std::string release() const { return m_releaseTag; }

    // Package catalog (returns JSON strings)
    std::string getPackages();
    std::string getPackages(const std::string& category);
    std::string getCategories();

    // Dependency resolution (returns JSON array string of resolved names)
    std::string resolveDependencies(const std::vector<std::string>& packageNames);

    // Download (synchronous)
    // Returns path to downloaded file, or empty string on error
    // If outputDir is non-empty, downloads to that directory; otherwise uses system temp dir
    std::string downloadPackage(const std::string& packageName);
    std::string downloadPackage(const std::string& packageName, const std::string& outputDir);
    bool downloadFile(const std::string& url, const std::string& destinationPath);

private:
    std::string m_releaseTag;

    std::string downloadBaseUrl() const;
    std::string fetchPackageListFromOnline();

    std::string findPackageByName(const std::string& packagesJson, const std::string& packageName);
    std::string filterPackagesByCategory(const std::string& packagesJson, const std::string& category);
    std::string extractCategories(const std::string& packagesJson);
    void resolveDependenciesRecursive(const std::string& packageName, const std::string& allPackagesJson,
                                      std::vector<std::string>& processed, std::vector<std::string>& result);

    // HTTP helper (uses libcurl)
    bool httpGet(const std::string& url, std::string& responseBody);
};
