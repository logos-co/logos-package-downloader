#ifndef LGPD_H
#define LGPD_H

#include <stddef.h>
#include <stdbool.h>

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

/* Result type */
typedef struct {
    bool success;
    const char* error;
} lgpd_result_t;

/* Context lifecycle */
LGPD_EXPORT lgpd_context_t lgpd_create(void);
LGPD_EXPORT void lgpd_free(lgpd_context_t ctx);

/**
 * Get package catalog (returns JSON array string, caller frees with lgpd_free_string).
 * release_tag: GitHub release tag; NULL or empty string resolves to "latest".
 */
LGPD_EXPORT char* lgpd_get_packages(lgpd_context_t ctx, const char* release_tag);
LGPD_EXPORT char* lgpd_get_packages_by_category(lgpd_context_t ctx, const char* release_tag, const char* category);
LGPD_EXPORT char* lgpd_get_categories(lgpd_context_t ctx, const char* release_tag);

/**
 * Get list of GitHub releases (returns JSON array string, caller frees with lgpd_free_string).
 */
LGPD_EXPORT char* lgpd_get_releases(lgpd_context_t ctx);

/**
 * Resolve dependencies for the given package names.
 * Returns JSON array string of resolved names (caller frees with lgpd_free_string).
 */
LGPD_EXPORT char* lgpd_resolve_dependencies(lgpd_context_t ctx, const char* release_tag, const char** names, size_t count);

/**
 * Download a package by name. Returns path to downloaded file (caller frees with lgpd_free_string),
 * or NULL on error.
 */
LGPD_EXPORT char* lgpd_download_package(lgpd_context_t ctx, const char* release_tag, const char* name);

/**
 * Download a file from URL to destination path.
 */
LGPD_EXPORT lgpd_result_t lgpd_download_file(lgpd_context_t ctx, const char* url, const char* dest_path);

/* Memory management */
LGPD_EXPORT void lgpd_free_string(char* str);

/* Error handling */
LGPD_EXPORT const char* lgpd_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* LGPD_H */
