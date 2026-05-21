#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lgpd {

/// Default repository URL (points to logos-co's logos-repo.json). Treated as
/// an opaque string by the rest of the code: the only reason it is wired up
/// at compile time is so the client always has *something* to talk to, even
/// when no config file is provided.
extern const char* kDefaultRepositoryUrl;

/// Minimal HTTP(S) fetcher abstraction. The concrete implementation in the
/// .cpp uses libcurl. Tests can inject their own implementation.
class Fetcher {
public:
    virtual ~Fetcher() = default;

    /// HTTP GET. Returns true on 2xx with the response body in `out`.
    virtual bool get(const std::string& url, std::string& out) = 0;

    /// HTTP GET to a file. Returns true on 2xx after writing all bytes.
    virtual bool getToFile(const std::string& url, const std::string& path) = 0;
};

/// One repository entry in the in-memory registry. The persisted form is
/// just `{ url, enabled }`; everything else is fetched at runtime from
/// `logos-repo.json`.
struct Repository {
    std::string url;          ///< URL of `logos-repo.json`
    bool enabled = true;
    bool isDefault = false;   ///< Synthesised at runtime, never persisted.

    // Resolved at runtime from logos-repo.json. Empty when fetch failed.
    std::string name;         ///< canonical id
    std::string displayName;
    std::string description;
    std::string homepage;
    std::string indexUrl;
    std::vector<std::string> trustedSignerDids;
    std::string resolveError; ///< non-empty when the fetch / parse failed
};

/// Registry of repositories. Persists `{ url, enabled }` plus a
/// `defaultDisabled` flag. The default repo is ALWAYS present in the
/// merged view; only user repos are written to disk.
class RepositoryRegistry {
public:
    /// Construct an in-memory registry with no config-file backing. Mutating
    /// methods (add/remove/setEnabled for user repos) will return an error.
    /// The default repo is always present.
    RepositoryRegistry();

    /// Construct backed by a JSON config file. The file is loaded if it
    /// exists. Missing parent dirs are created lazily on save.
    explicit RepositoryRegistry(std::string configPath);

    ~RepositoryRegistry();

    /// Set the fetcher used to populate resolved metadata. Defaults to a
    /// libcurl-backed `HttpsFetcher`.
    void setFetcher(std::shared_ptr<Fetcher> fetcher);

    /// Returns the in-memory list, default-first then user repos in
    /// declared order. Each entry has its `enabled` flag and its resolved
    /// metadata (if `refresh()` has been called and the fetch succeeded).
    std::vector<Repository> list() const;

    /// Add a user repo by URL. The URL must point to a `logos-repo.json`
    /// (or wherever the client can fetch one). On success persists the
    /// updated config to disk. Returns an empty string on success, or an
    /// error message.
    std::string addRepository(const std::string& url);

    /// Remove a user repo by URL. Cannot remove the default. Persists on
    /// success.
    std::string removeRepository(const std::string& url);

    /// Enable or disable a repo. Allowed for any entry. Toggling the
    /// default sets the `defaultDisabled` flag in the config file.
    std::string setEnabled(const std::string& url, bool enabled);

    /// Re-fetch `logos-repo.json` for every repo (default + user). After
    /// calling, `list()` returns entries with resolved metadata fields
    /// populated. Returns an empty string on overall success or a summary
    /// of fetch errors (the registry is best-effort: failures are recorded
    /// per-entry in `resolveError` but do not abort).
    std::string refresh();

    /// Look up a repo by URL or by canonical name. Returns nullopt if no
    /// match.
    std::optional<Repository> findByUrlOrName(const std::string& urlOrName) const;

    /// Whether the registry was constructed with a config-file path.
    bool isPersistent() const;

    /// Path to the backing config file, or empty when in-memory only.
    std::string configPath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// High-level operations on top of `RepositoryRegistry`.
///
/// The public API speaks JSON strings rather than typed structs so it is
/// trivially callable from a C wrapper, the package-downloader Logos
/// module, and tests. The JSON shapes mirror the schemas documented in
/// the plan (`index.json#packages[]` + a few synthesised fields).
class PackageDownloaderLib {
public:
    PackageDownloaderLib();
    explicit PackageDownloaderLib(std::string configPath);
    ~PackageDownloaderLib();

    PackageDownloaderLib(const PackageDownloaderLib&) = delete;
    PackageDownloaderLib& operator=(const PackageDownloaderLib&) = delete;

    void setFetcher(std::shared_ptr<Fetcher> fetcher);

    /// Returns the registry (mutable).
    RepositoryRegistry& registry();
    const RepositoryRegistry& registry() const;

    /// JSON array of repositories. Each element:
    /// `{ url, enabled, isDefault, name, displayName, description,
    ///    homepage, indexUrl, trustedSignerDids[], resolveError }`.
    std::string listRepositoriesJson();

    /// JSON array of all packages across all enabled repos. Each element:
    /// `{ repositoryUrl, repositoryName, name, versions: [...] }`.
    /// `versions[]` is sorted newest-first by `releasedAt` and contains
    /// `releasedAt, publisherRef, url, size, sha256, rootHash, manifest,
    /// signature?` exactly as in `index.json`.
    std::string getCatalogJson();

    /// JSON array of all packages for one repo (URL or canonical name).
    /// Same shape as `getCatalogJson()` filtered to one source.
    std::string getCatalogForRepoJson(const std::string& urlOrName);

    /// Force re-fetch of every enabled repo's `index.json`. Returns an
    /// empty string on success or a summary of errors.
    std::string refreshCatalogs();

    /// Download a package. If `rootHash` is empty and multiple entries
    /// share the same `version`, pick the newest by `releasedAt`. If
    /// `version` is empty, pick the newest version. `repoUrlOrName` may be
    /// empty to mean "any enabled repo, in registry order".
    /// Returns the local path to the downloaded `.lgx`, or empty on error.
    std::string downloadPackage(const std::string& repoUrlOrName,
                                const std::string& packageName,
                                const std::string& version = "",
                                const std::string& rootHash = "",
                                const std::string& outputDir = "");

    /// Cross-repo dependency resolution.
    ///
    /// Given a starting package's dependency list (one element per
    /// dependency, in the Dependency JSON form described by the manifest
    /// schema), returns a JSON array of resolved versions:
    ///   `[{ repositoryUrl, name, version, rootHash, url, topLevel }, ...]`
    /// in install order (deps before dependents, no duplicates).
    /// `topLevel: true` marks entries that came from the input array (the
    /// packages the caller explicitly requested); other entries are
    /// transitive deps the resolver pulled in.
    ///
    /// `installedPackagesJson` is an optional `[{ name, version, rootHash }, ...]`
    /// describing what's currently on disk. When supplied, the resolver
    /// uses it to short-circuit TRANSITIVE deps that are already
    /// satisfied: if an installed copy's version meets the dep's
    /// range, that dep is omitted from the output entirely (no install
    /// or change needed). Top-level entries (from the input array) are
    /// always resolved against the catalog — the caller picked them
    /// explicitly. Empty/missing installedPackagesJson disables the
    /// short-circuit and reproduces the pre-installed-aware behaviour
    /// (every transitive dep resolves to a catalog pick).
    ///
    /// When a constraint cannot be satisfied, an entry of the form
    /// `{ error: "...", name: "..." }` is included at the unsatisfied
    /// position and resolution stops; callers should check for `error`.
    std::string resolveDependenciesJson(const std::string& dependenciesJson,
                                        const std::string& installedPackagesJson = "");

    /// True if the given semver range matches the given concrete version.
    /// Exposed for tests and for callers that want to filter without going
    /// through the full resolver.
    static bool semverMatches(const std::string& range, const std::string& version);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lgpd

// ─────────────────────────────────────────────────────────────────────────────
// Backwards-compat C++ alias.
// ─────────────────────────────────────────────────────────────────────────────
// The historical public class name was `::PackageDownloaderLib`. The Logos
// module wrapper and existing CLI code use that. We re-export it here so the
// rewrite can land without breaking compile of every consumer in the same
// commit. New code should use `lgpd::PackageDownloaderLib`.
using ::lgpd::PackageDownloaderLib;
