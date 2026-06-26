# Logos Package Downloader — Project Description

## Overview

`logos-package-downloader` is the **network / download tier** of the Logos
package-management stack. It provides a shared C++ library
(`libpackage_downloader_lib`), a C ABI (`lgpd.h`), and a command-line tool
(`lgpd`) for browsing online catalogs of Logos modules and downloading their
`.lgx` artifacts.

It explicitly does **not** install packages — installation is the job of
[logos-package-manager](https://github.com/logos-co/logos-package-manager)
(`lgpm`). The two repos form a pipeline: `lgpd` fetches a `.lgx` and
verifies it against the catalog entry, then hands the file off to `lgpm` for
trust-checked installation.

The domain model is **multi-repository**. A *repository* is identified by the
URL of its `logos-repo.json` (its identity card: `name`, `displayName`,
`indexUrl`, `trustedSigners[].did`). The library keeps a registry containing a
hardcoded default repo (logos-co's `logos-modules-release`) plus any user-added
repos persisted to a JSON config file. At refresh time it fetches each repo's
`logos-repo.json` to resolve metadata, fetches each `index.json`, and **merges
all enabled repos into one catalog**. Packages are downloaded by their
per-version `url` and verified against the catalog (content `rootHash` +
manifest fields + signer DID) before the local path is returned.

### Place in the Logos dependency chain

```
        logos-package (lgx C library: lgx_verify / lgx_load /
                       lgx_get_manifest_json / lgx_verify_signature)
                              │  (build + runtime dep, via -DLGX_ROOT)
                              ▼
        ┌─────────────────────────────────────────────┐
        │            logos-package-downloader           │
        │                                               │
        │   libpackage_downloader_lib   (C++ core)      │
        │   lgpd.h                       (C ABI)         │
        │   lgpd                         (CLI)          │
        └─────────────────────────────────────────────┘
              │ fetch + verify-against-catalog          ▲
              ▼                                          │ consumes the same
        downloaded *.lgx  ──────────────────────►  logos-package-manager (lgpm)
                                                    installs with deep Ed25519
                                                    trust at install time

   Catalog data (logos-repo.json + index.json) is produced/specified by
   logos-modules-release-tool / logos-modules-release-base.
```

Unlike most repos in the workspace, this is **not** a Qt module — there is no
`metadata.json` and no Qt dependency. It is a plain C++17 library + CLI that
talks only over libcurl and emits JSON strings. That JSON-string public API is
what makes it trivially callable from the C wrapper, the package-downloader
Logos module, and tests.

## Project Structure

```
logos-package-downloader/
├── src/                              # Library sources (the shared object)
│   ├── package_downloader_lib.h      # Primary C++ public header (namespace lgpd):
│   │                                 #   Fetcher, Repository, RepositoryRegistry,
│   │                                 #   PackageDownloaderLib, kDefaultRepositoryUrl,
│   │                                 #   and the ::PackageDownloaderLib compat alias.
│   ├── package_downloader_lib.cpp    # Core implementation: semver matcher, libcurl
│   │                                 #   HttpsFetcher, logos-repo.json/index.json
│   │                                 #   parsers, registry persistence, catalog merge,
│   │                                 #   downloadPackage + verifyDownloadAgainstIndex,
│   │                                 #   BFS dependency resolver.
│   ├── lgpd.h                        # C ABI header (extern "C"): opaque context,
│   │                                 #   lgpd_result_t, create/free, repo mutations,
│   │                                 #   catalog, resolve, download, string/error helpers.
│   └── lgpd.cpp                      # C ABI implementation (wraps PackageDownloaderLib).
│
├── cmd/
│   ├── main.cpp                      # `lgpd` CLI: arg parser + subcommands
│   │                                 #   (list/search/info/download, repo {…}, config {…}).
│   └── CMakeLists.txt                # Builds the lgpd executable; links the lib +
│                                     #   nlohmann_json; RPATH $ORIGIN/../lib (@executable_path).
│
├── tests/
│   ├── test_downloader.cpp           # GoogleTest suite: semver matching, in-memory
│   │                                 #   registry behaviour, config round-trip, catalog
│   │                                 #   returns a JSON array without network.
│   └── CMakeLists.txt                # Builds lgpd_tests, links GTest, gtest_discover_tests.
│
├── nix/
│   ├── default.nix                   # Shared build config: pname/version, nativeBuildInputs,
│   │                                 #   buildInputs, cmakeFlags (-DLGX_ROOT), env.
│   ├── lib.nix                       # Library derivation; installs lib + headers, bundles
│   │                                 #   liblgx alongside for runtime RPATH resolution.
│   ├── cli.nix                       # lgpd CLI derivation; copies the lib alongside;
│   │                                 #   autoPatchelfHook on Linux.
│   └── tests.nix                     # Builds + runs tests via ctest; passes -DLGX_ROOT and
│                                     #   -DLGPD_BUILD_TESTS=ON explicitly.
│
├── assets/
│   ├── lgpd.desktop                  # AppImage desktop entry (used by cli-appimage)
│   └── lgpd.png                      # AppImage icon
│
├── docs/
│   ├── index.md                      # Docs landing page (links spec.md + project.md)
│   ├── spec.md                       # Functional / domain spec
│   └── project.md                    # This file
│
├── CMakeLists.txt                    # Top-level CMake: C++17 SHARED lib package_downloader_lib;
│                                     #   nlohmann_json + CURL + lgx (LGX_ROOT required);
│                                     #   install rules; LGPD_BUILD_TESTS option.
├── flake.nix                         # Nix flake: packages, checks, devShells.
├── flake.lock
└── .github/workflows/ci.yml          # CI: ubuntu + macos; nix build .#lib / .#cli / .#tests -L.
```

## Technology stack

| Name | Type | Purpose |
|------|------|---------|
| C++17 | Language | Library and CLI implementation (`CMAKE_CXX_STANDARD 17`). |
| CMake ≥ 3.14 + Ninja | Build system | Configure/build the shared library + `lgpd` binary. |
| Nix flakes | Packaging | Reproducible builds and portable distribution outputs. |
| libcurl (`CURL::libcurl`) | Library | HTTP(S) GETs of repo metadata/index and streaming `.lgx` downloads (the concrete `HttpsFetcher`). |
| nlohmann_json | Library | Parse/serialise config, `logos-repo.json`, `index.json`, and the JSON-string public API. Found via `find_package`, else `FetchContent` of v3.11.3. |
| lgx C library (logos-package) | Library | Post-download verification: `lgx_verify`, `lgx_load`, `lgx_get_manifest_json`, `lgx_verify_signature`. CMake requires `-DLGX_ROOT`. |
| zstd | Library | Transitive build input (pulled in via the lgx/`.lgx` codec path). |
| pkg-config | Build tool | Locating native dependencies. |
| GoogleTest v1.14.0 | Test framework | Unit tests, driven via CTest; found via `find_package(GTest)` or `FetchContent`. |

### Flake inputs

| Input | Purpose |
|-------|---------|
| `logos-nix` | Provides the `nixpkgs` `follows` and shared Logos Nix infrastructure. |
| `logos-package` | Supplies the lgx C library as `logos-package.packages.<system>.lib` (`logosPackageLib`), wired into CMake via `-DLGX_ROOT`. |
| `nix-bundle-dir` | Permissive bundler producing the self-contained `cli-bundle-dir` output. |
| `nix-bundle-appimage` | Builds the single-file Linux `cli-appimage` output. |

### Flake outputs (`flake.nix`)

| Output | Kind | Notes |
|--------|------|-------|
| `default` | `symlinkJoin` of `lib` + `cli` | Combined library + CLI (what bare `nix build` produces). |
| `lib` / `logos-package-downloader-lib` | derivation | Shared library + headers; bundles `liblgx` alongside. |
| `cli` / `logos-package-downloader-cli` | derivation | `lgpd` binary + lib; `autoPatchelfHook` on Linux. |
| `cli-bundle-dir` | bundle | Self-contained flat dir (`bin/` + `lib/`), Linux + macOS. |
| `cli-appimage` | bundle | Single-file `.AppImage`, **Linux only** (`optionalAttrs isLinux`). |
| `tests` (package + check) | derivation | Builds and runs the GoogleTest suite via ctest. |
| `devShells.default` | shell | cmake, ninja, pkg-config, nlohmann_json, curl, zstd, lgx. |

## Components

### `lgpd::PackageDownloaderLib` (high-level C++ client)

Defined in `src/package_downloader_lib.h`. Non-copyable. Layers catalog,
download, and resolution operations over `RepositoryRegistry`. Two
constructors:

```cpp
PackageDownloaderLib();                          // in-memory (default repo only)
explicit PackageDownloaderLib(std::string configPath);  // + persisted user repos
```

Also re-exported as `::PackageDownloaderLib` for backward compatibility (the
Logos module wrapper and existing CLI code use that name).

| Method | Description |
|--------|-------------|
| `void setFetcher(std::shared_ptr<lgpd::Fetcher>)` | Inject a custom `Fetcher` (default libcurl `HttpsFetcher`); clears caches and forces metadata re-resolution. |
| `RepositoryRegistry& registry()` / `const RepositoryRegistry& registry() const` | Access the underlying registry (mutable / const). |
| `std::string listRepositoriesJson()` | JSON array: `[{url, enabled, isDefault, name, displayName, description, homepage, indexUrl, trustedSignerDids[], resolveError}]`. |
| `std::string getCatalogJson()` | Merged catalog JSON across enabled repos: `[{repositoryUrl, repositoryName, repositoryDisplayName, name, description, type, category, author, icon, versions:[...]}]`; `versions[]` newest-first by `releasedAt`. |
| `std::string getCatalogForRepoJson(const std::string& urlOrName)` | Same synthesised shape scoped to one repo (by URL or canonical name). Returns `"[]"` if the repo is not found. |
| `std::string refreshCatalogs()` | Clear caches and force re-fetch of every enabled repo's metadata + `index.json`. Empty string on success, else an error summary. |
| `std::string downloadPackage(repoUrlOrName, packageName, version="", rootHash="", outputDir="")` | Download + verify a `.lgx`; returns the local path or empty on error. `repo` empty = any enabled repo (registry order); `version` empty = newest; `rootHash` disambiguates same-version builds; `outputDir` empty = system temp. |
| `std::string resolveDependenciesJson(dependenciesJson, installedPackagesJson="")` | Cross-repo BFS resolver. Output is a JSON array in install order: `[{repositoryUrl, name, version, rootHash, url, topLevel}]`; on failure an `{error, name}` entry at the unsatisfied position. The installed-state snapshot (`[{name, version, rootHash}]`) short-circuits already-satisfied transitive deps. |
| `static bool semverMatches(const std::string& range, const std::string& version)` | True if the semver range matches the concrete version. Empty range matches anything. Exposed for tests/filtering. |

### `lgpd::RepositoryRegistry`

Defined in `src/package_downloader_lib.h`. Manages the default repo + persisted
user repos. The default repo is **always present** in the merged view; only
user repos are written to disk. Mutations return a **non-empty error string on
failure** and persist only when the registry was constructed with a config
path.

```cpp
RepositoryRegistry();                            // in-memory; user mutations error
explicit RepositoryRegistry(std::string configPath);  // persisted
```

| Method | Description |
|--------|-------------|
| `void setFetcher(std::shared_ptr<Fetcher>)` | Set the metadata fetcher (defaults to `HttpsFetcher`). |
| `std::vector<Repository> list() const` | Default-first, then user repos in declared order; each with its `enabled` flag and resolved metadata. |
| `std::string addRepository(const std::string& url)` | Add a user repo by its `logos-repo.json` URL; resolves metadata and persists. Empty string = success. |
| `std::string removeRepository(const std::string& url)` | Remove a user repo (cannot remove the default). Persists on success. |
| `std::string setEnabled(const std::string& url, bool enabled)` | Enable/disable any repo. Toggling the default sets the `defaultDisabled` config flag. |
| `std::string refresh()` | Re-fetch `logos-repo.json` for every repo. Best-effort: per-entry failures land in `resolveError`, never abort. |
| `std::optional<Repository> findByUrlOrName(const std::string& s) const` | Look up by URL or canonical name. |
| `bool isPersistent() const` | True if constructed with a config-file path. |
| `std::string configPath() const` | The backing config path, or empty when in-memory. |

`extern const char* lgpd::kDefaultRepositoryUrl` is the compile-time default
repo URL
(`https://raw.githubusercontent.com/logos-co/logos-modules-release/refs/heads/main/logos-repo.json`).

### `lgpd::Fetcher` (HTTP abstraction)

```cpp
class Fetcher {
    virtual bool get(const std::string& url, std::string& out) = 0;       // GET into a string
    virtual bool getToFile(const std::string& url, const std::string& path) = 0;  // GET to a file
};
```

The concrete `HttpsFetcher` in the `.cpp` uses libcurl; tests inject their own.

### `lgpd::Repository` (struct)

```cpp
struct Repository {
    std::string url;          // URL of logos-repo.json
    bool enabled = true;
    bool isDefault = false;   // synthesised at runtime, never persisted
    // resolved at runtime from logos-repo.json (empty when fetch failed):
    std::string name, displayName, description, homepage, indexUrl;
    std::vector<std::string> trustedSignerDids;
    std::string resolveError; // non-empty when the fetch/parse failed
};
```

### C ABI (`src/lgpd.h`)

A C-compatible wrapper for non-C++ consumers. All returned `char*` are owned by
the caller — free with `lgpd_free_string`. `lgpd_result_t` is
`{ bool success; const char* error; }` where `error` is thread-local and only
valid until the next call.

| Function | Description |
|----------|-------------|
| `lgpd_context_t lgpd_create(void)` | Create an in-memory context (default repo only). |
| `lgpd_context_t lgpd_create_with_config(const char* config_path)` | Create a context backed by a persisted config file. |
| `void lgpd_free(lgpd_context_t)` | Destroy a context. |
| `lgpd_result_t lgpd_repo_add(ctx, url)` | Add a user repo (requires a config-path context). |
| `lgpd_result_t lgpd_repo_remove(ctx, url)` | Remove a user repo. |
| `lgpd_result_t lgpd_repo_set_enabled(ctx, url, bool)` | Enable/disable a repo. |
| `lgpd_result_t lgpd_repo_refresh(ctx)` | Re-fetch metadata + indexes. |
| `char* lgpd_repo_list(ctx)` | JSON array describing every configured repo. |
| `char* lgpd_get_catalog(ctx)` | Merged catalog JSON across enabled repos. |
| `char* lgpd_get_catalog_for_repo(ctx, repo_url_or_name)` | Single-repo catalog JSON. |
| `char* lgpd_resolve_dependencies(ctx, dependencies_json)` | Resolved install-order array (or an `error` entry). |
| `char* lgpd_download_package(ctx, repo_url_or_name, package_name, version, root_hash, output_dir)` | Local `.lgx` path or `NULL` on error. |
| `void lgpd_free_string(char* str)` | Free a returned string. |
| `const char* lgpd_get_last_error(void)` | Thread-local last-error accessor. |

### `lgpd` CLI (`cmd/main.cpp`)

A thin front-end over `PackageDownloaderLib`. The arg parser collects global
flags into a `CliOpts` struct, then dispatches the first positional as the
command. There are **no** `--release` or `categories` features (the workspace
`CLAUDE.md` `lgpd` section documents an older interface — `cmd/main.cpp` and the
README are authoritative). The version is build-derived: `lgpd version <v>`
followed by this repo's commit (with a `-dirty` marker) and the locked commits
of the flake inputs, all baked in at build time by `nix/build-info.nix`. `<v>`
is the `VERSION` file contents on a release branch, `pre-release-<sha7>` on a
clean master build, or `dev` for a dirty local build.

#### Catalog commands

| Command | Signature | Behaviour |
|---------|-----------|-----------|
| `list` | `lgpd [--repo <url-or-name>] [--category <cat>] [--json] list` | List all packages in the merged catalog (or one repo with `--repo`). `--category` filters case-insensitively. `--json` emits structured JSON; otherwise a `NAME / CATEGORY / TYPE / DESCRIPTION` table. Empty result prints `No packages found`. |
| `search` | `lgpd [--repo <url-or-name>] [--json] search <query>` | Case-insensitive substring search over package `name` and `description`. Honours `--repo`. Exit 1 if the query is missing. |
| `info` | `lgpd [--repo <url-or-name>] [--json] info <package>` | Show one package's details, then all versions newest-first — each line is `version  releasedAt  shortRootHash  [signed]/[unsigned]`. Exit 1 if not found. |
| `download` | `lgpd [--repo <url-or-name>] [--version <ver>] [--root-hash <hex>] [-o\|--output <dir>] download <package>` | Download a `.lgx`. `repo` empty = any enabled; `version` empty = newest; `root-hash` disambiguates same-version builds; `output` empty = system temp (`TMPDIR` or `/tmp`). Verifies against the catalog before printing the local path. Exit 1 on failure (prints `FAILED`). |

#### Repository management (`repo` mutations require `--config <path>`)

| Command | Signature | Behaviour |
|---------|-----------|-----------|
| `repo list` | `lgpd [--config <path>] [--json] repo list` | Show configured repositories with `[default]` / `[disabled]` / `[error: …]` tags, plus name, displayName, url, indexUrl. |
| `repo add` | `lgpd --config <path> repo add <url>` | Add a user repo by its `logos-repo.json` URL; resolves metadata and persists. Requires `--config` (else exit 1). |
| `repo remove` | `lgpd --config <path> repo remove <url>` | Remove a user repo (cannot remove default). Requires `--config`. |
| `repo enable` | `lgpd --config <path> repo enable <url>` | Enable a repo (default toggles the `defaultDisabled` flag). Requires `--config`. |
| `repo disable` | `lgpd --config <path> repo disable <url>` | Disable a repo. Requires `--config`. |
| `repo refresh` | `lgpd [--config <path>] repo refresh` | Re-fetch every repo's `logos-repo.json` + `index.json` (clears caches). Prints fetch errors to stderr, then `Refreshed.`. |

#### Config

| Command | Signature | Behaviour |
|---------|-----------|-----------|
| `config init` | `lgpd config init <path>` | Write an empty config: `{ "schemaVersion": 1, "repositories": [], "defaultDisabled": false }`. Exit 1 if it cannot write. |
| `config show` | `lgpd --config <path> config show` | Print the raw config-file contents. Requires `--config` (else exit 1). |
| `config path` | `lgpd [--config <path>] config path` | Print the current `--config` path, or `(none)`. |

#### Global flags & help

| Flag | Used by | Description |
|------|---------|-------------|
| `--config <path>` | repo / config | Path to `repositories.json` (required for repo mutations and `config show`). |
| `--repo <url-or-name>` | list / search / info / download | Restrict a catalog/download command to one repo. |
| `--version <ver>` | download / info | Pin a specific package version. |
| `--root-hash <hex>` | download | Disambiguate two releases sharing a version. |
| `--category <cat>` | list | Filter by category (case-insensitive). |
| `-o` / `--output <dir>` | download | Output directory. |
| `--json` | all read commands | Emit structured JSON. |
| `-h` / `--help` | — | Print usage (exit 0). |
| `-V` | — | Print the build-derived version banner (`lgpd version <v>` + commit + dependency commits, exit 0). |
| `--version` (bare) | — | With no value following, prints the version like `-V`. With a value, pins a package version (see above). |

## Verification model (`verifyDownloadAgainstIndex`)

After a `.lgx` is fetched, `downloadPackage` runs an index→file binding check
before returning the path. On any mismatch the artifact is deleted and the
download fails:

1. **Structural** — `lgx_verify` confirms the file is a well-formed `.lgx`.
2. **rootHash** — the catalog version's `rootHash` is compared (string equality)
   against the downloaded manifest's `hashes.root` (no recomputation — the
   structural check already validated internal integrity).
3. **Manifest fields** — `name`, `version`, `main`, `dependencies`, and `type`
   in the loaded manifest are bound to what the catalog advertised
   (via `lgx_load` + `lgx_get_manifest_json`).
4. **Signer DID** — `lgx_verify_signature` confirms the artifact's signer DID
   matches the catalog entry.

Deep Ed25519 trust verification against a keyring is **out of scope** here — it
stays `lgpm`'s job at install time. `lgpd` only proves the file is what the
catalog said it was (same DID, manifest, rootHash).

## Config file schema

Only user repos are persisted; resolved metadata is fetched at runtime, never
stored.

```json
{
  "schemaVersion": 1,
  "defaultDisabled": false,
  "repositories": [
    { "url": "https://example.com/my/logos-repo.json", "enabled": true }
  ]
}
```

The default repository can be **disabled** (`defaultDisabled: true`) but is
never written into `repositories[]` and never removed.

## Building and Testing

### Workspace (`ws`) — preferred

```bash
export PATH="/workspace/scripts:$PATH"

ws build logos-package-downloader              # build
ws build logos-package-downloader --auto-local # build with local dep overrides
ws test  logos-package-downloader              # run the nix checks (tests)
ws test  logos-package-downloader --auto-local
```

### Raw Nix

```bash
nix build                  # combined library + CLI (default = symlinkJoin)
nix build '.#lib'          # library only
nix build '.#cli'          # CLI only  →  ./result/bin/lgpd
nix build '.#cli-bundle-dir'   # self-contained flat dir (Linux + macOS)
nix build '.#cli-appimage'     # single-file .AppImage (Linux only)
nix develop                # dev shell: cmake, ninja, pkg-config, nlohmann_json, curl, zstd, lgx
```

> In zsh, quote targets containing `#` (e.g. `'.#cli'`) to prevent glob
> expansion. If flakes aren't enabled globally, add
> `--extra-experimental-features 'nix-command flakes'`.

### Tests

```bash
nix flake check            # run all checks (the test suite)
nix build .#tests          # build and run tests
nix build .#tests -L       # as run in CI (with full logs)
```

The test suite (`tests/test_downloader.cpp`, GoogleTest via CTest) covers:

| Test | Coverage |
|------|----------|
| `Semver.ExactAndComparator` | Exact match + `>=` / `<` comparators. |
| `Semver.CaretAndTilde` | `^` and `~` range semantics (including `0.x` caret behaviour). |
| `Semver.WildcardAndConjunction` | `*`, `1.x` wildcards, whitespace conjunction (`>=1.0 <2.0`), `\|\|` alternation. |
| `Registry.DefaultIsAlwaysPresent` | An in-memory registry always lists the default repo first. |
| `Registry.MutationsRequireConfig` | `addRepository` on an in-memory client returns a non-empty error. |
| `Registry.RoundTripsConfigFile` | Toggling the default's `enabled` flag persists and reloads from disk. |
| `Catalog.ReturnsJsonArrayWhenNoNetwork` | `getCatalogJson()` parses to a JSON array even with no network (lazy fetch degrades to empty). |

### Raw CMake (inside `nix develop`)

`-DLGX_ROOT` is **required** — CMake reads the variable, not the `LGX_ROOT` env
var:

```bash
cmake -S . -B build -GNinja -DLGX_ROOT=<logos-package-lib-path>
cmake --build build

# with tests
cmake -S . -B build -GNinja -DLGX_ROOT=<path> -DLGPD_BUILD_TESTS=ON
cmake --build build
(cd build && ctest --output-on-failure)
```

### CI

`.github/workflows/ci.yml` runs on an `ubuntu-latest` + `macos-latest` matrix,
installs Nix + Cachix (`logos-co` cache), then runs `nix build .#lib -L`,
`nix build .#cli -L`, and `nix build .#tests -L`.

## Examples

### CLI

```bash
# Browse the merged catalog (no config needed — the default repo is built in)
lgpd list
lgpd search waku
lgpd info wallet_module --json

# Add and manage a custom repository (persisted to the config file)
lgpd config init ~/.config/logos/repositories.json
lgpd --config ~/.config/logos/repositories.json repo add https://example.com/my/logos-repo.json
lgpd --config ~/.config/logos/repositories.json repo list
lgpd --config ~/.config/logos/repositories.json repo refresh

# Download — pin a version, scope to one repo, choose an output dir
lgpd download wallet_module --version 1.0.0 --repo my-catalog -o ./packages/
# → verifies against the catalog (rootHash + manifest + signer), prints the
#   local .lgx path; then hand off to `lgpm` to install.

# Built CLI from a nix build
nix build '.#cli' && ./result/bin/lgpd --help
nix build '.#cli-appimage' && ./result/lgpd.AppImage --help
```

### C++ library

```cpp
#include <package_downloader_lib.h>

// In-memory (default repo only) or backed by a config file (persists user repos).
PackageDownloaderLib dl{"/path/repositories.json"};
dl.registry().addRepository("https://example.com/my/logos-repo.json");

std::string repos   = dl.listRepositoriesJson();   // [{url,enabled,isDefault,name,…}]
std::string catalog = dl.getCatalogJson();          // merged across enabled repos

// Plan an install (no download): resolved versions in install order.
std::string plan = dl.resolveDependenciesJson(R"([{"name":"chat_module"}])");

// Download + verify; returns the local .lgx path or empty on error.
std::string path = dl.downloadPackage("", "wallet_module", "1.0.0", "", "/tmp/pkgs");
```

### C ABI

```c
#include <lgpd.h>

lgpd_context_t ctx = lgpd_create();
char* catalog = lgpd_get_catalog(ctx);
/* ... use catalog ... */
lgpd_free_string(catalog);

char* path = lgpd_download_package(ctx, "", "wallet_module", "1.0.0", "", "");
if (path) { /* ... */ lgpd_free_string(path); }
lgpd_free(ctx);
```

## Known limitations

- **HTTPS-only.** A non-`https` repository URL is rejected with
  `unsupported URL scheme (https required in v1)`.
- **Repo mutations need a config path.** `repo add/remove/enable/disable` and
  `config show` require `--config <path>`; an in-memory client returns errors
  for these mutations.
- **The default repository can be disabled but never removed** (and cannot be
  re-added) — it is hardcoded as `kDefaultRepositoryUrl`.
- **Semver matcher is a subset of semver 2.0** — build metadata is ignored for
  comparison; pre-release ordering only handles the basic `X-pre < X` rule.
- **Deep Ed25519 trust is out of scope.** `lgpd` only binds index→file
  (same-DID, manifest fields, rootHash). Keyring-based signature trust is
  `logos-package-manager`'s job at install time.
- **Lazy, per-process metadata cache.** Catalog metadata is resolved once per
  process (`ensureMetadata`); a stale view requires `refreshCatalogs()` /
  `lgpd repo refresh` to re-fetch.
- **Best-effort registry.** Per-repo fetch/parse failures are recorded in
  `resolveError` and skipped, not fatal.
- **Legacy catalog rows.** A version row with `manifest: null` or a missing
  `rootHash` skips the corresponding verification facet — the tool can't verify
  what the catalog didn't advertise.
- **Stale platform docs.** The workspace `CLAUDE.md` `lgpd` section describes an
  older interface (a `--release` flag and a `categories` command) that does
  **not** match the current implementation. `README.md` and `cmd/main.cpp` are
  authoritative: version reports a build-derived `lgpd version <v>` banner (see
  the CLI section), repos are config-file based, and there is no `categories`
  subcommand.
- **Not a Qt module / not a Python package.** There is no `metadata.json` or
  `pyproject.toml` — this is a plain C++ library + CLI.
