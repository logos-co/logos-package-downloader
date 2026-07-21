#include <gtest/gtest.h>
#include "package_downloader_lib.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ─── Mock fetcher: serve a canned catalog so the resolver can be tested
//     without a network. Answers the default repo's logos-repo.json and the
//     index.json it points at; everything else 404s.
namespace {
constexpr const char* kIndexUrl = "https://test.local/index.json";

class MockFetcher : public lgpd::Fetcher {
public:
    std::string repoJson;   // served for the default repo URL
    std::string indexJson;  // served for kIndexUrl
    bool get(const std::string& url, std::string& out) override {
        if (url == lgpd::kDefaultRepositoryUrl) { out = repoJson;  return true; }
        if (url == kIndexUrl)                    { out = indexJson; return true; }
        return false;
    }
    bool getToFile(const std::string&, const std::string&) override { return false; }
};

json makeVersion(const char* ver, const char* hash, const json& deps) {
    // These tests order versions by SemVer precedence, so releasedAt only
    // tie-breaks equal versions (none here) — a single valid date suffices.
    return json{
        {"releasedAt", "2026-01-01T00:00:00Z"},
        {"url", std::string("https://test.local/") + ver + ".lgx"},
        {"rootHash", hash},
        {"manifest", {
            {"manifestVersion", "0.1.0"}, {"name", "x"}, {"version", ver},
            {"type", "core"}, {"dependencies", deps},
            {"main", {{"linux-amd64", "lib/x.so"}}},
        }},
    };
}

// A two-package catalog: blockchain_module @ {0.1.0, 0.2.0} and blockchain_ui
// @ 0.1.0 which depends on blockchain_module (range configurable).
std::shared_ptr<MockFetcher> catalogFetcher(const json& uiDepRange) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    json bmV1 = makeVersion("0.1.0", "h_bm_010", json::array());
    bmV1["manifest"]["name"] = "blockchain_module";
    json bmV2 = makeVersion("0.2.0", "h_bm_020", json::array());
    bmV2["manifest"]["name"] = "blockchain_module";
    json ui  = makeVersion("0.1.0", "h_ui_010", json::array({ uiDepRange }));
    ui["manifest"]["name"] = "blockchain_ui";
    ui["manifest"]["type"] = "ui_qml";
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "blockchain_module"}, {"versions", json::array({bmV2, bmV1})}},
            json{{"name", "blockchain_ui"},     {"versions", json::array({ui})}},
        })},
    }.dump();
    return f;
}

// Collect resolved entries by name -> list of versions.
std::map<std::string, std::vector<std::string>> resolvedVersions(const std::string& resolvedJson) {
    std::map<std::string, std::vector<std::string>> byName;
    for (const auto& e : json::parse(resolvedJson)) {
        if (e.contains("error") || !e.contains("name")) continue;
        byName[e.value("name", "")].push_back(e.value("version", ""));
    }
    return byName;
}
}  // namespace

// ─── Semver matcher ───────────────────────────────────────────────────────────
// These are pure tests — no network involved.

TEST(Semver, ExactAndComparator) {
    using lgpd::PackageDownloaderLib;
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("1.2.3",     "1.2.3"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("1.2.3",     "1.2.4"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches(">=1.0.0",   "1.2.3"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches(">=1.0.0",   "9.0.0"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches(">=2.0.0",   "1.99.0"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("<2.0.0",    "1.99.0"));
}

TEST(Semver, CaretAndTilde) {
    using lgpd::PackageDownloaderLib;
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("^1.2.3",    "1.9.9"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("^1.2.3",    "2.0.0"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("^0.2.3",    "0.2.9"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("^0.2.3",    "0.3.0"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("~1.2.3",    "1.2.9"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("~1.2.3",    "1.3.0"));
}

TEST(Semver, WildcardAndConjunction) {
    using lgpd::PackageDownloaderLib;
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("*",          "9.9.9"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("1.x",        "1.5.0"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("1.x",        "2.0.0"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches(">=1.0 <2.0", "1.5.0"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches(">=1.0 <2.0", "2.0.1"));
    // Alternation
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("1.x || 2.x", "2.3.4"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("1.x || 2.x", "3.0.0"));
}

// REGRESSION. Ranges must not pull in pre-releases nobody asked for.
//
// The old hand-rolled matcher had no pre-release rule, so `^1.0.0` matched
// `2.0.0-alpha`: an unreleased alpha of the *next major* satisfied a caret range
// on 1.x and could be resolved as a dependency. Matching now goes through the
// shared implementation in logos-package, which enforces the npm rule — a range
// only sees pre-releases if it names one at the same major.minor.patch.
TEST(Semver, RangesDoNotMatchUnrequestedPreReleases) {
    using lgpd::PackageDownloaderLib;
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("^1.0.0",  "2.0.0-alpha"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("^1.0.0",  "1.5.0-beta.1"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches(">=1.0.0", "1.0.0-rc.1"));
    EXPECT_FALSE(PackageDownloaderLib::semverMatches("*",       "1.0.0-alpha"));

    // ...but an opted-in range still resolves them.
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("^1.0.0-rc.1", "1.0.0-rc.2"));
    EXPECT_TRUE (PackageDownloaderLib::semverMatches("^1.0.0-rc.1", "1.0.0"));
}

// ─── Resolver ranking ────────────────────────────────────────────────────────

// REGRESSION. The resolver picked the matching candidate with the newest
// `releasedAt`, which is not the highest version. A 1.2.1 hotfix backported
// after 2.0.0 shipped has a newer timestamp but is an older version — and it won.
TEST(Ranking, HighestVersionBeatsLaterPublishedLowerVersion) {
    using lgpd::PackageDownloaderLib;
    // 2.0.0 released Jan; 1.2.1 backported in March. 2.0.0 must still win.
    EXPECT_FALSE(PackageDownloaderLib::outranks("1.2.1", "2026-03-01T00:00:00Z",
                                                "2.0.0", "2026-01-01T00:00:00Z"));
    EXPECT_TRUE (PackageDownloaderLib::outranks("2.0.0", "2026-01-01T00:00:00Z",
                                                "1.2.1", "2026-03-01T00:00:00Z"));
}

TEST(Ranking, ReleasedAtOnlyBreaksTiesBetweenEqualVersions) {
    using lgpd::PackageDownloaderLib;
    // Same version republished (e.g. a different rootHash, or a second repo):
    // the newer publish wins.
    EXPECT_TRUE (PackageDownloaderLib::outranks("1.0.0", "2026-02-01T00:00:00Z",
                                                "1.0.0", "2026-01-01T00:00:00Z"));
    EXPECT_FALSE(PackageDownloaderLib::outranks("1.0.0", "2026-01-01T00:00:00Z",
                                                "1.0.0", "2026-02-01T00:00:00Z"));
}

TEST(Ranking, PreReleaseRanksBelowItsRelease) {
    using lgpd::PackageDownloaderLib;
    EXPECT_FALSE(PackageDownloaderLib::outranks("1.0.0-rc.1", "2026-03-01T00:00:00Z",
                                                "1.0.0",      "2026-01-01T00:00:00Z"));
    // ...and rc.11 outranks rc.2, which a string compare gets backwards.
    EXPECT_TRUE (PackageDownloaderLib::outranks("1.0.0-rc.11", "2026-01-01T00:00:00Z",
                                                "1.0.0-rc.2",  "2026-01-01T00:00:00Z"));
}

// ─── Resolver: top-level pin wins over a transitive re-resolve (issue 2) ──────

// REGRESSION. A module that is BOTH explicitly pinned at top level AND a
// dependency of another pinned package must resolve to ONE entry, at the pin —
// not a second entry at the newest catalog version. The duplicate newest-copy
// is what made the app-manager version dropdown snap back to the latest release.
TEST(Resolver, TopLevelPinNotReResolvedAsTransitiveDep) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(catalogFetcher(json{{"name", "blockchain_module"}, {"version", "*"}}));

    // The app-manager pins BOTH the app and the module (both top-level inputs).
    const std::string input = json::array({
        json{{"name", "blockchain_ui"},     {"version", "0.1.0"}},
        json{{"name", "blockchain_module"}, {"version", "0.1.0"}},
    }).dump();

    const std::string raw = lib.resolveDependenciesJson(input);
    const auto byName = resolvedVersions(raw);

    ASSERT_EQ(byName.count("blockchain_module"), 1u) << "raw resolver output: " << raw;
    // Exactly one entry, and it's the pinned 0.1.0 — NOT a duplicate 0.2.0.
    EXPECT_EQ(byName.at("blockchain_module"), (std::vector<std::string>{"0.1.0"}))
        << "pinned module was re-resolved to newest as a transitive dep; raw: " << raw;
}

// Control: with the module NOT pinned at top level, it still resolves
// transitively to the newest matching version (normal behaviour preserved).
TEST(Resolver, UnpinnedTransitiveDepStillResolvesToNewest) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(catalogFetcher(json{{"name", "blockchain_module"}, {"version", "*"}}));

    const std::string input = json::array({
        json{{"name", "blockchain_ui"}, {"version", "0.1.0"}},
    }).dump();

    const auto byName = resolvedVersions(lib.resolveDependenciesJson(input));
    ASSERT_EQ(byName.count("blockchain_module"), 1u);
    EXPECT_EQ(byName.at("blockchain_module"), (std::vector<std::string>{"0.2.0"}));
}

// ─── Repository registry (in-memory) ─────────────────────────────────────────

TEST(Registry, DefaultIsPresentWhenEnabled) {
    lgpd::PackageDownloaderLib lib;
    auto repos = lib.registry().list();
    ASSERT_FALSE(repos.empty());
    EXPECT_TRUE(repos.front().isDefault);
    EXPECT_TRUE(repos.front().enabled);
}

TEST(Registry, MutationsRequireConfig) {
    lgpd::PackageDownloaderLib lib;
    auto err = lib.registry().addRepository("https://example.com/logos-repo.json");
    EXPECT_FALSE(err.empty());
    // remove of the default also requires a config-backed registry
    err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
    EXPECT_FALSE(err.empty());
}

// defaultDisabled omits the default from list() (not "present but disabled").
TEST(Registry, DisableDefaultOmitsFromListAndPersists) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_cfg_" + std::to_string(std::rand()) + ".json");
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        auto err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
        EXPECT_TRUE(err.empty()) << err;
        EXPECT_TRUE(lib.registry().list().empty());
    }
    ASSERT_TRUE(fs::exists(cfg));
    {
        // Config on disk carries defaultDisabled: true.
        std::ifstream in(cfg);
        json j; in >> j;
        EXPECT_TRUE(j.value("defaultDisabled", false));

        lgpd::PackageDownloaderLib lib2(cfg.string());
        EXPECT_TRUE(lib2.registry().list().empty());
    }
    std::error_code ec; fs::remove(cfg, ec);
}

// removeRepository on the default URL is the permanent-disable path used by
// Settings → Repositories. It must round-trip through the config file, and
// addRepository(defaultUrl) must restore it.
TEST(Registry, RemoveDefaultThenReAdd) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_rm_" + std::to_string(std::rand()) + ".json");
    auto mock = std::make_shared<MockFetcher>();
    mock->repoJson = json{{"schemaVersion", 1}, {"name", "test"},
                          {"displayName", "Test"}, {"indexUrl", kIndexUrl},
                          {"trustedSigners", json::array()}}.dump();
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "test"},
                           {"packages", json::array()}}.dump();
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(mock);
        ASSERT_FALSE(lib.registry().list().empty());

        auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        EXPECT_TRUE(lib.registry().list().empty());

        // Re-removing while already disabled is still a success (idempotent
        // disable) — it just rewrites defaultDisabled: true.
        err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;

        // Re-add by the hardcoded default URL restores the entry.
        err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 1u);
        EXPECT_TRUE(repos.front().isDefault);
        EXPECT_EQ(repos.front().url, lgpd::kDefaultRepositoryUrl);

        // Adding again while present is rejected.
        err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_FALSE(err.empty());
        EXPECT_NE(err.find("already registered"), std::string::npos);
    }
    // Persistence: after remove + save, a fresh client sees an empty list.
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(mock);
        ASSERT_FALSE(lib.registry().list().empty());
        auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
    }
    {
        lgpd::PackageDownloaderLib lib2(cfg.string());
        lib2.setFetcher(mock);
        EXPECT_TRUE(lib2.registry().list().empty());

        auto err = lib2.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        ASSERT_EQ(lib2.registry().list().size(), 1u);
        EXPECT_TRUE(lib2.registry().list().front().isDefault);
    }
    std::error_code ec; fs::remove(cfg, ec);
}

// refresh() must not try to fetch the default when it is disabled — otherwise
// a missing/unreachable default would keep showing up as a refresh error
// after the user removed it.
TEST(Registry, RefreshSkipsDisabledDefault) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_ref_" + std::to_string(std::rand()) + ".json");
    lgpd::PackageDownloaderLib lib(cfg.string());
    // Inject a fetcher that fails every URL. With the default enabled this
    // would produce a "default: ..." error; after remove it must be quiet.
    auto failing = std::make_shared<MockFetcher>();
    failing->repoJson.clear();
    failing->indexJson.clear();
    lib.setFetcher(failing);

    auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
    ASSERT_TRUE(err.empty()) << err;

    err = lib.registry().refresh();
    EXPECT_TRUE(err.empty()) << "refresh reported errors for a disabled default: " << err;

    std::error_code ec; fs::remove(cfg, ec);
}

TEST(Catalog, ReturnsJsonArrayWhenNoNetwork) {
    // No real network is required because the lib lazy-fetches per repo
    // and degrades to empty results on failure.
    lgpd::PackageDownloaderLib lib;
    json catalog;
    ASSERT_NO_THROW(catalog = json::parse(lib.getCatalogJson()));
    EXPECT_TRUE(catalog.is_array());
}
