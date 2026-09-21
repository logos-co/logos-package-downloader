#include <gtest/gtest.h>
#include "package_downloader_lib.h"
#include "progress_throttle.h"   // private header; reachable via the lib's PUBLIC src/ include dir
#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
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
    // Extra per-URL responses, consulted first. Lets a test stand up a
    // SECOND repository with its own logos-repo.json, which the two canned
    // slots above can't express.
    std::map<std::string, std::string> byUrl;

    lgpd::FetchResult get(const std::string& url, std::string& out) override {
        auto it = byUrl.find(url);
        if (it != byUrl.end()) {
            out = it->second;
            return {true, {}};
        }

        if (url == lgpd::kDefaultRepositoryUrl) {
            out = repoJson;
            return {true, {}};
        }

        if (url == kIndexUrl) {
            out = indexJson;
            return {true, {}};
        }

        return {false, "no such url"};
    }
    lgpd::FetchResult getToFile(const std::string&, const std::string&) override {
        return {false, "not served"};
    }
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

// A logos-repo.json body. `name`/`displayName` are exactly what the remote
// says they are — which is the whole point of the qualification tests below.
std::string repoManifest(const char* name, const char* displayName) {
    return json{{"schemaVersion", 1}, {"name", name},
                {"displayName", displayName}, {"indexUrl", kIndexUrl},
                {"trustedSigners", json::array()}}.dump();
}

// One repository entry out of listRepositoriesJson, by URL.
json repoByUrl(lgpd::PackageDownloaderLib& lib, const std::string& url) {
    for (const auto& e : json::parse(lib.listRepositoriesJson()))
        if (e.value("url", "") == url) return e;
    return json::object();
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

// The resolver's output doubles as an install plan, so it carries each pick's
// size — what lets a caller show a denominator before any bytes move.
TEST(Resolver, ResolvedEntryCarriesAdvertisedSize) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    json v = makeVersion("1.0.0", "h_sized", json::array());
    v["manifest"]["name"] = "sized_module";
    v["size"] = 17083080;
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "sized_module"}, {"versions", json::array({v})}},
        })},
    }.dump();

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);
    const auto out = json::parse(lib.resolveDependenciesJson(
        json::array({json{{"name", "sized_module"}}}).dump()));

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].value("size", std::uint64_t{0}), 17083080u);
}

// A catalog predating the field must still resolve, reporting 0 rather than
// dropping the entry or producing a garbage denominator.
TEST(Resolver, MissingSizeResolvesToZeroNotAnError) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(catalogFetcher(json{{"name", "blockchain_module"}, {"version", "*"}}));

    const auto out = json::parse(lib.resolveDependenciesJson(
        json::array({json{{"name", "blockchain_ui"}, {"version", "0.1.0"}}}).dump()));

    ASSERT_FALSE(out.empty());
    for (const auto& e : out) {
        ASSERT_FALSE(e.contains("error")) << "resolver failed: " << e.dump();
        EXPECT_EQ(e.value("size", std::uint64_t{999}), 0u)
            << "absent size should read as 0, not the fallback";
    }
}

// ─── ProgressThrottle ────────────────────────────────────────────────────────
//
// Time is injected, so these are deterministic — no sleeping, no flake.

TEST(ProgressThrottle, FirstSampleAlwaysPasses) {
    lgpd::ProgressThrottle t(200);
    EXPECT_TRUE(t.shouldEmit(0, 1000, 0));
}

TEST(ProgressThrottle, RateLimitsWithinTheInterval) {
    lgpd::ProgressThrottle t(200);
    ASSERT_TRUE(t.shouldEmit(0, 1000, 0));
    EXPECT_FALSE(t.shouldEmit(100, 1000, 50));
    EXPECT_FALSE(t.shouldEmit(200, 1000, 199));
    EXPECT_TRUE(t.shouldEmit(300, 1000, 200)) << "interval elapsed";
    EXPECT_FALSE(t.shouldEmit(400, 1000, 250));
    EXPECT_TRUE(t.shouldEmit(500, 1000, 400));
}

TEST(ProgressThrottle, DropsSamplesCarryingNoNewBytes) {
    lgpd::ProgressThrottle t(200);
    ASSERT_TRUE(t.shouldEmit(500, 1000, 0));
    // A stalled transfer must not emit a stream of identical events.
    EXPECT_FALSE(t.shouldEmit(500, 1000, 1000));
    EXPECT_FALSE(t.shouldEmit(499, 1000, 2000)) << "counters must never go backwards";
    EXPECT_TRUE(t.shouldEmit(501, 1000, 3000));
}

TEST(ProgressThrottle, CompletionAlwaysPassesEvenInsideTheInterval) {
    lgpd::ProgressThrottle t(200);
    ASSERT_TRUE(t.shouldEmit(0, 1000, 0));
    EXPECT_FALSE(t.shouldEmit(500, 1000, 10));
    EXPECT_TRUE(t.shouldEmit(1000, 1000, 11));
}

TEST(ProgressThrottle, UnknownTotalStillRateLimitsAndNeverFakesCompletion) {
    lgpd::ProgressThrottle t(200);
    ASSERT_TRUE(t.shouldEmit(0, 0, 0));
    // total == 0 is "size unknown"; reading received >= total as complete
    // would let every sample bypass the rate limit.
    EXPECT_FALSE(t.shouldEmit(100, 0, 10));
    EXPECT_TRUE(t.shouldEmit(200, 0, 300));
}

// ─── downloadPackage progress plumbing ───────────────────────────────────────

namespace {

// Replays a scripted sequence of (received, total) samples the way libcurl
// would. Delivered back-to-back, so anything the caller sees survived the
// rate limit.
class ProgressFetcher : public MockFetcher {
public:
    using Sample = std::pair<std::uint64_t, std::uint64_t>;

    explicit ProgressFetcher(std::vector<Sample> script)
        : m_script(std::move(script)) {}

    lgpd::FetchResult getToFile(const std::string&, const std::string& path,
                                const lgpd::ProgressFn& onProgress) override {
        std::ofstream(path, std::ios::binary) << "not a real lgx";
        if (onProgress)
            for (const auto& s : m_script) onProgress(s.first, s.second);
        return {true, {}};
    }

private:
    std::vector<Sample> m_script;
};

// Overrides ONLY the two-argument getToFile, like every pre-existing Fetcher
// implementation.
class LegacyOnlyFetcher : public MockFetcher {
public:
    bool called = false;
    lgpd::FetchResult getToFile(const std::string&, const std::string& path) override {
        called = true;
        std::ofstream(path, std::ios::binary) << "not a real lgx";
        return {true, {}};
    }
};

// A one-package catalog whose single version advertises `size`.
template <typename F>
std::shared_ptr<F> sizedFetcher(std::uint64_t advertisedSize, F* seed) {
    std::shared_ptr<F> f(seed);
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    json v = makeVersion("1.0.0", "h_dl", json::array());
    v["manifest"]["name"] = "dl_module";
    v["size"] = advertisedSize;
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "dl_module"}, {"versions", json::array({v})}},
        })},
    }.dump();
    return f;
}

}  // namespace

// The download itself fails afterwards — the served bytes are not a valid
// .lgx — deliberately: progress must be reported DURING the transfer, before
// the verdict is known.
TEST(DownloadProgress, ReportsByteCountsAndRateLimitsOnTheWayOut) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(sizedFetcher(4096, new ProgressFetcher(
        {{0, 4096}, {2048, 4096}, {4096, 4096}})));

    std::vector<std::pair<std::uint64_t, std::uint64_t>> samples;
    std::string err;
    lib.downloadPackage("", "dl_module", err, "", "", "",
                        [&](std::uint64_t received, std::uint64_t total) {
                            samples.emplace_back(received, total);
                        });

    // First and completing survive; the mid-transfer one lands inside the
    // rate-limit window.
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0], (std::pair<std::uint64_t, std::uint64_t>{0, 4096}));
    EXPECT_EQ(samples[1], (std::pair<std::uint64_t, std::uint64_t>{4096, 4096}));
}

// A chunked response reports dltotal == 0; the catalog's size stands in so
// the UI still gets a denominator.
TEST(DownloadProgress, AdvertisedSizeSubstitutesForAnUnknownTransportTotal) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(sizedFetcher(9999, new ProgressFetcher(
        {{0, 0}, {512, 0}, {9999, 0}})));

    std::vector<std::pair<std::uint64_t, std::uint64_t>> samples;
    std::string err;
    lib.downloadPackage("", "dl_module", err, "", "", "",
                        [&](std::uint64_t received, std::uint64_t total) {
                            samples.emplace_back(received, total);
                        });

    ASSERT_FALSE(samples.empty());
    for (const auto& s : samples)
        EXPECT_EQ(s.second, 9999u)
            << "unknown transport total should fall back to the catalog size";
    // The substituted denominator must also drive completion detection, or
    // the final sample is rate-limited away and the bar stalls.
    EXPECT_EQ(samples.back().first, 9999u);
}

// With neither a transport total nor a catalog size, `total` stays 0 rather
// than being faked into something divisible.
TEST(DownloadProgress, TotalStaysZeroWhenNobodyKnowsTheSize) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(sizedFetcher(0, new ProgressFetcher({{0, 0}, {512, 0}})));

    std::vector<std::uint64_t> totals;
    std::string err;
    lib.downloadPackage("", "dl_module", err, "", "", "",
                        [&](std::uint64_t, std::uint64_t total) {
                            totals.push_back(total);
                        });

    ASSERT_EQ(totals.size(), 1u);
    EXPECT_EQ(totals.front(), 0u) << "no size anywhere must stay 0, not be "
                                     "substituted with the absent catalog size";
}

// A throwing sink must cost a progress sample, never the download — and
// never the process. In production the sink is called from inside a libcurl
// C callback, where letting an exception unwind is undefined behaviour.
TEST(DownloadProgress, AThrowingSinkDoesNotBreakTheDownload) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(sizedFetcher(4096, new ProgressFetcher(
        {{0, 4096}, {2048, 4096}, {4096, 4096}})));

    int calls = 0;
    std::string err;
    // Reaching verifyDownloadAgainstIndex at all proves the transfer ran to
    // completion rather than being torn down by the throw.
    lib.downloadPackage("", "dl_module", err, "", "", "",
                        [&](std::uint64_t, std::uint64_t) {
                            ++calls;
                            throw std::runtime_error("sink blew up");
                        });

    EXPECT_GT(calls, 0) << "the sink must actually have been called";
    EXPECT_NE(err.find("verification"), std::string::npos)
        << "download should fail on the bad .lgx, not on the throwing sink; err=" << err;
}

// A Fetcher implementing only the two-argument getToFile must still be
// driven, and the download must still succeed — it just reports no progress.
TEST(DownloadProgress, FetcherWithoutProgressSupportStillDownloads) {
    auto* seed = new LegacyOnlyFetcher();
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(sizedFetcher(4096, seed));

    bool progressed = false;
    std::string err;
    lib.downloadPackage("", "dl_module", err, "", "", "",
                        [&](std::uint64_t, std::uint64_t) { progressed = true; });

    EXPECT_TRUE(seed->called) << "the legacy overload must still be reached";
    EXPECT_FALSE(progressed);
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
        lgpd::FetchResult get(const std::string& u, std::string& out) override {
            if (u == kIndexUrl) {
                out = indexJson;
                return {true, {}};
            }

            // Any logos-repo.json variant, including the default.
            out = repoJson; return {true, {}};
        }
        lgpd::FetchResult getToFile(const std::string&, const std::string&) override {
            return {false, "not served"};
        }
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
        lgpd::FetchResult get(const std::string&, std::string&) override {
            return {false, "network unreachable"};
        }
        lgpd::FetchResult getToFile(const std::string&, const std::string&) override {
            return {false, "network unreachable"};
        }
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

// ─── Repository source ───────────────────────────────────────────────────────
//
// `displayName` is remote-supplied and not unique, so UIs disambiguate with
// the source parsed out of the URL. The lib reports those as facts; deciding
// when to surface them is the UI's call and is tested there.

TEST(Registry, SourceFieldsSplitAGitHubUrl) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_src_gh_" + std::to_string(std::rand()) + ".json");
    auto mock = std::make_shared<MockFetcher>();
    mock->repoJson = repoManifest("logos-modules-official", "Logos Official");
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "x"},
                           {"packages", json::array()}}.dump();
    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(mock);

    const auto r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    EXPECT_EQ(r.value("sourceOwner", ""), "logos-co");
    EXPECT_EQ(r.value("sourceRepo",  ""), "logos-modules-release");
    EXPECT_EQ(r.value("sourceHost",  ""), "raw.githubusercontent.com");
    std::error_code ec; fs::remove(cfg, ec);
}

// The real-world case: a fork whose logos-repo.json is byte-identical to the
// official one, so `name`, `displayName` AND `indexUrl` all collide. Only the
// URL separates them, which is why the source fields exist.
TEST(Registry, SourceOwnerSeparatesAForkClaimingTheSameName) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_src_fork_" + std::to_string(std::rand()) + ".json");
    const std::string forkUrl =
        "https://raw.githubusercontent.com/0x-r4bbit/logos-modules-release-upstream/main/logos-repo.json";

    auto mock = std::make_shared<MockFetcher>();
    mock->repoJson = repoManifest("logos-modules-official", "Logos Official");
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "x"},
                           {"packages", json::array()}}.dump();
    mock->byUrl[forkUrl] = repoManifest("logos-modules-official", "Logos Official");

    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(mock);
    const auto err = lib.registry().addRepository(forkUrl);
    ASSERT_TRUE(err.empty()) << err;

    const auto def  = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    const auto fork = repoByUrl(lib, forkUrl);
    // Every name field collides...
    EXPECT_EQ(def.value("displayName", ""), fork.value("displayName", ""));
    EXPECT_EQ(def.value("name", ""),        fork.value("name", ""));
    // ...and the source is what still tells them apart.
    EXPECT_EQ(def.value("sourceOwner", ""),  "logos-co");
    EXPECT_EQ(fork.value("sourceOwner", ""), "0x-r4bbit");
    EXPECT_EQ(fork.value("sourceRepo", ""),  "logos-modules-release-upstream");
    std::error_code ec; fs::remove(cfg, ec);
}

// Off GitHub there is no owner/repo to parse. Both come back empty and the
// host carries the identification — UIs fall back to it.
TEST(Registry, SourceOwnerIsEmptyOffGitHubAndHostCarriesIt) {
    fs::path cfg = fs::temp_directory_path() / ("lgpd_src_host_" + std::to_string(std::rand()) + ".json");
    const std::string url = "https://packages.acme.dev/logos/logos-repo.json";

    auto mock = std::make_shared<MockFetcher>();
    mock->repoJson = repoManifest("logos-modules-official", "Logos Official");
    mock->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "x"},
                           {"packages", json::array()}}.dump();
    mock->byUrl[url] = repoManifest("acme-internal", "Internal Tools");

    lgpd::PackageDownloaderLib lib(cfg.string());
    lib.setFetcher(mock);
    ASSERT_TRUE(lib.registry().addRepository(url).empty());

    const auto r = repoByUrl(lib, url);
    EXPECT_EQ(r.value("sourceOwner", ""), "");
    EXPECT_EQ(r.value("sourceRepo",  ""), "");
    EXPECT_EQ(r.value("sourceHost",  ""), "packages.acme.dev");
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

TEST(Catalog, ArrayValuedProvidesKeepsTheWholeRepo) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    const json provides = json::array({ json{{"intent", "evm.rpc.configure"}} });
    json withIntents = makeVersion("0.1.0", "h_010", json::array());
    withIntents["manifest"]["name"] = "widget";
    withIntents["manifest"]["provides"] = provides;
    json plain = makeVersion("0.1.0", "h_020", json::array());
    plain["manifest"]["name"] = "gadget";
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "widget"}, {"versions", json::array({withIntents})}},
            json{{"name", "gadget"}, {"versions", json::array({plain})}},
        })},
    }.dump();
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);
    auto catalog = json::parse(lib.getCatalogJson());
    // `provides` is an array; reading it as a string threw and the catch
    // around the loop swallowed every package in the repo, not just this one.
    ASSERT_EQ(catalog.size(), 2u);
    EXPECT_EQ(catalog[0]["provides"], provides);
    EXPECT_EQ(catalog[1]["provides"], json::array());
}

TEST(Catalog, AMalformedFieldCostsOnlyItsOwnPackage) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    json bad = makeVersion("0.1.0", "h_010", json::array());
    bad["manifest"]["name"] = "widget";
    bad["manifest"]["display_name"] = json::array({"not", "a", "string"});
    json good = makeVersion("0.1.0", "h_020", json::array());
    good["manifest"]["name"] = "gadget";
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "widget"}, {"versions", json::array({bad})}},
            json{{"name", "gadget"}, {"versions", json::array({good})}},
        })},
    }.dump();
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);
    auto catalog = json::parse(lib.getCatalogJson());
    ASSERT_EQ(catalog.size(), 1u);
    EXPECT_EQ(catalog[0].value("name", ""), "gadget");
}

TEST(Catalog, ScopedByNameWorksBeforeAnExplicitRefresh) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    json v = makeVersion("0.1.0", "h_010", json::array());
    v["manifest"]["name"] = "widget";
    f->indexJson = json{
        {"schemaVersion", 2}, {"repositoryName", "test"},
        {"packages", json::array({
            json{{"name", "widget"}, {"versions", json::array({v})}},
        })},
    }.dump();
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);
    // Both the name and the indexUrl come from logos-repo.json, so a lookup
    // that runs before metadata resolution matches nothing / fetches nothing.
    auto byName = json::parse(lib.getCatalogForRepoJson("test"));
    ASSERT_EQ(byName.size(), 1u);
    EXPECT_EQ(byName[0].value("name", ""), "widget");
}

// ─── Dependency signer pin ────────────────────────────────────────────────────
//
// A dependency's `signer` field DISAMBIGUATES among same-named candidates:
// "of the several packages called `bm` in the merged catalog, I mean the one
// this identity published". It is NOT an authorization. Matching a pin does
// not make a package installable — that decision is the install-time
// trust-anchor policy in logos-package-manager, which rejects a package no
// ACTIVE anchor validates. The two checks live in different processes, run at
// different times, and neither substitutes for the other; these tests cover
// only the first.
//
// The property under test is one-directional: a pin can only ever SHRINK the
// candidate set. It must never widen it, and it must never be a route to the
// candidates that carry no signature at all.

namespace {

constexpr const char* kGoodDid  = "did:jwk:eyJrdHkiOiJPS1AiLCJnb29kIn0";
constexpr const char* kOtherDid = "did:jwk:eyJrdHkiOiJPS1AiLCJvdGhlciJ9";

// One row of the `bm` package's version list. `signature` is placed verbatim
// into the catalog entry; a null means the key is absent entirely, which is
// what every one of the 49 versions in the live official catalog looks like.
struct SignerRow {
    std::string version;
    json        signature;
};

// A catalog holding one package `bm` with the given versions, and optionally a
// package `app` @0.1.0 whose MANIFEST declares `appDep` as a dependency. The
// two shapes exercise the resolver's two entry paths: the caller-supplied
// top-level array, and a transitive entry read out of a catalog-embedded
// manifest. Only the first is ever seen by `lgx verify`, so the second is the
// one with no upstream validation whatsoever — both must be covered.
std::shared_ptr<MockFetcher> signerCatalogFetcher(const std::vector<SignerRow>& rows,
                                                  const json& appDep = json()) {
    auto f = std::make_shared<MockFetcher>();
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();

    json versions = json::array();
    for (const auto& r : rows) {
        const std::string hash = "h_bm_" + r.version;
        json v = makeVersion(r.version.c_str(), hash.c_str(), json::array());
        v["manifest"]["name"] = "bm";
        if (!r.signature.is_null()) v["signature"] = r.signature;
        versions.push_back(std::move(v));
    }

    json packages = json::array({ json{{"name", "bm"}, {"versions", versions}} });
    if (!appDep.is_null()) {
        json app = makeVersion("0.1.0", "h_app_010", json::array({appDep}));
        app["manifest"]["name"] = "app";
        app["manifest"]["type"] = "ui_qml";
        packages.push_back(json{{"name", "app"}, {"versions", json::array({app})}});
    }

    f->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "test"},
                        {"packages", packages}}.dump();
    return f;
}

std::vector<std::string> resolverErrors(const std::string& raw) {
    std::vector<std::string> errs;
    for (const auto& e : json::parse(raw))
        if (e.contains("error")) errs.push_back(e.value("error", ""));
    return errs;
}

// bm 2.0.0 signed by the good key, bm 1.0.0 with no signature at all. This is
// the shape that makes an empty pin dangerous: the newest release is signed
// and an OLDER one is not, so "select the unsigned rows" is also "downgrade".
std::vector<SignerRow> signedNewUnsignedOld() {
    return { {"2.0.0", json{{"did", kGoodDid}, {"sig", "deadbeef"}}},
             {"1.0.0", json()} };
}

}  // namespace

// ── ABSENT: no pin declared, no filtering ────────────────────────────────────

TEST(SignerPin, AbsentPinDoesNotFilterAndResolvesTheNewest) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld()));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}}
    }).dump());
    const auto byName = resolvedVersions(raw);
    ASSERT_EQ(byName.count("bm"), 1u) << raw;
    EXPECT_EQ(byName.at("bm"), (std::vector<std::string>{"2.0.0"})) << raw;
    EXPECT_TRUE(resolverErrors(raw).empty()) << raw;
}

// ── MATCHING: the pin selects that signer's release ──────────────────────────

TEST(SignerPin, MatchingPinSelectsThatSignersRelease) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld()));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}, {"signer", kGoodDid}}
    }).dump());
    const auto byName = resolvedVersions(raw);
    ASSERT_EQ(byName.count("bm"), 1u) << raw;
    EXPECT_EQ(byName.at("bm"), (std::vector<std::string>{"2.0.0"})) << raw;
}

// A pin genuinely DISAMBIGUATES: with the newest release published by someone
// else, the pin picks the older one its signer published rather than the newest.
TEST(SignerPin, MatchingPinPicksItsSignersOlderReleaseOverAnotherSignersNewer) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher({
        {"2.0.0", json{{"did", kOtherDid}, {"sig", "beef"}}},
        {"1.0.0", json{{"did", kGoodDid},  {"sig", "cafe"}}},
    }));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}, {"signer", kGoodDid}}
    }).dump());
    const auto byName = resolvedVersions(raw);
    ASSERT_EQ(byName.count("bm"), 1u) << raw;
    EXPECT_EQ(byName.at("bm"), (std::vector<std::string>{"1.0.0"})) << raw;
}

// ── NON-MATCHING: an error, never a fallback ─────────────────────────────────

TEST(SignerPin, NonMatchingPinIsAnErrorAndResolvesNothing) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld()));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}, {"signer", kOtherDid}}
    }).dump());
    EXPECT_EQ(resolvedVersions(raw).count("bm"), 0u)
        << "an unmatched pin fell back to some other candidate; raw: " << raw;
    ASSERT_FALSE(resolverErrors(raw).empty()) << raw;
    EXPECT_NE(resolverErrors(raw)[0].find("no candidate matches"), std::string::npos) << raw;
}

// ── EMPTY: rejected, and never a route to the unsigned candidates ────────────

// REGRESSION (B1). `"signer": ""` used to parse clean with the option ENGAGED
// holding "". findBest compared it against each candidate's `signature.did`,
// and an unsigned row yields "" — so the pin matched exactly the rows with NO
// signature and skipped every signed one. Against this catalog it resolved the
// unsigned, older 1.0.0 over the signed 2.0.0, with no error and exit 0.
TEST(SignerPin, EmptyPinIsRejectedAndNeverSelectsAnUnsignedRelease) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld()));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}, {"signer", ""}}
    }).dump());
    EXPECT_EQ(resolvedVersions(raw).count("bm"), 0u)
        << "empty signer pin resolved a candidate (the unsigned one); raw: " << raw;
    ASSERT_FALSE(resolverErrors(raw).empty())
        << "empty signer pin was accepted silently; raw: " << raw;
    EXPECT_NE(resolverErrors(raw)[0].find("signer"), std::string::npos) << raw;
}

// The same pin arriving through the OTHER entry path. A transitive dep comes
// out of a catalog-embedded manifest, which the downloader never runs
// Manifest::validate() on — so logos-package's did:jwk regex, the one gate
// that would have caught this, never sees it.
TEST(SignerPin, EmptyPinInATransitiveManifestIsReportedNotSilentlyHonoured) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld(),
                                        json{{"name", "bm"}, {"signer", ""}}));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "app"}}
    }).dump());
    EXPECT_EQ(resolvedVersions(raw).count("bm"), 0u)
        << "empty signer pin in a manifest resolved the unsigned release; raw: " << raw;
    ASSERT_FALSE(resolverErrors(raw).empty())
        << "a dependency entry that cannot be parsed was dropped, not reported; raw: " << raw;
    EXPECT_NE(resolverErrors(raw)[0].find("signer"), std::string::npos) << raw;
}

// ── MALFORMED: null, non-string, not a did:jwk ───────────────────────────────
//
// All three used to fall through `&& j["signer"].is_string()` and leave the
// dep UNPINNED — a declared constraint silently widened to "anything", which
// is the one outcome a pin exists to prevent.

TEST(SignerPin, NullPinIsRejectedRatherThanTreatedAsUnpinned) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld()));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}, {"signer", nullptr}}
    }).dump());
    EXPECT_EQ(resolvedVersions(raw).count("bm"), 0u) << raw;
    ASSERT_FALSE(resolverErrors(raw).empty())
        << "null signer silently widened the pin to unconstrained; raw: " << raw;
}

TEST(SignerPin, NonStringPinIsRejectedRatherThanTreatedAsUnpinned) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld()));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}, {"signer", 42}}
    }).dump());
    EXPECT_EQ(resolvedVersions(raw).count("bm"), 0u) << raw;
    ASSERT_FALSE(resolverErrors(raw).empty())
        << "non-string signer silently widened the pin to unconstrained; raw: " << raw;
    EXPECT_NE(resolverErrors(raw)[0].find("signer"), std::string::npos) << raw;
}

TEST(SignerPin, MalformedDidPinIsRejected) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld()));
    for (const char* bad : { "GOODKEY", "did:key:abc", "did:jwk:", "did:jwk:has space" }) {
        const std::string raw = lib.resolveDependenciesJson(json::array({
            json{{"name", "bm"}, {"signer", bad}}
        }).dump());
        EXPECT_EQ(resolvedVersions(raw).count("bm"), 0u) << bad << " -> " << raw;
        const auto errs = resolverErrors(raw);
        ASSERT_FALSE(errs.empty()) << "malformed pin '" << bad << "' was accepted; raw: " << raw;
        // It must fail AS A MALFORMED PIN. Falling through to "no candidate
        // matches" is the wrong diagnosis -- it blames the catalog for not
        // carrying a signer that could never have been spelled that way, and
        // it is indistinguishable from the case where the pin is well-formed
        // and the publisher simply has not released yet.
        EXPECT_EQ(errs[0].find("no candidate matches"), std::string::npos)
            << "malformed pin '" << bad << "' was diagnosed as a catalog miss; raw: " << raw;
        EXPECT_NE(errs[0].find("signer"), std::string::npos) << raw;
    }
}

// ── The invariant, at the comparison itself ──────────────────────────────────
//
// parseDep refuses an empty pin, but that is input validation on one call
// path. signerPinMatches is the property, sitting next to the comparison it
// constrains, so a ParsedDep built some other way cannot reopen the hole.

TEST(SignerPin, PinNeverMatchesACandidateWithNoOrEmptySignerDid) {
    using lgpd::PackageDownloaderLib;
    // An unsigned candidate (empty DID) matches NOTHING — not a real pin...
    EXPECT_FALSE(PackageDownloaderLib::signerPinMatches(kGoodDid, ""));
    // ...and not an empty one. This is B1 stated as a property: an empty pin
    // must never be the thing that selects the unsigned rows.
    EXPECT_FALSE(PackageDownloaderLib::signerPinMatches("", ""));
    EXPECT_FALSE(PackageDownloaderLib::signerPinMatches("", kGoodDid));
    // A real pin matches its own signer and nobody else's.
    EXPECT_TRUE (PackageDownloaderLib::signerPinMatches(kGoodDid, kGoodDid));
    EXPECT_FALSE(PackageDownloaderLib::signerPinMatches(kGoodDid, kOtherDid));
}

// ...and the same property through the resolver, for the two catalog shapes
// that produce an empty DID: no `signature` key, `"signature": {}`, and
// `"signature": {"did": ""}`.
TEST(SignerPin, PinFindsNoCandidateWhenEveryCandidateIsEffectivelyUnsigned) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher({
        {"3.0.0", json()},                        // no signature key
        {"2.0.0", json::object()},                // signature present, no did
        {"1.0.0", json{{"did", ""}}},             // did present, empty
    }));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}, {"signer", kGoodDid}}
    }).dump());
    EXPECT_EQ(resolvedVersions(raw).count("bm"), 0u)
        << "a pin matched an unsigned candidate; raw: " << raw;
    ASSERT_FALSE(resolverErrors(raw).empty()) << raw;
}

// ── The pin is not an authorization ──────────────────────────────────────────
//
// Nothing about matching a pin makes a package trusted. The resolver has no
// keyring, consults no anchor set, and a satisfied pin produces an ordinary
// resolved entry that the installer will still judge on its own terms. This
// test pins the SHAPE of that output so nobody later adds a "trusted" flag
// here and turns a self-asserted identity into an install decision.
TEST(SignerPin, ResolvedEntryCarriesNoTrustVerdict) {
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(signerCatalogFetcher(signedNewUnsignedOld()));
    const std::string raw = lib.resolveDependenciesJson(json::array({
        json{{"name", "bm"}, {"signer", kGoodDid}}
    }).dump());
    for (const auto& e : json::parse(raw)) {
        if (!e.contains("name")) continue;
        EXPECT_FALSE(e.contains("trusted"))    << raw;
        EXPECT_FALSE(e.contains("trusted_as")) << raw;
        EXPECT_FALSE(e.contains("verified"))   << raw;
    }
}

// ── Binding a DOWNLOADED FILE to the signer the catalog advertised ───────────
//
// A different question from the pin above, on different inputs. signerPinMatches
// compares two pieces of CATALOG metadata while choosing what to fetch;
// downloadedSignerBinds compares the catalog against the BYTES THAT ARRIVED.
//
// The term that was missing is `fileSignatureValid`. logos-package sets
// `signer_did` from manifest.sig BEFORE it runs the Ed25519 check
// (Package::verifySignature), so a DID read off a downloaded file is a CLAIM
// until that check passes — and the DID being claimed is published in the
// catalog for anyone to copy. The binding used to read `is_signed` and
// `signer_did` only, so a substituted package carrying a hand-written
// manifest.sig that merely NAMED the advertised DID bound successfully. No key
// required, and nothing downstream re-checks: package_manager under the default
// WARN policy installs a package whose signature failed... no. It refuses that
// one. What it does NOT refuse is the swap this binding exists to catch, which
// is the whole reason 2c is here at all.

TEST(DownloadedSignerBinding, AForgedSignatureNamingTheAdvertisedDidDoesNotBind) {
    using lgpd::PackageDownloaderLib;

    // The attack, stated exactly: signed=true (there IS a manifest.sig), the
    // DID inside it is the advertised one, and it does not verify.
    EXPECT_FALSE(PackageDownloaderLib::downloadedSignerBinds(
        /*fileSigned=*/true, /*fileSignatureValid=*/false, kGoodDid, kGoodDid))
        << "a manifest.sig that merely NAMES the advertised DID bound to the catalog";

    // The honest case still binds.
    EXPECT_TRUE(PackageDownloaderLib::downloadedSignerBinds(true, true, kGoodDid, kGoodDid));

    // ...and every other way of failing still fails.
    EXPECT_FALSE(PackageDownloaderLib::downloadedSignerBinds(false, false, "", kGoodDid))
        << "an unsigned substitute bound to a signed catalog entry";
    EXPECT_FALSE(PackageDownloaderLib::downloadedSignerBinds(true, true, kOtherDid, kGoodDid))
        << "a validly signed package by the WRONG publisher bound";
    EXPECT_FALSE(PackageDownloaderLib::downloadedSignerBinds(true, false, kOtherDid, kGoodDid));
    // An empty advertised DID binds nothing — the same shape of hole B1 was.
    EXPECT_FALSE(PackageDownloaderLib::downloadedSignerBinds(true, true, kGoodDid, ""));
    EXPECT_FALSE(PackageDownloaderLib::downloadedSignerBinds(true, true, "", ""));
}

// ─── Download staging + failure attribution ──────────────────────────────────

namespace {

// Captures the path downloadPackage asked for and stops there: producing a
// real .lgx is unnecessary to observe WHERE the transfer was staged.
class PathCapturingFetcher : public MockFetcher {
public:
    std::string seenPath;
    lgpd::FetchResult getToFile(const std::string&, const std::string& path) override {
        seenPath = path;
        return {false, "stopped by test"};
    }
};

// Serves logos-repo.json but fails index.json, i.e. a resolved repository
// whose catalog cannot be read.
class IndexlessFetcher : public MockFetcher {
public:
    lgpd::FetchResult get(const std::string& url, std::string& out) override {
        if (url == lgpd::kDefaultRepositoryUrl) {
            out = repoJson;
            return {true, {}};
        }
        return {false, "connection refused"};
    }
};

std::shared_ptr<MockFetcher> repoWith(MockFetcher* seed, const json& packages) {
    std::shared_ptr<MockFetcher> f(seed);
    f->repoJson = json{{"schemaVersion", 1}, {"name", "test"}, {"displayName", "Test"},
                       {"indexUrl", kIndexUrl}, {"trustedSigners", json::array()}}.dump();
    f->indexJson = json{{"schemaVersion", 2}, {"repositoryName", "test"},
                        {"packages", packages}}.dump();
    return f;
}

json onePackage(const char* name, const char* ver) {
    json v = makeVersion(ver, "h_stage", json::array());
    v["manifest"]["name"] = name;
    return json::array({json{{"name", name}, {"versions", json::array({v})}}});
}

}  // namespace

// The published name is package+version, so staging in the shared temp ROOT
// let two accounts on one host collide on a single path: the loser cannot
// overwrite a file it does not own, and cannot unlink it either because the
// temp root is sticky. Staging must therefore happen somewhere private.
TEST(DownloadStaging, StagesOutsideTheSharedTempRoot) {
    auto f = repoWith(new PathCapturingFetcher, onePackage("stage_module", "1.0.0"));
    auto* capture = static_cast<PathCapturingFetcher*>(f.get());

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);
    std::string err;
    lib.downloadPackage("", "stage_module", err);

    ASSERT_FALSE(capture->seenPath.empty());
    const fs::path staged(capture->seenPath);
    EXPECT_NE(staged.parent_path(), fs::temp_directory_path());
    EXPECT_EQ(staged.parent_path().parent_path(), fs::temp_directory_path());
#ifndef _WIN32
    // Per-uid, and private: another account must not be able to plant a file
    // at a path we are going to write.
    EXPECT_EQ(staged.parent_path().filename().string().rfind("lgpd-", 0), 0u);
    EXPECT_EQ(fs::status(staged.parent_path()).permissions() &
                  (fs::perms::group_all | fs::perms::others_all),
              fs::perms::none);
#endif
}

// An explicit outputDir is still honoured verbatim.
TEST(DownloadStaging, ExplicitOutputDirIsUnchanged) {
    auto f = repoWith(new PathCapturingFetcher, onePackage("stage_module", "1.0.0"));
    auto* capture = static_cast<PathCapturingFetcher*>(f.get());
    const fs::path out =
        fs::temp_directory_path() / ("lgpd_out_" + std::to_string(std::rand()));
    fs::create_directories(out);

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);
    std::string err;
    lib.downloadPackage("", "stage_module", err, "", "", out.string());

    ASSERT_FALSE(capture->seenPath.empty());
    EXPECT_EQ(fs::path(capture->seenPath).parent_path(), out);
    fs::remove_all(out);
}

// An unreachable catalog used to be indistinguishable from one that simply
// does not carry the package — both produced an empty path and, later, the
// same "no repository advertises it" verdict.
TEST(DownloadErrors, UnreadableIndexIsNotReportedAsAMissingPackage) {
    auto f = repoWith(new IndexlessFetcher, onePackage("stage_module", "1.0.0"));
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    std::string err;
    EXPECT_TRUE(lib.downloadPackage("", "stage_module", err).empty());
    EXPECT_NE(err.find("could not read any repository index"), std::string::npos);
    EXPECT_NE(err.find("connection refused"), std::string::npos);
    EXPECT_EQ(err.find("no repository advertises"), std::string::npos);
}

// A version pin that matches nothing names what the catalog does offer.
TEST(DownloadErrors, VersionPinMismatchListsTheOfferedVersions) {
    auto f = repoWith(new PathCapturingFetcher, onePackage("stage_module", "1.0.0"));
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    std::string err;
    EXPECT_TRUE(lib.downloadPackage("", "stage_module", err, "0.2.4").empty());
    EXPECT_NE(err.find("0.2.4"), std::string::npos);
    EXPECT_NE(err.find("offers: 1.0.0"), std::string::npos);
}

// refreshCatalogs() promises to re-fetch every enabled repo's index.json. It
// used to resolve only logos-repo.json, so `catalog refresh` reported success
// against a catalog it had never managed to read.
TEST(RefreshCatalogs, ReportsAnIndexItCannotRead) {
    auto f = repoWith(new IndexlessFetcher, onePackage("stage_module", "1.0.0"));
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const std::string errs = lib.refreshCatalogs();
    EXPECT_NE(errs.find("index fetch failed"), std::string::npos);
    EXPECT_NE(errs.find("connection refused"), std::string::npos);
}

// ─── Catalog includes ─────────────────────────────────────────────────────────
//
// A catalog can name other catalogs in `logos-repo.json#includes[]` and draw
// packages from them. The client resolves that graph at fetch time; the index
// format is untouched. These tests stand up several catalogs in one mock
// fetcher and check what the merged catalog ends up containing.

namespace {

constexpr const char* kBRepo  = "https://b.local/logos-repo.json";
constexpr const char* kBIndex = "https://b.local/index.json";
constexpr const char* kBIncludes = "https://b.local/includes.json";
constexpr const char* kCRepo  = "https://c.local/logos-repo.json";
constexpr const char* kCIndex = "https://c.local/index.json";
constexpr const char* kCIncludes = "https://c.local/includes.json";
// The root here is the built-in default repository, so a test needs no config
// file on disk to get a configured repo that draws from other catalogs.
constexpr const char* kRootIncludes = "https://test.local/includes.json";

// A version whose download URL and description both name the catalog serving
// it, so a merged entry can be traced back to where each part came from.
json versionFrom(const std::string& origin, const char* name,
                 const char* ver, const char* hash) {
    return json{
        {"releasedAt", "2026-01-01T00:00:00Z"},
        {"url", "https://" + origin + ".local/" + name + "-" + ver + ".lgx"},
        {"rootHash", hash},
        {"manifest", {
            {"manifestVersion", "0.1.0"}, {"name", name}, {"version", ver},
            {"type", "core"}, {"dependencies", json::array()},
            {"description", "served by " + origin},
            {"main", {{"linux-amd64", "lib/x.so"}}},
        }},
    };
}

json packageOf(const char* name, const json& versions) {
    return json{{"name", name}, {"versions", versions}};
}

// The identity card points at the includes document; it never carries the list
// itself. `includesUrl` empty means the catalog draws from nothing.
std::string repoDoc(const char* name, const char* indexUrl,
                    const std::string& includesUrl = {}) {
    json j{{"schemaVersion", 1}, {"name", name}, {"displayName", name},
           {"indexUrl", indexUrl}, {"trustedSigners", json::array()}};
    if (!includesUrl.empty()) j["includesUrl"] = includesUrl;
    return j.dump();
}

std::string includesDoc(const json& includes) {
    return json{{"schemaVersion", 1}, {"includes", includes}}.dump();
}

std::string indexDoc(const char* name, const json& packages) {
    return json{{"schemaVersion", 2}, {"repositoryName", name},
                {"packages", packages}}.dump();
}

// Stand up the root catalog with an includes document served beside it.
std::shared_ptr<MockFetcher> rootIncluding(const json& includes,
                                           const json& rootPackages,
                                           MockFetcher* seed = nullptr) {
    std::shared_ptr<MockFetcher> f(seed ? seed : new MockFetcher);
    f->repoJson  = repoDoc("root", kIndexUrl, kRootIncludes);
    f->indexJson = indexDoc("root", rootPackages);
    f->byUrl[kRootIncludes] = includesDoc(includes);
    return f;
}

// Everything the merged catalog offers, as name -> versions (in catalog order).
std::map<std::string, std::vector<std::string>> catalogVersions(
        lgpd::PackageDownloaderLib& lib) {
    std::map<std::string, std::vector<std::string>> out;
    for (const auto& e : json::parse(lib.getCatalogJson())) {
        auto& vers = out[e.value("name", "")];
        for (const auto& v : e.value("versions", json::array()))
            vers.push_back(v["manifest"].value("version", ""));
    }
    return out;
}

json catalogEntry(lgpd::PackageDownloaderLib& lib, const std::string& name) {
    for (const auto& e : json::parse(lib.getCatalogJson()))
        if (e.value("name", "") == name) return e;
    return json::object();
}

class UrlCapturingFetcher : public MockFetcher {
public:
    std::string seenUrl;
    lgpd::FetchResult getToFile(const std::string& url, const std::string&) override {
        seenUrl = url;
        return {false, "stopped by test"};
    }
};

}  // namespace

// An include with no `packages` filter takes the whole catalog.
TEST(CatalogIncludes, WholeCatalogIsDrawnIn) {
    auto f = rootIncluding(json::array({json{{"repo", kBRepo}}}),
                           json::array({packageOf("root_module",
                               json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_one", json::array({versionFrom("b", "b_one", "1.0.0", "h_b1")})),
        packageOf("b_two", json::array({versionFrom("b", "b_two", "2.0.0", "h_b2")})),
    }));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const auto got = catalogVersions(lib);
    EXPECT_EQ(got.size(), 3u);
    EXPECT_EQ(got.at("root_module"), std::vector<std::string>({"1.0.0"}));
    EXPECT_EQ(got.at("b_one"), std::vector<std::string>({"1.0.0"}));
    EXPECT_EQ(got.at("b_two"), std::vector<std::string>({"2.0.0"}));
}

// The UI groups its sections by repositoryUrl. An included package must group
// under the repository the user actually added, or an aggregate catalog renders
// as nothing at all. Provenance travels in the origin* fields instead.
TEST(CatalogIncludes, EntriesAreStampedWithRootAndOrigin) {
    auto f = rootIncluding(json::array({json{{"repo", kBRepo}}}), json::array());
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_one", json::array({versionFrom("b", "b_one", "1.0.0", "h_b1")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json e = catalogEntry(lib, "b_one");
    EXPECT_EQ(e.value("repositoryUrl", ""), lgpd::kDefaultRepositoryUrl);
    EXPECT_EQ(e.value("repositoryName", ""), "root");
    EXPECT_EQ(e.value("originRepositoryUrl", ""), kBRepo);
    EXPECT_EQ(e.value("originRepositoryName", ""), "b");
    // The download still points at the catalog that published it.
    EXPECT_EQ(e["versions"][0].value("url", ""), "https://b.local/b_one-1.0.0.lgx");
    EXPECT_EQ(e["versions"][0].value("originRepositoryUrl", ""), kBRepo);
}

// A bare name in `packages` takes every version of that package and nothing else.
TEST(CatalogIncludes, NameFilterSelectsOnePackage) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo}, {"packages", json::array({"b_two"})}}}),
        json::array());
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_one", json::array({versionFrom("b", "b_one", "1.0.0", "h_b1")})),
        packageOf("b_two", json::array({versionFrom("b", "b_two", "2.0.0", "h_b2"),
                                        versionFrom("b", "b_two", "1.0.0", "h_b2a")})),
    }));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const auto got = catalogVersions(lib);
    EXPECT_EQ(got.count("b_one"), 0u);
    EXPECT_EQ(got.at("b_two"), std::vector<std::string>({"2.0.0", "1.0.0"}));
}

// A bare version string is an exact pin, because that is what the npm range
// dialect already means by it.
TEST(CatalogIncludes, ExactVersionPinSelectsOneRelease) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo},
                          {"packages", json::array({json{{"name", "b_two"},
                                                         {"version", "1.0.0"}}})}}}),
        json::array());
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_two", json::array({versionFrom("b", "b_two", "2.0.0", "h_b2"),
                                        versionFrom("b", "b_two", "1.0.0", "h_b1")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    EXPECT_EQ(catalogVersions(lib).at("b_two"), std::vector<std::string>({"1.0.0"}));
}

// Ranges go through the same semver implementation the dependency resolver
// uses, so the accepted dialect cannot drift between the two.
TEST(CatalogIncludes, VersionRangeSelectsASubset) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo},
                          {"packages", json::array({json{{"name", "b_two"},
                                                         {"version", "^1.0.0"}}})}}}),
        json::array());
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_two", json::array({versionFrom("b", "b_two", "2.0.0", "h_c"),
                                        versionFrom("b", "b_two", "1.2.0", "h_b"),
                                        versionFrom("b", "b_two", "1.0.0", "h_a")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    EXPECT_EQ(catalogVersions(lib).at("b_two"),
              std::vector<std::string>({"1.2.0", "1.0.0"}));
}

// rootHash disambiguates two builds that share a version.
TEST(CatalogIncludes, RootHashPinSelectsOneBuild) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo},
                          {"packages", json::array({json{{"name", "b_two"},
                                                         {"rootHash", "h_second"}}})}}}),
        json::array());
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_two", json::array({versionFrom("b", "b_two", "1.0.0", "h_first"),
                                        versionFrom("b", "b_two", "1.0.0", "h_second")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json e = catalogEntry(lib, "b_two");
    ASSERT_EQ(e["versions"].size(), 1u);
    EXPECT_EQ(e["versions"][0].value("rootHash", ""), "h_second");
}

// Two selectors naming ONE package union rather than intersect: a settled
// release plus one build under test, without a range contrived to span both.
// `rootHash` has no range form, so several builds can only be asked for that way.
TEST(CatalogIncludes, TwoSelectorsForOnePackageUnion) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo},
                          {"packages", json::array({
                               json{{"name", "b_two"}, {"version", "1.0.0"}},
                               json{{"name", "b_two"}, {"rootHash", "h_three"}}})}}}),
        json::array());
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_two", json::array({versionFrom("b", "b_two", "3.0.0", "h_three"),
                                        versionFrom("b", "b_two", "2.0.0", "h_two"),
                                        versionFrom("b", "b_two", "1.0.0", "h_one")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    // Neither selector names 2.0.0; intersecting the two would have kept nothing.
    EXPECT_EQ(catalogVersions(lib).at("b_two"),
              std::vector<std::string>({"3.0.0", "1.0.0"}));
}

// The other half of that rule: WITHIN one selector `version` and `rootHash`
// both have to hold, so the pair names a build rather than either field alone.
TEST(CatalogIncludes, VersionAndRootHashInOneSelectorIntersect) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo},
                          {"packages", json::array({json{{"name", "b_two"},
                                                         {"version", "1.0.0"},
                                                         {"rootHash", "h_second"}}})}}}),
        json::array());
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_two", json::array({versionFrom("b", "b_two", "2.0.0", "h_second"),
                                        versionFrom("b", "b_two", "1.0.0", "h_second"),
                                        versionFrom("b", "b_two", "1.0.0", "h_first")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    // 2.0.0 shares the hash and 1.0.0/h_first the version; only the pair passes.
    const json e = catalogEntry(lib, "b_two");
    ASSERT_EQ(e["versions"].size(), 1u);
    EXPECT_EQ(e["versions"][0]["manifest"].value("version", ""), "1.0.0");
    EXPECT_EQ(e["versions"][0].value("rootHash", ""), "h_second");
}

// Two catalogs publishing DIFFERENT versions of one package is the point of an
// include: the versions union rather than one entry replacing the other.
TEST(CatalogIncludes, VersionsFromTwoCatalogsAreUnioned) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo}}}),
        json::array({packageOf("shared",
            json::array({versionFrom("root", "shared", "1.0.0", "h_root")}))}));
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("shared", json::array({versionFrom("b", "shared", "1.1.0", "h_b")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json e = catalogEntry(lib, "shared");
    ASSERT_EQ(e["versions"].size(), 2u);
    EXPECT_EQ(e["versions"][0]["manifest"].value("version", ""), "1.1.0");
    EXPECT_EQ(e["versions"][1]["manifest"].value("version", ""), "1.0.0");
    // Header fields follow versions[0] AFTER the merge — with several catalogs
    // contributing, "first in the file" is no longer a meaningful source.
    EXPECT_EQ(e.value("description", ""), "served by b");
    EXPECT_EQ(e.value("originRepositoryName", ""), "b");
}

// The same name AND version from two catalogs is the real collision. The
// local copy keeps it, and the loser is reported rather than dropped silently.
TEST(CatalogIncludes, LocalCopyWinsACollisionAndTheLossIsReported) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo}}}),
        json::array({packageOf("shared",
            json::array({versionFrom("root", "shared", "1.0.0", "h_root")}))}));
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("shared", json::array({versionFrom("b", "shared", "1.0.0", "h_b")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json e = catalogEntry(lib, "shared");
    ASSERT_EQ(e["versions"].size(), 1u);
    EXPECT_EQ(e["versions"][0].value("rootHash", ""), "h_root");

    const std::string errs = lib.refreshCatalogs();
    EXPECT_NE(errs.find("shared 1.0.0"), std::string::npos);
    EXPECT_NE(errs.find("shadowed"), std::string::npos);
}

// A -> B -> A must terminate. A per-path check, not a global one, so a diamond
// still resolves.
TEST(CatalogIncludes, CycleTerminates) {
    auto f = rootIncluding(json::array({json{{"repo", kBRepo}}}),
                           json::array({packageOf("root_module",
                               json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    f->byUrl[kBRepo]     = repoDoc("b", kBIndex, kBIncludes);
    f->byUrl[kBIncludes] =
        includesDoc(json::array({json{{"repo", lgpd::kDefaultRepositoryUrl}}}));
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_one", json::array({versionFrom("b", "b_one", "1.0.0", "h_b1")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const auto got = catalogVersions(lib);
    EXPECT_EQ(got.size(), 2u);
    EXPECT_EQ(got.count("root_module"), 1u);
    EXPECT_EQ(got.count("b_one"), 1u);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    bool sawCycleWarning = false;
    for (const auto& w : r.value("includeWarnings", json::array()))
        if (w.get<std::string>().find("cycle") != std::string::npos) sawCycleWarning = true;
    EXPECT_TRUE(sawCycleWarning);
}

// Two includes that both reach the same catalog is legitimate: the packages
// resolve once, not zero times.
TEST(CatalogIncludes, DiamondStillResolves) {
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo}}, json{{"repo", kCRepo}}}), json::array());
    // Both B and C include D; D is served at b.local's sibling URL.
    const char* kDRepo  = "https://d.local/logos-repo.json";
    const char* kDIndex = "https://d.local/index.json";
    f->byUrl[kBRepo]     = repoDoc("b", kBIndex, kBIncludes);
    f->byUrl[kBIncludes] = includesDoc(json::array({json{{"repo", kDRepo}}}));
    f->byUrl[kBIndex]    = indexDoc("b", json::array());
    f->byUrl[kCRepo]     = repoDoc("c", kCIndex, kCIncludes);
    f->byUrl[kCIncludes] = includesDoc(json::array({json{{"repo", kDRepo}}}));
    f->byUrl[kCIndex]    = indexDoc("c", json::array());
    f->byUrl[kDRepo]  = repoDoc("d", kDIndex);
    f->byUrl[kDIndex] = indexDoc("d", json::array({
        packageOf("d_one", json::array({versionFrom("d", "d_one", "1.0.0", "h_d")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    // Reached twice, listed once: the second copy collides on name+version and
    // is merged away.
    const json e = catalogEntry(lib, "d_one");
    ASSERT_EQ(e["versions"].size(), 1u);
}

// A catalog the walk cannot read degrades to a recorded error. It must not take
// the including catalog down with it.
TEST(CatalogIncludes, UnreachableIncludeDoesNotBlackoutTheRoot) {
    auto f = rootIncluding(json::array({json{{"repo", kBRepo}}}),
                           json::array({packageOf("root_module",
                               json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    // kBRepo is deliberately not served.

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    EXPECT_EQ(catalogVersions(lib).count("root_module"), 1u);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    ASSERT_EQ(r.value("includes", json::array()).size(), 1u);
    EXPECT_EQ(r["includes"][0].value("url", ""), kBRepo);
    EXPECT_FALSE(r["includes"][0].value("resolveError", "").empty());
    EXPECT_TRUE(r.value("resolveError", "x").empty());
}

// An include is a delegation. A user who does not want it can refuse the whole
// mechanism and see only the catalogs they configured themselves.
TEST(CatalogIncludes, FollowIncludesOffKeepsOnlyConfiguredCatalogs) {
    auto f = rootIncluding(json::array({json{{"repo", kBRepo}}}),
                           json::array({packageOf("root_module",
                               json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_one", json::array({versionFrom("b", "b_one", "1.0.0", "h_b1")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.registry().setFollowIncludes(false);
    lib.setFetcher(f);

    const auto got = catalogVersions(lib);
    EXPECT_EQ(got.size(), 1u);
    EXPECT_EQ(got.count("root_module"), 1u);
}

// A malformed include entry is a warning, not a parse failure: refusing the
// whole logos-repo.json would take a working catalog offline over a typo in a
// purely additive field.
TEST(CatalogIncludes, MalformedEntriesWarnWithoutBreakingTheCatalog) {
    auto f = rootIncluding(
        json::array({
            json{{"repo", "http://insecure.local/logos-repo.json"}},
            json{{"packages", json::array({"x"})}},          // no `repo`
            json{{"repo", kBRepo}, {"packages", json::array()}},  // selects nothing
            json{{"repo", kBRepo}},                          // the good one
        }),
        json::array({packageOf("root_module",
            json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_one", json::array({versionFrom("b", "b_one", "1.0.0", "h_b1")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const auto got = catalogVersions(lib);
    EXPECT_EQ(got.count("root_module"), 1u);
    EXPECT_EQ(got.count("b_one"), 1u);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(r.value("resolveError", "x").empty());
    EXPECT_EQ(r.value("includeWarnings", json::array()).size(), 3u);
}

// Browse and install have to agree: a version the include's filter excluded is
// not installable by name either, or the pin would be advice rather than a rule.
TEST(CatalogIncludes, DownloadHonoursTheIncludeFilter) {
    auto seed = new UrlCapturingFetcher;
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo},
                          {"packages", json::array({json{{"name", "b_two"},
                                                         {"version", "1.0.0"}}})}}}),
        json::array(), seed);
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_two", json::array({versionFrom("b", "b_two", "2.0.0", "h_b2"),
                                        versionFrom("b", "b_two", "1.0.0", "h_b1")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    std::string err;
    EXPECT_TRUE(lib.downloadPackage("", "b_two", err, "2.0.0").empty());
    EXPECT_NE(err.find("2.0.0"), std::string::npos);
    EXPECT_TRUE(seed->seenUrl.empty());

    // The pinned version still installs, straight from the catalog that
    // published it.
    err.clear();
    lib.downloadPackage("", "b_two", err);
    EXPECT_EQ(seed->seenUrl, "https://b.local/b_two-1.0.0.lgx");
}

// `--repo <configured>` covers what that repository offers, includes and all.
// `--repo <included>` scopes to that catalog's own slice.
TEST(CatalogIncludes, RepoScopeResolvesRootAndOriginDifferently) {
    auto f = rootIncluding(json::array({json{{"repo", kBRepo}}}),
                           json::array({packageOf("root_module",
                               json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_one", json::array({versionFrom("b", "b_one", "1.0.0", "h_b1")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json viaRoot = json::parse(lib.getCatalogForRepoJson("root"));
    EXPECT_EQ(viaRoot.size(), 2u);

    const json viaOrigin = json::parse(lib.getCatalogForRepoJson("b"));
    ASSERT_EQ(viaOrigin.size(), 1u);
    EXPECT_EQ(viaOrigin[0].value("name", ""), "b_one");
}

// The graph is third-party input, so it has to be bounded. A chain deeper than
// the cap stops, and says so.
TEST(CatalogIncludes, DepthIsCapped) {
    auto f = rootIncluding(json::array({json{{"repo", "https://lvl1.local/logos-repo.json"}}}),
                           json::array());
    // Six levels, each including the next; the cap is four.
    for (int i = 1; i <= 6; ++i) {
        const std::string base  = "https://lvl" + std::to_string(i) + ".local";
        const std::string next  = "https://lvl" + std::to_string(i + 1) + ".local/logos-repo.json";
        const std::string name  = "lvl" + std::to_string(i);
        const std::string index = base + "/index.json";
        const std::string incUrl = base + "/includes.json";
        f->byUrl[base + "/logos-repo.json"] =
            repoDoc(name.c_str(), index.c_str(), i < 6 ? incUrl : std::string{});
        if (i < 6)
            f->byUrl[incUrl] = includesDoc(json::array({json{{"repo", next}}}));
        f->byUrl[index] = indexDoc(name.c_str(), json::array({
            packageOf(name.c_str(),
                      json::array({versionFrom(name, name.c_str(), "1.0.0", "h")}))}));
    }

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const auto got = catalogVersions(lib);
    EXPECT_EQ(got.count("lvl4"), 1u);
    EXPECT_EQ(got.count("lvl5"), 0u);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    bool sawDepthWarning = false;
    for (const auto& w : r.value("includeWarnings", json::array()))
        if (w.get<std::string>().find("deeper than") != std::string::npos) sawDepthWarning = true;
    EXPECT_TRUE(sawDepthWarning);
}

// `list` and `download` have to name the same release. Selection used to stop
// at the first candidate that could serve the name, so an older copy in the
// including catalog beat a newer one in an included catalog: the catalog
// advertised 1.2.0 and the download fetched 1.0.0.
TEST(CatalogIncludes, DownloadPicksTheVersionTheCatalogAdvertises) {
    auto seed = new UrlCapturingFetcher;
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo}}}),
        json::array({packageOf("shared",
            json::array({versionFrom("root", "shared", "1.0.0", "h_root")}))}),
        seed);
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("shared", json::array({versionFrom("b", "shared", "1.2.0", "h_b")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    // What the catalog advertises as latest.
    const json e = catalogEntry(lib, "shared");
    ASSERT_FALSE(e["versions"].empty());
    EXPECT_EQ(e["versions"][0]["manifest"].value("version", ""), "1.2.0");

    // What an unpinned download actually fetches.
    std::string err;
    lib.downloadPackage("", "shared", err);
    EXPECT_EQ(seed->seenUrl, "https://b.local/shared-1.2.0.lgx");
}

// Ranking must not undo the merge's precedence. Same version in both catalogs
// means the including one keeps it, and the download has to agree.
TEST(CatalogIncludes, DownloadPrefersTheLocalCopyOnAnExactTie) {
    auto seed = new UrlCapturingFetcher;
    auto f = rootIncluding(
        json::array({json{{"repo", kBRepo}}}),
        json::array({packageOf("shared",
            json::array({versionFrom("root", "shared", "1.0.0", "h_root")}))}),
        seed);
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("shared", json::array({versionFrom("b", "shared", "1.0.0", "h_b")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    std::string err;
    lib.downloadPackage("", "shared", err);
    EXPECT_EQ(seed->seenUrl, "https://root.local/shared-1.0.0.lgx");
}

// ─── the includes document ────────────────────────────────────────────────────
//
// `includesUrl` is a second fetch, so it is a second thing that can fail. None
// of these may cost the catalog its own packages: an unreadable composition
// list says nothing about what the catalog itself publishes.

TEST(IncludesDocument, UnreachableDocumentIsAWarningNotAnError) {
    auto f = rootIncluding(json::array({json{{"repo", kBRepo}}}),
                           json::array({packageOf("root_module",
                               json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    f->byUrl.erase(kRootIncludes);  // declared, not served

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    EXPECT_EQ(catalogVersions(lib).count("root_module"), 1u);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(r.value("resolveError", "x").empty());
    EXPECT_EQ(r.value("includes", json::array()).size(), 0u);
    ASSERT_EQ(r.value("includeWarnings", json::array()).size(), 1u);
    EXPECT_NE(r["includeWarnings"][0].get<std::string>().find("unavailable"),
              std::string::npos);
}

TEST(IncludesDocument, MalformedDocumentIsAWarningNotAnError) {
    auto f = rootIncluding(json::array(),
                           json::array({packageOf("root_module",
                               json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    f->byUrl[kRootIncludes] = "{ not json";

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    EXPECT_EQ(catalogVersions(lib).count("root_module"), 1u);
    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(r.value("resolveError", "x").empty());
    EXPECT_EQ(r.value("includeWarnings", json::array()).size(), 1u);
}

// The wrapper is what makes the document versionable. A bare array — the shape
// the list had while it lived inline — is not it.
TEST(IncludesDocument, DocumentWithoutAnIncludesArrayWarns) {
    auto f = rootIncluding(json::array(), json::array());
    f->byUrl[kRootIncludes] = json::array({json{{"repo", kBRepo}}}).dump();

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    ASSERT_EQ(r.value("includeWarnings", json::array()).size(), 1u);
    EXPECT_NE(r["includeWarnings"][0].get<std::string>().find("bare array"),
              std::string::npos);
}

// The other shape of the same mistake: an object that simply forgot the key.
TEST(IncludesDocument, ObjectWithoutAnIncludesKeyWarns) {
    auto f = rootIncluding(json::array(), json::array());
    f->byUrl[kRootIncludes] = json{{"schemaVersion", 1}}.dump();

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    ASSERT_EQ(r.value("includeWarnings", json::array()).size(), 1u);
    EXPECT_NE(r["includeWarnings"][0].get<std::string>().find("'includes' array"),
              std::string::npos);
}

// schemaVersion is advisory here, as it is everywhere else in this format. A
// document that omits it still resolves rather than silently contributing
// nothing.
TEST(IncludesDocument, SchemaVersionIsAdvisory) {
    auto f = rootIncluding(json::array(), json::array());
    f->byUrl[kRootIncludes] =
        json{{"includes", json::array({json{{"repo", kBRepo}}})}}.dump();
    f->byUrl[kBRepo]  = repoDoc("b", kBIndex);
    f->byUrl[kBIndex] = indexDoc("b", json::array({
        packageOf("b_one", json::array({versionFrom("b", "b_one", "1.0.0", "h_b1")}))}));

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    EXPECT_EQ(catalogVersions(lib).count("b_one"), 1u);
}

TEST(IncludesDocument, NonHttpsUrlIsRefusedWithAWarning) {
    auto f = rootIncluding(json::array(), json::array());
    f->repoJson = repoDoc("root", kIndexUrl, "http://insecure.local/includes.json");

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(r.value("resolveError", "x").empty());
    ASSERT_EQ(r.value("includeWarnings", json::array()).size(), 1u);
    EXPECT_NE(r["includeWarnings"][0].get<std::string>().find("https"),
              std::string::npos);
}

// A non-string here used to throw out of the whole logos-repo.json parse and
// drop a repository that was otherwise fine — the same shape as the bug where
// one array-valued manifest field discarded an entire catalog.
TEST(IncludesDocument, NonStringIncludesUrlDoesNotDropTheRepository) {
    auto f = rootIncluding(json::array(),
                           json::array({packageOf("root_module",
                               json::array({versionFrom("root", "root_module", "1.0.0", "h_r")}))}));
    json repo = json::parse(f->repoJson);
    repo["includesUrl"] = json::array({"https://b.local/includes.json"});
    f->repoJson = repo.dump();

    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    EXPECT_EQ(catalogVersions(lib).count("root_module"), 1u);
    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    EXPECT_TRUE(r.value("resolveError", "x").empty());
    EXPECT_EQ(r.value("includeWarnings", json::array()).size(), 1u);
}

// The declared URL is reported whether or not anything resolved from it —
// otherwise a catalog that meant to draw from others looks like one that never
// tried.
TEST(IncludesDocument, DeclaredUrlIsReportedEvenWhenItResolvesToNothing) {
    auto f = rootIncluding(json::array(), json::array());
    lgpd::PackageDownloaderLib lib;
    lib.setFetcher(f);

    const json r = repoByUrl(lib, lgpd::kDefaultRepositoryUrl);
    EXPECT_EQ(r.value("includesUrl", ""), kRootIncludes);
    EXPECT_EQ(r.value("includes", json::array()).size(), 0u);
}
