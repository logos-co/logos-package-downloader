#include "package_downloader_lib.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

static const std::string MODULES_REPO_BASE = "https://github.com/logos-co/logos-modules/releases";
static const std::string MODULES_REPO_API_RELEASES = "https://api.github.com/repos/logos-co/logos-modules/releases";

// libcurl write callback
static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

PackageDownloaderLib::PackageDownloaderLib()
{
}

PackageDownloaderLib::~PackageDownloaderLib()
{
}

std::string PackageDownloaderLib::resolveTag(const std::string& releaseTag)
{
    return releaseTag.empty() ? "latest" : releaseTag;
}

std::string PackageDownloaderLib::downloadBaseUrl(const std::string& releaseTag)
{
    std::string tag = resolveTag(releaseTag);
    if (tag == "latest") {
        return MODULES_REPO_BASE + "/latest/download";
    }
    return MODULES_REPO_BASE + "/download/" + tag;
}

bool PackageDownloaderLib::httpGet(const std::string& url, std::string& responseBody)
{
    return httpGet(url, responseBody, {});
}

bool PackageDownloaderLib::httpGet(const std::string& url, std::string& responseBody,
                                   const std::vector<std::string>& extraHeaders)
{
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    responseBody.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lgpd/1.0");

    struct curl_slist* headerList = nullptr;
    for (const auto& h : extraHeaders) {
        headerList = curl_slist_append(headerList, h.c_str());
    }
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    if (headerList) curl_slist_free_all(headerList);

    return res == CURLE_OK && httpCode >= 200 && httpCode < 300;
}

std::string PackageDownloaderLib::fetchPackageListFromOnline(const std::string& releaseTag)
{
    std::string url = downloadBaseUrl(releaseTag) + "/list.json";
    std::string body;

    if (!httpGet(url, body)) {
        return "[]";
    }

    // Validate it's a JSON array
    try {
        json doc = json::parse(body);
        if (!doc.is_array()) return "[]";
        return body;
    } catch (...) {
        return "[]";
    }
}

std::string PackageDownloaderLib::getPackages(const std::string& releaseTag)
{
    std::string rawJson = fetchPackageListFromOnline(releaseTag);
    json onlinePackages;
    try {
        onlinePackages = json::parse(rawJson);
    } catch (...) {
        return "[]";
    }

    if (onlinePackages.empty()) return "[]";

    json packagesArray = json::array();
    for (const auto& packageObj : onlinePackages) {
        std::string packageFile = packageObj.value("package", "");
        if (packageFile.empty()) continue;

        json resultPackage;
        resultPackage["name"] = packageObj.value("name", "");
        resultPackage["description"] = packageObj.value("description", "");
        resultPackage["type"] = packageObj.value("type", "");
        resultPackage["moduleName"] = packageObj.value("moduleName", "");
        resultPackage["category"] = packageObj.value("category", "");
        resultPackage["author"] = packageObj.value("author", "");
        resultPackage["dependencies"] = packageObj.value("dependencies", json::array());
        resultPackage["package"] = packageFile;
        resultPackage["variants"] = packageObj.value("variants", json::array());
        resultPackage["manifest"] = packageObj.value("manifest", json::object());
        packagesArray.push_back(resultPackage);
    }

    return packagesArray.dump();
}

std::string PackageDownloaderLib::getPackages(const std::string& releaseTag, const std::string& category)
{
    std::string allPackages = getPackages(releaseTag);

    if (category.empty()) return allPackages;

    // Case-insensitive compare for "All"
    std::string catLower = category;
    std::transform(catLower.begin(), catLower.end(), catLower.begin(), ::tolower);
    if (catLower == "all") return allPackages;

    return filterPackagesByCategory(allPackages, category);
}

std::string PackageDownloaderLib::getCategories(const std::string& releaseTag)
{
    std::string packagesJson = fetchPackageListFromOnline(releaseTag);
    return extractCategories(packagesJson);
}

std::string PackageDownloaderLib::getReleases()
{
    std::string body;
    std::vector<std::string> headers = {
        "Accept: application/vnd.github+json",
        "X-GitHub-Api-Version: 2022-11-28"
    };
    if (!httpGet(MODULES_REPO_API_RELEASES, body, headers)) {
        return "[]";
    }

    json doc;
    try {
        doc = json::parse(body);
    } catch (...) {
        return "[]";
    }
    if (!doc.is_array()) return "[]";

    json result = json::array();
    for (const auto& rel : doc) {
        if (rel.value("draft", false)) continue;
        json entry;
        entry["tag_name"] = rel.value("tag_name", "");
        entry["name"] = rel.value("name", "");
        entry["published_at"] = rel.value("published_at", "");
        entry["prerelease"] = rel.value("prerelease", false);
        entry["html_url"] = rel.value("html_url", "");
        result.push_back(entry);
    }
    return result.dump();
}

std::string PackageDownloaderLib::findPackageByName(const std::string& packagesJson, const std::string& packageName)
{
    try {
        json packages = json::parse(packagesJson);
        for (const auto& pkg : packages) {
            if (pkg.value("name", "") == packageName) {
                return pkg.dump();
            }
        }
    } catch (...) {}
    return "{}";
}

std::string PackageDownloaderLib::resolveDependencies(const std::string& releaseTag, const std::vector<std::string>& packageNames)
{
    std::string allPackagesJson = fetchPackageListFromOnline(releaseTag);

    json allPackages;
    try {
        allPackages = json::parse(allPackagesJson);
    } catch (...) {
        json result = json::array();
        for (const auto& name : packageNames) result.push_back(name);
        return result.dump();
    }

    if (allPackages.empty()) {
        json result = json::array();
        for (const auto& name : packageNames) result.push_back(name);
        return result.dump();
    }

    std::vector<std::string> processed;
    std::vector<std::string> result;

    for (const auto& packageName : packageNames) {
        resolveDependenciesRecursive(packageName, allPackagesJson, processed, result);
    }

    json resultJson = json::array();
    for (const auto& name : result) resultJson.push_back(name);
    return resultJson.dump();
}

bool PackageDownloaderLib::downloadFile(const std::string& url, const std::string& destinationPath)
{
    std::string body;
    if (!httpGet(url, body)) return false;

    // Create parent directories
    fs::path destPath(destinationPath);
    std::error_code ec;
    fs::create_directories(destPath.parent_path(), ec);
    if (ec) return false;

    std::ofstream file(destinationPath, std::ios::binary);
    if (!file.is_open()) return false;

    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    return file.good();
}

std::string PackageDownloaderLib::downloadPackage(const std::string& releaseTag, const std::string& packageName)
{
    return downloadPackage(releaseTag, packageName, "");
}

std::string PackageDownloaderLib::downloadPackage(const std::string& releaseTag, const std::string& packageName, const std::string& outputDir)
{
    std::string allPackagesJson = getPackages(releaseTag);
    std::string packageJson = findPackageByName(allPackagesJson, packageName);

    json packageObj;
    try {
        packageObj = json::parse(packageJson);
    } catch (...) {
        return {};
    }

    if (packageObj.empty() || !packageObj.contains("name")) return {};

    std::string packageFile = packageObj.value("package", "");
    if (packageFile.empty()) return {};

    std::string destDir;
    if (!outputDir.empty()) {
        destDir = outputDir;
        std::error_code ec;
        fs::create_directories(destDir, ec);
        if (ec) return {};
    } else {
        const char* tmpDir = std::getenv("TMPDIR");
        if (!tmpDir) tmpDir = "/tmp";
        destDir = tmpDir;
    }

    std::string downloadUrl = downloadBaseUrl(releaseTag) + "/" + packageFile;
    std::string destinationPath = (fs::path(destDir) / packageFile).string();

    if (!downloadFile(downloadUrl, destinationPath)) return {};

    return destinationPath;
}

void PackageDownloaderLib::resolveDependenciesRecursive(const std::string& packageName,
                                                         const std::string& allPackagesJson,
                                                         std::vector<std::string>& processed,
                                                         std::vector<std::string>& result)
{
    if (std::find(processed.begin(), processed.end(), packageName) != processed.end())
        return;

    processed.push_back(packageName);

    std::string packageJson = findPackageByName(allPackagesJson, packageName);
    json packageObj;
    try {
        packageObj = json::parse(packageJson);
    } catch (...) {
        return;
    }

    if (packageObj.empty() || !packageObj.contains("name")) return;

    if (packageObj.contains("dependencies") && packageObj["dependencies"].is_array()) {
        for (const auto& dep : packageObj["dependencies"]) {
            std::string depName = dep.get<std::string>();
            if (!depName.empty()) {
                resolveDependenciesRecursive(depName, allPackagesJson, processed, result);
            }
        }
    }

    if (std::find(result.begin(), result.end(), packageName) == result.end()) {
        result.push_back(packageName);
    }
}

std::string PackageDownloaderLib::filterPackagesByCategory(const std::string& packagesJson, const std::string& category)
{
    json filtered = json::array();
    std::string catLower = category;
    std::transform(catLower.begin(), catLower.end(), catLower.begin(), ::tolower);

    try {
        json packages = json::parse(packagesJson);
        for (const auto& pkg : packages) {
            std::string pkgCat = pkg.value("category", "");
            std::string pkgCatLower = pkgCat;
            std::transform(pkgCatLower.begin(), pkgCatLower.end(), pkgCatLower.begin(), ::tolower);
            if (pkgCatLower == catLower) {
                filtered.push_back(pkg);
            }
        }
    } catch (...) {}

    return filtered.dump();
}

std::string PackageDownloaderLib::extractCategories(const std::string& packagesJson)
{
    std::vector<std::string> categories;

    try {
        json packages = json::parse(packagesJson);
        for (const auto& pkg : packages) {
            std::string cat = pkg.value("category", "");
            if (!cat.empty()) {
                // Capitalize first letter
                if (!cat.empty()) {
                    cat[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cat[0])));
                }
                if (std::find(categories.begin(), categories.end(), cat) == categories.end()) {
                    categories.push_back(cat);
                }
            }
        }
    } catch (...) {}

    std::sort(categories.begin(), categories.end());
    categories.insert(categories.begin(), "All");

    json result = json::array();
    for (const auto& cat : categories) result.push_back(cat);
    return result.dump();
}
