// Thin C ABI wrapper around `lgpd::PackageDownloaderLib`. Kept intentionally
// simple — the heavy lifting lives in `package_downloader_lib.cpp`.

#include "lgpd.h"
#include "package_downloader_lib.h"

#include <cstdlib>
#include <cstring>
#include <string>

static thread_local std::string g_last_error;

static void set_error(const std::string& err) { g_last_error = err; }

static char* to_c_string(const std::string& s) {
    if (s.empty()) return nullptr;
    char* result = static_cast<char*>(std::malloc(s.size() + 1));
    if (result) std::memcpy(result, s.c_str(), s.size() + 1);
    return result;
}

static std::string safeStr(const char* p) { return p ? std::string(p) : std::string(); }

struct lgpd_context_opaque {
    // Use a pointer so the underlying class can hold non-copyable state.
    lgpd::PackageDownloaderLib* lib = nullptr;
};

extern "C" {

lgpd_context_t lgpd_create(void) {
    auto* ctx = new lgpd_context_opaque();
    ctx->lib = new lgpd::PackageDownloaderLib();
    return ctx;
}

lgpd_context_t lgpd_create_with_config(const char* config_path) {
    auto* ctx = new lgpd_context_opaque();
    ctx->lib = new lgpd::PackageDownloaderLib(safeStr(config_path));
    return ctx;
}

void lgpd_free(lgpd_context_t ctx) {
    if (!ctx) return;
    delete ctx->lib;
    delete ctx;
}

static lgpd_result_t makeResult(const std::string& err) {
    lgpd_result_t r;
    if (err.empty()) {
        r.success = true;
        r.error = nullptr;
    } else {
        set_error(err);
        r.success = false;
        r.error = g_last_error.c_str();
    }
    return r;
}

lgpd_result_t lgpd_repo_add(lgpd_context_t ctx, const char* url) {
    if (!ctx || !ctx->lib) return makeResult("invalid context");
    return makeResult(ctx->lib->registry().addRepository(safeStr(url)));
}

lgpd_result_t lgpd_repo_remove(lgpd_context_t ctx, const char* url) {
    if (!ctx || !ctx->lib) return makeResult("invalid context");
    return makeResult(ctx->lib->registry().removeRepository(safeStr(url)));
}

lgpd_result_t lgpd_repo_set_enabled(lgpd_context_t ctx, const char* url, bool enabled) {
    if (!ctx || !ctx->lib) return makeResult("invalid context");
    return makeResult(ctx->lib->registry().setEnabled(safeStr(url), enabled));
}

lgpd_result_t lgpd_repo_refresh(lgpd_context_t ctx) {
    if (!ctx || !ctx->lib) return makeResult("invalid context");
    return makeResult(ctx->lib->refreshCatalogs());
}

char* lgpd_repo_list(lgpd_context_t ctx) {
    if (!ctx || !ctx->lib) { set_error("invalid context"); return nullptr; }
    return to_c_string(ctx->lib->listRepositoriesJson());
}

char* lgpd_get_catalog(lgpd_context_t ctx) {
    if (!ctx || !ctx->lib) { set_error("invalid context"); return nullptr; }
    return to_c_string(ctx->lib->getCatalogJson());
}

char* lgpd_get_catalog_for_repo(lgpd_context_t ctx, const char* repo_url_or_name) {
    if (!ctx || !ctx->lib) { set_error("invalid context"); return nullptr; }
    return to_c_string(ctx->lib->getCatalogForRepoJson(safeStr(repo_url_or_name)));
}

char* lgpd_resolve_dependencies(lgpd_context_t ctx, const char* dependencies_json) {
    if (!ctx || !ctx->lib) { set_error("invalid context"); return nullptr; }
    return to_c_string(ctx->lib->resolveDependenciesJson(safeStr(dependencies_json)));
}

char* lgpd_download_package(lgpd_context_t ctx,
                            const char* repo_url_or_name,
                            const char* package_name,
                            const char* version,
                            const char* root_hash,
                            const char* output_dir) {
    if (!ctx || !ctx->lib || !package_name) {
        set_error("invalid arguments");
        return nullptr;
    }
    std::string path = ctx->lib->downloadPackage(
        safeStr(repo_url_or_name),
        safeStr(package_name),
        safeStr(version),
        safeStr(root_hash),
        safeStr(output_dir));
    if (path.empty()) {
        set_error("download failed for package: " + std::string(package_name));
        return nullptr;
    }
    return to_c_string(path);
}

void lgpd_free_string(char* str) { std::free(str); }

const char* lgpd_get_last_error(void) { return g_last_error.c_str(); }

} // extern "C"
