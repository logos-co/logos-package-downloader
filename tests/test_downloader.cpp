#include <gtest/gtest.h>
#include "package_downloader_lib.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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

// The disabled default stays in list() marked enabled=false so Settings can
// render it as a toggled-off row rather than silently dropping it. The
// defaultDisabled config flag remains authoritative for catalog participation.
TEST(Registry, DisableDefaultKeepsInListMarkedDisabled) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_cfg_" + std::to_string(std::rand()) + ".json");
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        auto err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
        EXPECT_TRUE(err.empty()) << err;
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 1u);
        EXPECT_TRUE(repos.front().isDefault);
        EXPECT_FALSE(repos.front().enabled);
    }
    ASSERT_TRUE(fs::exists(cfg));
    {
        // Config on disk carries defaultDisabled: true.
        std::ifstream in(cfg);
        json j; in >> j;
        EXPECT_TRUE(j.value("defaultDisabled", false));

        lgpd::PackageDownloaderLib lib2(cfg.string());
        auto repos = lib2.registry().list();
        ASSERT_EQ(repos.size(), 1u);
        EXPECT_TRUE(repos.front().isDefault);
        EXPECT_FALSE(repos.front().enabled);
    }
    std::error_code ec; fs::remove(cfg, ec);
}

// removeRepository on the default URL is a hard delete — the row leaves
// list() entirely, and the caller must re-add by URL to restore it.
// Disable (setEnabled) is a separate state — see
// DisableDefaultKeepsInListMarkedDisabled above.
TEST(Registry, RemoveDefaultOmitsFromListAndReAddRestoresIt) {
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
        {
            auto repos = lib.registry().list();
            ASSERT_EQ(repos.size(), 1u);
            EXPECT_TRUE(repos.front().enabled);
        }

        auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        EXPECT_TRUE(lib.registry().list().empty());

        // Re-removing an already-removed default returns "not registered"
        // (parity with a user repo that isn't present).
        err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_FALSE(err.empty());
        EXPECT_NE(err.find("not registered"), std::string::npos);

        // Toggling a removed default is also an error — the caller must
        // re-add first.
        err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
        EXPECT_FALSE(err.empty());
        EXPECT_NE(err.find("not registered"), std::string::npos);

        // Re-add by the hardcoded default URL restores present + enabled.
        err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        {
            auto repos = lib.registry().list();
            ASSERT_EQ(repos.size(), 1u);
            EXPECT_TRUE(repos.front().isDefault);
            EXPECT_TRUE(repos.front().enabled);
            EXPECT_EQ(repos.front().url, lgpd::kDefaultRepositoryUrl);
        }

        // Adding again while present-and-enabled is rejected.
        err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_FALSE(err.empty());
        EXPECT_NE(err.find("already registered"), std::string::npos);
    }
    // Persistence: remove-then-reload — the fresh client sees an empty list.
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(mock);
        {
            auto repos = lib.registry().list();
            ASSERT_EQ(repos.size(), 1u);
            EXPECT_TRUE(repos.front().enabled);
        }
        auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
    }
    {
        lgpd::PackageDownloaderLib lib2(cfg.string());
        lib2.setFetcher(mock);
        EXPECT_TRUE(lib2.registry().list().empty());

        auto err = lib2.registry().addRepository(lgpd::kDefaultRepositoryUrl);
        EXPECT_TRUE(err.empty()) << err;
        auto after = lib2.registry().list();
        ASSERT_EQ(after.size(), 1u);
        EXPECT_TRUE(after.front().isDefault);
        EXPECT_TRUE(after.front().enabled);
    }
    std::error_code ec; fs::remove(cfg, ec);
}

// User (external) repo lifecycle: add → disable → enable → remove → re-add.
// Covers the symmetry with the default's flags — a disable keeps the row
// in list() with enabled=false; a remove takes it out; a re-add restores
// it with enabled=true. Duplicate add is rejected.
TEST(Registry, UserRepoLifecycle) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_user_" + std::to_string(std::rand()) + ".json");
    const std::string url = "https://example.com/ext/logos-repo.json";

    // A URL-agnostic fetcher: any logos-repo.json request gets the same
    // repo manifest; any request to that manifest's indexUrl gets an empty
    // package list. This keeps the mock small and covers both the default
    // URL and the user URL added below.
    class AllUrlsFetcher : public lgpd::Fetcher {
    public:
        std::string repoJson;
        std::string indexJson;
        bool get(const std::string& u, std::string& out) override {
            if (u == kIndexUrl) { out = indexJson; return true; }
            // Any logos-repo.json variant, including the default.
            out = repoJson; return true;
        }
        bool getToFile(const std::string&, const std::string&) override { return false; }
    };
    auto mock = std::make_shared<AllUrlsFetcher>();
    mock->repoJson = json{{"schemaVersion", 1}, {"name", "ext"},
                          {"displayName", "External"}, {"indexUrl", kIndexUrl},
                          {"trustedSigners", json::array()}}.dump();
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "ext"},
                           {"packages", json::array()}}.dump();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(mock);

    // Add.
    auto err = lib.registry().addRepository(url);
    ASSERT_TRUE(err.empty()) << err;
    {
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 2u);   // default + user
        // User row is the second one (default is first).
        EXPECT_EQ(repos[1].url, url);
        EXPECT_TRUE(repos[1].enabled);
        EXPECT_FALSE(repos[1].isDefault);
    }

    // Add-duplicate.
    err = lib.registry().addRepository(url);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("already registered"), std::string::npos);

    // Disable keeps in list, flips enabled=false.
    err = lib.registry().setEnabled(url, false);
    ASSERT_TRUE(err.empty()) << err;
    {
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 2u);
        EXPECT_EQ(repos[1].url, url);
        EXPECT_FALSE(repos[1].enabled);
    }

    // Enable again.
    err = lib.registry().setEnabled(url, true);
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_TRUE(lib.registry().list().at(1).enabled);

    // Remove drops it from the list.
    err = lib.registry().removeRepository(url);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(lib.registry().list().size(), 1u);
    EXPECT_TRUE(lib.registry().list().front().isDefault);

    // Removing again reports not-registered.
    err = lib.registry().removeRepository(url);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("not registered"), std::string::npos);

    // setEnabled on an absent user repo also errors.
    err = lib.registry().setEnabled(url, true);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("not registered"), std::string::npos);

    // Re-adding the same URL after remove works (enabled=true again).
    err = lib.registry().addRepository(url);
    ASSERT_TRUE(err.empty()) << err;
    {
        auto repos = lib.registry().list();
        ASSERT_EQ(repos.size(), 2u);
        EXPECT_EQ(repos[1].url, url);
        EXPECT_TRUE(repos[1].enabled);
    }

    // Add rejects malformed URLs.
    err = lib.registry().addRepository("");
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("empty"), std::string::npos);
    err = lib.registry().addRepository("http://example.com/insecure/logos-repo.json");
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("https"), std::string::npos);

    std::error_code ec; fs::remove(cfg, ec);
}

// Re-adding a disabled (but not removed) default clears the disabled flag —
// the "add" action means "present and enabled", regardless of prior state.
TEST(Registry, AddDefaultWhileDisabledClearsDisabledFlag) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_ad_" + std::to_string(std::rand()) + ".json");
    auto mock = std::make_shared<MockFetcher>();
    mock->repoJson = json{{"schemaVersion", 1}, {"name", "logos-modules-official"},
                          {"displayName", "Logos Official"}, {"indexUrl", kIndexUrl},
                          {"trustedSigners", json::array()}}.dump();
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "logos-modules-official"},
                           {"packages", json::array()}}.dump();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(mock);
    auto err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_EQ(lib.registry().list().size(), 1u);
    ASSERT_FALSE(lib.registry().list().front().enabled);

    err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(err.empty()) << err;
    auto after = lib.registry().list();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_TRUE(after.front().enabled);
    std::error_code ec; fs::remove(cfg, ec);
}

// Regression: add-back after remove is atomic. If the metadata fetch fails,
// addRepository returns the resolveError and the persistent state stays
// exactly as it was — otherwise the default lands in list() as an "unnamed
// repository" row with resolveError set, which never contributes to the
// catalog (getCatalogJson skips repos with resolveError) yet is persisted.
TEST(Registry, AddDefaultAfterRemoveIsAtomicOnFetchFailure) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_ad_atomic_" + std::to_string(std::rand()) + ".json");

    // Prime with a successful fetch so the default is fully hydrated first.
    auto working = std::make_shared<MockFetcher>();
    working->repoJson = json{{"schemaVersion", 1}, {"name", "logos-modules-official"},
                             {"displayName", "Logos Official"}, {"indexUrl", kIndexUrl},
                             {"trustedSigners", json::array()}}.dump();
    working->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "logos-modules-official"},
                              {"packages", json::array()}}.dump();
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(working);
        // Force a resolve so metadata caches populate, then remove the default.
        (void)lib.registry().refresh();
        auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
        ASSERT_TRUE(err.empty()) << err;
        ASSERT_TRUE(lib.registry().list().empty());
    }

    // Reload in a fresh process with the network down. Simulates the
    // reported bug scenario: last session persisted defaultRemoved=true,
    // this session's startup refresh skipped the default, and the user's
    // re-add attempt hits a transient fetch failure.
    //
    // MockFetcher's default `get()` returns TRUE for the default URL even
    // when its body is empty, which trips the parse path instead of the
    // fetch path. Use an override that returns false to actually simulate
    // "network unreachable".
    class NoNetworkFetcher : public lgpd::Fetcher {
    public:
        bool get(const std::string&, std::string&) override { return false; }
        bool getToFile(const std::string&, const std::string&) override { return false; }
    };
    auto failing = std::make_shared<NoNetworkFetcher>();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(failing);
    ASSERT_TRUE(lib.registry().list().empty());   // still removed

    auto err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
    EXPECT_FALSE(err.empty()) << "add-back must fail when refresh fails";
    EXPECT_NE(err.find("fetch failed"), std::string::npos)
        << "expected fetch-failure error, got: " << err;

    // Critical: persistent state is unchanged. list() is still empty,
    // defaultRemoved is still true on disk, and no half-hydrated default
    // row is exposed to the UI.
    EXPECT_TRUE(lib.registry().list().empty())
        << "list() must not contain a half-hydrated default row";
    {
        std::ifstream in(cfg);
        json j; in >> j;
        EXPECT_TRUE(j.value("defaultRemoved", false))
            << "defaultRemoved must remain true after a failed add-back";
    }

    // Fixing the network and retrying succeeds — the row comes back fully
    // populated (name + displayName from the fetched logos-repo.json), and
    // the config now reflects the restored state.
    lib.setFetcher(working);
    err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(err.empty()) << err;
    auto after = lib.registry().list();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_TRUE(after.front().isDefault);
    EXPECT_TRUE(after.front().enabled);
    EXPECT_EQ(after.front().name,        "logos-modules-official");
    EXPECT_EQ(after.front().displayName, "Logos Official");
    EXPECT_TRUE(after.front().resolveError.empty());
    std::error_code ec; fs::remove(cfg, ec);
}

// Successful add-back hydrates the default row — name + displayName come
// from the fetched logos-repo.json, not from empty defaults. This is the
// counterpart to the atomicity test above: it pins the observable state
// after a successful restore, so a regression that leaves fields empty
// even on success would fail here.
TEST(Registry, AddDefaultAfterRemoveHydratesRow) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_ad_hyd_" + std::to_string(std::rand()) + ".json");
    auto mock = std::make_shared<MockFetcher>();
    mock->repoJson = json{{"schemaVersion", 1}, {"name", "logos-modules-official"},
                          {"displayName", "Logos Official"}, {"indexUrl", kIndexUrl},
                          {"trustedSigners", json::array()}}.dump();
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "logos-modules-official"},
                           {"packages", json::array()}}.dump();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(mock);
    // Remove first so we're exercising the add-back path, not just an add.
    auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_TRUE(lib.registry().list().empty());

    err = lib.registry().addRepository(lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(err.empty()) << err;
    auto repos = lib.registry().list();
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_TRUE(repos.front().isDefault);
    EXPECT_TRUE(repos.front().enabled);
    // The row is hydrated — name + displayName populated from the fetched
    // logos-repo.json. Empty here would render as "(unnamed repository)"
    // in the UI and gate the row out of getCatalogJson.
    EXPECT_EQ(repos.front().name,        "logos-modules-official");
    EXPECT_EQ(repos.front().displayName, "Logos Official");
    EXPECT_TRUE(repos.front().resolveError.empty());
    std::error_code ec; fs::remove(cfg, ec);
}

// refresh() must not try to fetch the default when it has been removed —
// the row is gone from list() so a fetch would only produce a phantom
// error the user can't clear. Disabled default is NOT skipped: user
// repos already refresh regardless of enabled state, and the disabled
// default needs its metadata to render in the UI.
TEST(Registry, RefreshSkipsRemovedDefault) {
    auto failing = std::make_shared<MockFetcher>();
    failing->repoJson.clear();
    failing->indexJson.clear();

    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_ref_r_" + std::to_string(std::rand()) + ".json");
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(failing);
    auto err = lib.registry().removeRepository(lgpd::kDefaultRepositoryUrl);
    ASSERT_TRUE(err.empty()) << err;
    err = lib.registry().refresh();
    EXPECT_TRUE(err.empty()) << "refresh reported errors for a removed default: " << err;
    std::error_code ec; fs::remove(cfg, ec);
}

// Regression: disable + restart used to leave the default row unnamed.
// Sequence: config on disk carries defaultDisabled=true from a previous
// session. On this session's startup, refresh() must fetch the default
// (previously it skipped disabled repos, leaving name/displayName/indexUrl
// empty for the whole session — a subsequent setEnabled only flips the
// flag without triggering a refresh, so the row rendered as an "unnamed
// repository" that toggling couldn't fix).
TEST(Registry, RefreshHydratesDisabledDefaultOnStartup) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_dr_" + std::to_string(std::rand()) + ".json");
    auto mock = std::make_shared<MockFetcher>();
    mock->repoJson = json{{"schemaVersion", 1}, {"name", "logos-modules-official"},
                          {"displayName", "Logos Official"}, {"indexUrl", kIndexUrl},
                          {"trustedSigners", json::array()}}.dump();
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "logos-modules-official"},
                           {"packages", json::array()}}.dump();

    // Session 1: disable and quit.
    {
        lgpd::PackageDownloaderLib lib(cfg.string());
        lib.setFetcher(mock);
        auto err = lib.registry().setEnabled(lgpd::kDefaultRepositoryUrl, false);
        ASSERT_TRUE(err.empty()) << err;
    }
    // Config persisted defaultDisabled=true (no runtime fields).
    {
        std::ifstream in(cfg);
        json j; in >> j;
        ASSERT_TRUE(j.value("defaultDisabled", false));
    }

    // Session 2: fresh instance sees defaultDisabled=true. defaultRepo's
    // name/displayName start empty (Impl() ctor state). After refresh they
    // MUST be populated even though the repo is disabled.
    lgpd::PackageDownloaderLib lib2(cfg.string());
    lib2.setFetcher(mock);
    ASSERT_EQ(lib2.registry().list().size(), 1u);
    EXPECT_FALSE(lib2.registry().list().front().enabled);
    // Before refresh, name is empty (nothing has fetched yet).
    EXPECT_EQ(lib2.registry().list().front().name, "");

    auto err = lib2.registry().refresh();
    EXPECT_TRUE(err.empty()) << err;

    auto after = lib2.registry().list();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_FALSE(after.front().enabled) << "disabled state must survive refresh";
    EXPECT_EQ(after.front().name,        "logos-modules-official");
    EXPECT_EQ(after.front().displayName, "Logos Official");
    EXPECT_TRUE(after.front().resolveError.empty());

    // Re-enabling now is instant — the row is already hydrated, so
    // setEnabled just flips the flag; no re-fetch needed.
    err = lib2.registry().setEnabled(lgpd::kDefaultRepositoryUrl, true);
    EXPECT_TRUE(err.empty()) << err;
    auto enabled = lib2.registry().list();
    ASSERT_EQ(enabled.size(), 1u);
    EXPECT_TRUE(enabled.front().enabled);
    EXPECT_EQ(enabled.front().displayName, "Logos Official");
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

TEST(Catalog, IconUrlIsConstructedFromTopLevelIconAndIndexUrl) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    json v = makeVersion("0.1.0", "h_010", json::array());
    v["manifest"]["name"] = "widget";
    v["manifest"]["icon"] = "assets/icon.png";  // legacy relative path
    v["icon"] = {                                // schema-v2 top-level object
        {"path", "26fa06d3.png"},
        {"sha256", "26fa06d3"},
        {"size", 7375},
    };
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "widget"}, {"versions", json::array({v})}},
        })},
    }.dump();
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);
    auto catalog = json::parse(lib.getCatalogJson());
    ASSERT_EQ(catalog.size(), 1u);
    // dirname("https://test.local/index.json") + "/26fa06d3.png"
    EXPECT_EQ(catalog[0].value("icon", ""),
              "https://test.local/26fa06d3.png");
}

TEST(Catalog, IconAbsentWhenNoTopLevelIcon) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    json v = makeVersion("0.1.0", "h_010", json::array());
    v["manifest"]["name"] = "widget";
    v["manifest"]["icon"] = "assets/icon.png";
    // deliberately NO top-level v["icon"]
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "widget"}, {"versions", json::array({v})}},
        })},
    }.dump();
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);
    auto catalog = json::parse(lib.getCatalogJson());
    ASSERT_EQ(catalog.size(), 1u);
    EXPECT_FALSE(catalog[0].contains("icon"));
}

// ─── Concurrent fetching / per-repo isolation ────────────────────────────────
//
// Background: one unreachable address behind raw.githubusercontent.com made
// every call into the package_downloader module take 60–120 s, because the
// repos were fetched one after another and the UI's per-call timeout is
// shorter than that (logos-package-downloader-module#23). These tests pin
// the two behaviours that fix it: all repos are fetched at once, and one
// broken repo neither hides the others nor fails silently.
namespace {

// Serves a small multi-repo world from in-memory maps, and can be "armed" to
// prove that the gets for a set of URLs overlap in time: every armed get()
// blocks until `expected` armed gets are in flight together, giving up after
// a bounded wait and counting a miss. Serial callers can never meet, so a
// serial implementation turns every armed get into a miss (and makes the
// test take expected × 2 s, which is the point: it must not be slow).
class WorldFetcher : public lgpd::Fetcher {
public:
    std::map<std::string, std::string> bodies;   // url -> body; absent = fails
    std::string failDetail = "HTTP status 503";

    void arm(std::function<bool(const std::string&)> match, int expected) {
        std::lock_guard<std::mutex> lock(mu);
        gate = std::move(match);
        want = expected;
        inFlight = 0;
    }

    int misses() const { return missCount.load(); }

    bool get(const std::string& url, std::string& out) override {
        {
            std::unique_lock<std::mutex> lock(mu);
            if (gate && gate(url)) {
                ++inFlight;
                cv.notify_all();
                const bool met = cv.wait_for(lock, std::chrono::seconds(2),
                                             [&] { return inFlight >= want; });
                if (!met) ++missCount;
            }
        }
        auto it = bodies.find(url);
        if (it == bodies.end()) return false;
        out = it->second;
        return true;
    }
    bool getToFile(const std::string&, const std::string&) override { return false; }
    std::string lastError() const override { return failDetail; }

private:
    std::mutex mu;
    std::condition_variable cv;
    std::function<bool(const std::string&)> gate;
    int want = 0;
    int inFlight = 0;
    std::atomic<int> missCount{0};
};

std::string repoJsonFor(const std::string& name, const std::string& indexUrl) {
    return json{{"schemaVersion", 1}, {"name", name}, {"displayName", name},
                {"indexUrl", indexUrl}, {"trustedSigners", json::array()}}.dump();
}

std::string indexJsonWithOnePackage(const std::string& pkgName) {
    const std::string hash = "h_" + pkgName;
    json v = makeVersion("1.0.0", hash.c_str(), json::array());
    v["manifest"]["name"] = pkgName;
    return json{{"schemaVersion", 2}, {"repositoryName", pkgName},
                {"packages", json::array({
                    json{{"name", pkgName}, {"versions", json::array({v})}},
                })}}.dump();
}

constexpr const char* kRepoA = "https://a.example/logos-repo.json";
constexpr const char* kRepoB = "https://b.example/logos-repo.json";
constexpr const char* kIndexA = "https://a.example/index.json";
constexpr const char* kIndexB = "https://b.example/index.json";

// Default repo + two user repos, each with its own index and one package.
std::shared_ptr<WorldFetcher> threeRepoWorld() {
    auto f = std::make_shared<WorldFetcher>();
    f->bodies[lgpd::kDefaultRepositoryUrl] = repoJsonFor("official", kIndexUrl);
    f->bodies[kIndexUrl] = indexJsonWithOnePackage("pkg_official");
    f->bodies[kRepoA] = repoJsonFor("a", kIndexA);
    f->bodies[kIndexA] = indexJsonWithOnePackage("pkg_a");
    f->bodies[kRepoB] = repoJsonFor("b", kIndexB);
    f->bodies[kIndexB] = indexJsonWithOnePackage("pkg_b");
    return f;
}

bool endsWith(const std::string& s, const char* suffix) {
    const std::string suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::vector<std::string> packageNames(const json& catalog) {
    std::vector<std::string> out;
    for (const auto& e : catalog) out.push_back(e.value("name", ""));
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST(Concurrency, RefreshFetchesEveryRepoAtOnce) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_conc_" + std::to_string(std::rand()) + ".json");
    auto world = threeRepoWorld();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(world);
    ASSERT_TRUE(lib.registry().addRepository(kRepoA).empty());
    ASSERT_TRUE(lib.registry().addRepository(kRepoB).empty());

    // Three logos-repo.json fetches must be in flight together.
    world->arm([](const std::string& u) { return endsWith(u, "logos-repo.json"); }, 3);
    const auto t0 = std::chrono::steady_clock::now();
    const std::string err = lib.registry().refresh();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(world->misses(), 0) << "repo fetches did not overlap";
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    for (const auto& r : lib.registry().list()) {
        EXPECT_TRUE(r.resolveError.empty()) << r.url << ": " << r.resolveError;
        EXPECT_FALSE(r.indexUrl.empty()) << r.url;
    }
    std::error_code ec; fs::remove(cfg, ec);
}

TEST(Concurrency, CatalogFetchesEveryIndexAtOnce) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_conc_" + std::to_string(std::rand()) + ".json");
    auto world = threeRepoWorld();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(world);
    ASSERT_TRUE(lib.registry().addRepository(kRepoA).empty());
    ASSERT_TRUE(lib.registry().addRepository(kRepoB).empty());

    // Three index.json fetches must be in flight together.
    world->arm([](const std::string& u) { return endsWith(u, "index.json"); }, 3);
    const auto t0 = std::chrono::steady_clock::now();
    const json catalog = json::parse(lib.getCatalogJson());
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(world->misses(), 0) << "index fetches did not overlap";
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_EQ(packageNames(catalog),
              (std::vector<std::string>{"pkg_a", "pkg_b", "pkg_official"}));
    std::error_code ec; fs::remove(cfg, ec);
}

TEST(Concurrency, BrokenIndexIsIsolatedReportedAndRecovers) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_conc_" + std::to_string(std::rand()) + ".json");
    auto world = threeRepoWorld();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(world);
    ASSERT_TRUE(lib.registry().addRepository(kRepoA).empty());
    ASSERT_TRUE(lib.registry().addRepository(kRepoB).empty());

    // b's logos-repo.json resolves but its index.json is down.
    world->bodies.erase(kIndexB);

    // The other repos' packages are still served.
    json catalog = json::parse(lib.getCatalogJson());
    EXPECT_EQ(packageNames(catalog), (std::vector<std::string>{"pkg_a", "pkg_official"}));

    // The failure is visible on b's row, and only there.
    json repos = json::parse(lib.listRepositoriesJson());
    ASSERT_EQ(repos.size(), 3u);
    for (const auto& r : repos) {
        const std::string url = r.value("url", "");
        const std::string err = r.value("resolveError", "");
        if (url == kRepoB) {
            EXPECT_NE(err.find("index.json fetch failed"), std::string::npos) << err;
            EXPECT_NE(err.find(kIndexB), std::string::npos) << err;
            EXPECT_NE(err.find("HTTP status 503"), std::string::npos) << err;
        } else {
            EXPECT_TRUE(err.empty()) << url << ": " << err;
        }
    }

    // refreshCatalogs re-fetches indexes too and names the broken one.
    std::string summary = lib.refreshCatalogs();
    EXPECT_NE(summary.find(kRepoB), std::string::npos) << summary;
    EXPECT_NE(summary.find("index.json fetch failed"), std::string::npos) << summary;

    // Once b's index is back, the next refresh clears the error and the
    // catalog is whole again — no stale failure sticks to the row.
    world->bodies[kIndexB] = indexJsonWithOnePackage("pkg_b");
    summary = lib.refreshCatalogs();
    EXPECT_TRUE(summary.empty()) << summary;
    catalog = json::parse(lib.getCatalogJson());
    EXPECT_EQ(packageNames(catalog),
              (std::vector<std::string>{"pkg_a", "pkg_b", "pkg_official"}));
    for (const auto& r : json::parse(lib.listRepositoriesJson())) {
        EXPECT_TRUE(r.value("resolveError", "").empty()) << r.value("url", "");
    }
    std::error_code ec; fs::remove(cfg, ec);
}

TEST(Concurrency, BrokenRepoMetadataDoesNotHideTheOthers) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_test_conc_" + std::to_string(std::rand()) + ".json");
    auto world = threeRepoWorld();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(world);
    ASSERT_TRUE(lib.registry().addRepository(kRepoA).empty());
    ASSERT_TRUE(lib.registry().addRepository(kRepoB).empty());

    // b's logos-repo.json itself stops answering.
    world->bodies.erase(kRepoB);
    const std::string summary = lib.refreshCatalogs();
    EXPECT_NE(summary.find(kRepoB), std::string::npos) << summary;
    EXPECT_NE(summary.find("fetch failed"), std::string::npos) << summary;

    const json catalog = json::parse(lib.getCatalogJson());
    EXPECT_EQ(packageNames(catalog), (std::vector<std::string>{"pkg_a", "pkg_official"}));
    std::error_code ec; fs::remove(cfg, ec);
}
