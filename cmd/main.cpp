#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "package_downloader_lib.h"

using json = nlohmann::json;

static void printPackageTable(const json& packages) {
    printf("%-30s %-15s %-10s %-40s\n", "NAME", "CATEGORY", "TYPE", "DESCRIPTION");
    std::cout << std::string(95, '-') << "\n";

    for (const auto& pkg : packages) {
        std::string desc = pkg.value("description", "");
        if (desc.size() > 40) desc = desc.substr(0, 40);
        printf("%-30s %-15s %-10s %-40s\n",
               pkg.value("name", "").c_str(),
               pkg.value("category", "").c_str(),
               pkg.value("type", "").c_str(),
               desc.c_str());
    }
}

static int cmdSearch(PackageDownloaderLib& dl, const std::string& query, bool jsonOutput) {
    std::string allPackagesJson = dl.getPackages();
    json allPackages = json::parse(allPackagesJson);
    json results = json::array();

    std::string queryLower = query;
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::tolower);

    for (const auto& pkg : allPackages) {
        std::string name = pkg.value("name", "");
        std::string description = pkg.value("description", "");
        std::string nameLower = name, descLower = description;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
        std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::tolower);

        if (nameLower.find(queryLower) != std::string::npos ||
            descLower.find(queryLower) != std::string::npos) {
            results.push_back(pkg);
        }
    }

    if (results.empty()) {
        std::cout << "No packages found matching '" << query << "'\n";
        return 0;
    }

    if (jsonOutput) {
        std::cout << results.dump(2) << "\n";
    } else {
        std::cout << "Found " << results.size() << " package(s) matching '" << query << "':\n\n";
        printPackageTable(results);
    }

    return 0;
}

static int cmdList(PackageDownloaderLib& dl, const std::string& category, bool jsonOutput) {
    std::string packagesJson;
    if (category.empty()) {
        packagesJson = dl.getPackages();
    } else {
        packagesJson = dl.getPackages(category);
    }

    json packages = json::parse(packagesJson);

    if (packages.empty()) {
        std::cout << "No packages found\n";
        return 0;
    }

    if (jsonOutput) {
        std::cout << packages.dump(2) << "\n";
    } else {
        std::cout << "Found " << packages.size() << " package(s):\n\n";
        printPackageTable(packages);
    }

    return 0;
}

static int cmdCategories(PackageDownloaderLib& dl, bool jsonOutput) {
    std::string categoriesJson = dl.getCategories();
    json categories = json::parse(categoriesJson);

    if (jsonOutput) {
        std::cout << categories.dump(2) << "\n";
    } else {
        std::cout << "Available categories:\n";
        for (const auto& cat : categories) {
            std::cout << "  " << cat.get<std::string>() << "\n";
        }
    }

    return 0;
}

static int cmdInfo(PackageDownloaderLib& dl, const std::string& packageName, bool jsonOutput) {
    std::string allPackagesJson = dl.getPackages();
    json allPackages = json::parse(allPackagesJson);

    json found;
    for (const auto& pkg : allPackages) {
        if (pkg.value("name", "") == packageName) {
            found = pkg;
            break;
        }
    }

    if (found.is_null()) {
        std::cerr << "Error: package '" << packageName << "' not found\n";
        return 1;
    }

    if (jsonOutput) {
        std::cout << found.dump(2) << "\n";
    } else {
        std::cout << "Name: " << found.value("name", "") << "\n";
        std::cout << "Description: " << found.value("description", "") << "\n";
        std::cout << "Category: " << found.value("category", "") << "\n";
        std::cout << "Type: " << found.value("type", "") << "\n";
        std::cout << "Author: " << found.value("author", "") << "\n";
        std::cout << "Module Name: " << found.value("moduleName", "") << "\n";

        if (found.contains("dependencies") && found["dependencies"].is_array() && !found["dependencies"].empty()) {
            std::cout << "Dependencies: ";
            bool first = true;
            for (const auto& d : found["dependencies"]) {
                if (!first) std::cout << ", ";
                std::cout << d.get<std::string>();
                first = false;
            }
            std::cout << "\n";
        } else {
            std::cout << "Dependencies: none\n";
        }
    }

    return 0;
}

static int cmdDownload(PackageDownloaderLib& dl, const std::string& packageName, const std::string& outputPath) {
    std::cout << "Downloading package: " << packageName << "..." << std::flush;

    std::string filePath;
    if (!outputPath.empty()) {
        filePath = dl.downloadPackage(packageName, outputPath);
    } else {
        filePath = dl.downloadPackage(packageName);
    }
    if (filePath.empty()) {
        std::cout << " FAILED\n";
        return 1;
    }

    std::cout << " done\n";
    std::cout << "Downloaded to: " << filePath << "\n";
    return 0;
}

static void printHelp() {
    std::cout << "lgpd - Logos Package Downloader CLI\n"
              << "\n"
              << "Usage: lgpd [options] <command> [arguments]\n"
              << "\n"
              << "Commands:\n"
              << "  search <query>          Search packages by name or description\n"
              << "  list                    List all available packages\n"
              << "  categories              List available categories\n"
              << "  info <package>          Show package details from catalog\n"
              << "  download <package>      Download .lgx package\n"
              << "\n"
              << "Options:\n"
              << "  --release <tag>         GitHub release tag to use (default: latest)\n"
              << "  --category <cat>        Filter by category (for list command)\n"
              << "  -o, --output <path>     Output directory for download (default: system temp)\n"
              << "  --json                  Output in JSON format\n"
              << "  -h, --help              Show this help message\n"
              << "  -v, --version           Show version information\n";
}

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    std::string releaseTag, categoryFilter, outputPath, command;
    std::vector<std::string> positionalArgs;
    bool jsonOutput = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-h" || args[i] == "--help") {
            printHelp();
            return 0;
        } else if (args[i] == "-v" || args[i] == "--version") {
            std::cout << "lgpd version 1.0.0\n";
            return 0;
        } else if (args[i] == "--release" && i + 1 < args.size()) {
            releaseTag = args[++i];
        } else if (args[i] == "--category" && i + 1 < args.size()) {
            categoryFilter = args[++i];
        } else if ((args[i] == "-o" || args[i] == "--output") && i + 1 < args.size()) {
            outputPath = args[++i];
        } else if (args[i] == "--json") {
            jsonOutput = true;
        } else {
            positionalArgs.push_back(args[i]);
        }
    }

    if (positionalArgs.empty()) {
        printHelp();
        return 1;
    }

    command = positionalArgs[0];
    positionalArgs.erase(positionalArgs.begin());

    PackageDownloaderLib dl;

    if (!releaseTag.empty())
        dl.setRelease(releaseTag);

    if (command == "search") {
        if (positionalArgs.empty()) {
            std::cerr << "Error: search requires a query argument\n";
            return 1;
        }
        return cmdSearch(dl, positionalArgs[0], jsonOutput);
    } else if (command == "list") {
        return cmdList(dl, categoryFilter, jsonOutput);
    } else if (command == "categories") {
        return cmdCategories(dl, jsonOutput);
    } else if (command == "info") {
        if (positionalArgs.empty()) {
            std::cerr << "Error: info requires a package name\n";
            return 1;
        }
        return cmdInfo(dl, positionalArgs[0], jsonOutput);
    } else if (command == "download") {
        if (positionalArgs.empty()) {
            std::cerr << "Error: download requires a package name\n";
            return 1;
        }
        return cmdDownload(dl, positionalArgs[0], outputPath);
    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        std::cerr << "Run 'lgpd --help' for usage information\n";
        return 1;
    }
}
