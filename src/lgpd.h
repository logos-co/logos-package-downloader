#ifndef LGPD_H
#define LGPD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-specific export macros */
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef LGPD_BUILD_SHARED
    #ifdef __GNUC__
      #define LGPD_EXPORT __attribute__((dllexport))
    #else
      #define LGPD_EXPORT __declspec(dllexport)
    #endif
  #else
    #ifdef __GNUC__
      #define LGPD_EXPORT __attribute__((dllimport))
    #else
      #define LGPD_EXPORT __declspec(dllimport)
    #endif
  #endif
#else
  #if __GNUC__ >= 4
    #define LGPD_EXPORT __attribute__((visibility("default")))
  #else
    #define LGPD_EXPORT
  #endif
#endif

/* Opaque handle */
typedef struct lgpd_context_opaque* lgpd_context_t;

/* Generic success/error result */
typedef struct {
    bool success;
    const char* error;     /* thread-local; only valid until the next call */
} lgpd_result_t;

/*
 * Context lifecycle
 * ─────────────────
 * `lgpd_create` constructs an in-memory client with no persisted
 * configuration. `lgpd_create_with_config` reads/writes user repositories
 * from `config_path` (`{ repositories[], defaultDisabled }` JSON).
 *
 * The hardcoded default repository is always present, regardless of the
 * config-file state.
 */
LGPD_EXPORT lgpd_context_t lgpd_create(void);
LGPD_EXPORT lgpd_context_t lgpd_create_with_config(const char* config_path);
LGPD_EXPORT void lgpd_free(lgpd_context_t ctx);

/*
 * Repository management
 * ─────────────────────
 * All `repo_*` mutating calls require the context to have been created
 * with a config file path; otherwise the call fails with a clear error.
 */
LGPD_EXPORT lgpd_result_t lgpd_repo_add(lgpd_context_t ctx, const char* url);
LGPD_EXPORT lgpd_result_t lgpd_repo_remove(lgpd_context_t ctx, const char* url);
LGPD_EXPORT lgpd_result_t lgpd_repo_set_enabled(lgpd_context_t ctx, const char* url, bool enabled);
LGPD_EXPORT lgpd_result_t lgpd_repo_refresh(lgpd_context_t ctx);

/*
 * Returns a JSON array string describing every configured repository.
 * Caller frees the returned string with `lgpd_free_string`.
 */
LGPD_EXPORT char* lgpd_repo_list(lgpd_context_t ctx);

/*
 * Catalog
 * ───────
 * `lgpd_get_catalog` returns the merged catalog across all enabled repos
 * as a JSON array (see `index.json` schema in the plan).
 * `lgpd_get_catalog_for_repo` is the single-repo view (URL or canonical
 * name).
 */
LGPD_EXPORT char* lgpd_get_catalog(lgpd_context_t ctx);
LGPD_EXPORT char* lgpd_get_catalog_for_repo(lgpd_context_t ctx, const char* repo_url_or_name);

/*
 * Resolve a JSON-encoded list of dependencies (mirror of manifest format).
 * Returns a JSON array of resolved `{ name, version, rootHash, url,
 * repositoryUrl }` entries in install order, or an entry with `error` when
 * a constraint is unsatisfiable.
 */
LGPD_EXPORT char* lgpd_resolve_dependencies(lgpd_context_t ctx, const char* dependencies_json);

/*
 * Download a package. `version` may be empty (= newest). `root_hash` may be
 * empty (= newest matching version). `repo_url_or_name` may be empty (=
 * search every enabled repo in registry order). `output_dir` may be empty
 * (= system temp). Returns the local path to the downloaded `.lgx`, or
 * NULL on error. Caller frees with `lgpd_free_string`.
 */
LGPD_EXPORT char* lgpd_download_package(lgpd_context_t ctx,
                                        const char* repo_url_or_name,
                                        const char* package_name,
                                        const char* version,
                                        const char* root_hash,
                                        const char* output_dir);

/* Memory management */
LGPD_EXPORT void lgpd_free_string(char* str);

/* Error handling (thread-local) */
LGPD_EXPORT const char* lgpd_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* LGPD_H */
