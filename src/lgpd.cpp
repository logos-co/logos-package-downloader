#include "lgpd.h"
#include "package_downloader_lib.h"
#include <cstring>
#include <string>
#include <vector>

static thread_local std::string g_last_error;

static void set_error(const std::string& err) {
    g_last_error = err;
}

static char* to_c_string(const std::string& s) {
    if (s.empty()) return nullptr;
    char* result = static_cast<char*>(malloc(s.size() + 1));
    if (result) {
        memcpy(result, s.c_str(), s.size() + 1);
    }
    return result;
}

struct lgpd_context_opaque {
    PackageDownloaderLib lib;
};

extern "C" {

lgpd_context_t lgpd_create(void) {
    return new lgpd_context_opaque();
}

void lgpd_free(lgpd_context_t ctx) {
    delete ctx;
}

void lgpd_set_release(lgpd_context_t ctx, const char* release_tag) {
    if (ctx && release_tag) ctx->lib.setRelease(release_tag);
}

char* lgpd_get_packages(lgpd_context_t ctx) {
    if (!ctx) { set_error("Invalid context"); return nullptr; }
    return to_c_string(ctx->lib.getPackages());
}

char* lgpd_get_packages_by_category(lgpd_context_t ctx, const char* category) {
    if (!ctx) { set_error("Invalid context"); return nullptr; }
    return to_c_string(ctx->lib.getPackages(category ? category : ""));
}

char* lgpd_get_categories(lgpd_context_t ctx) {
    if (!ctx) { set_error("Invalid context"); return nullptr; }
    return to_c_string(ctx->lib.getCategories());
}

char* lgpd_resolve_dependencies(lgpd_context_t ctx, const char** names, size_t count) {
    if (!ctx) { set_error("Invalid context"); return nullptr; }

    std::vector<std::string> nameVec;
    for (size_t i = 0; i < count; ++i) {
        if (names[i]) nameVec.push_back(names[i]);
    }

    return to_c_string(ctx->lib.resolveDependencies(nameVec));
}

char* lgpd_download_package(lgpd_context_t ctx, const char* name) {
    if (!ctx || !name) { set_error("Invalid arguments"); return nullptr; }

    std::string result = ctx->lib.downloadPackage(name);
    if (result.empty()) {
        set_error("Failed to download package: " + std::string(name));
        return nullptr;
    }
    return to_c_string(result);
}

lgpd_result_t lgpd_download_file(lgpd_context_t ctx, const char* url, const char* dest_path) {
    lgpd_result_t result = {false, nullptr};
    if (!ctx || !url || !dest_path) {
        set_error("Invalid arguments");
        result.error = g_last_error.c_str();
        return result;
    }

    if (ctx->lib.downloadFile(url, dest_path)) {
        result.success = true;
    } else {
        set_error("Failed to download file from: " + std::string(url));
        result.error = g_last_error.c_str();
    }
    return result;
}

void lgpd_free_string(char* str) {
    free(str);
}

const char* lgpd_get_last_error(void) {
    return g_last_error.c_str();
}

} // extern "C"
