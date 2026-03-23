# logos-package-downloader

C++ library and CLI for fetching the online Logos package catalog and downloading `.lgx` packages from GitHub releases.

This repo handles **network/download** operations only. It does not install packages — that is the responsibility of [logos-package-manager](https://github.com/logos-co/logos-package-manager).

## Library API

```cpp
#include <package_downloader_lib.h>

PackageDownloaderLib dl;

// Configure release tag (default: "latest")
dl.setRelease("v1.0.0");

// Fetch package catalog (returns JSON strings)
std::string packages = dl.getPackages();
std::string filtered = dl.getPackages("networking");
std::string categories = dl.getCategories();

// Dependency resolution (returns JSON array string of resolved names)
std::string resolved = dl.resolveDependencies({"chat_module"});

// Synchronous download (returns local file path, or empty on error)
std::string path = dl.downloadPackage("waku_module");
std::string path = dl.downloadPackage("waku_module", "/output/dir");

// Low-level file download
dl.downloadFile("https://example.com/file.lgx", "/local/path.lgx");
```

### C API

A C-compatible API is available via `lgpd.h` for use from non-C++ consumers:

```c
#include <lgpd.h>

lgpd_context_t ctx = lgpd_create();
lgpd_set_release(ctx, "v1.0.0");
char* packages = lgpd_get_packages(ctx);
lgpd_free_string(packages);
lgpd_free(ctx);
```

## CLI (`lgpd`)

```
lgpd [options] <command> [arguments]

Commands:
  search <query>              Search packages by name/description
  list [--category <cat>]     List available packages
  categories                  List package categories
  info <package>              Show package details from catalog
  download <package>          Download .lgx package

Options:
  --release <tag>             GitHub release tag (default: latest)
  --category <cat>            Filter by category (for list command)
  -o, --output <path>         Output file or directory (for download command)
  --json                      Output in JSON format
  -h, --help                  Show help
  -v, --version               Show version
```

### Examples

```bash
# Search the online catalog
lgpd search waku

# List packages in a category
lgpd list --category networking

# Show package details
lgpd info waku_module --json

# Download a package to a specific directory
lgpd download waku_module -o ./packages/
```

## Building

```bash
nix build                        # library + CLI
nix build .#lib                  # library only
nix build .#cli                  # CLI only
```

## Testing

```bash
nix flake check                  # run all tests
nix build .#tests                # build and run tests
```

## Dependencies

- `nlohmann_json` — JSON parsing
- `libcurl` — HTTP operations
