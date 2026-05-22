// Logos package downloader — multi-repository client.
//
// This file implements:
//   • `lgpd::RepositoryRegistry` — manages the hardcoded default repo plus
//     a user-managed list persisted as JSON. Fetches `logos-repo.json`
//     from each entry at refresh time to resolve canonical metadata.
//   • `lgpd::PackageDownloaderLib` — merges every enabled repo's
//     `index.json` into one catalog, downloads `.lgx` files by their URL,
//     and performs semver-aware cross-repo dependency resolution.
//
// All persisted JSON shapes are documented in the plan at
// /Users/dlipicar/.claude/plans/i-want-to-make-virtual-journal.md
// (sections "Client-side repository config", "`logos-repo.json` schema",
// and "`index.json` schema").

#include "package_downloader_lib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_map>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <lgx.h>   // lgx_load / lgx_get_manifest_json / lgx_verify_signature

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lgpd {

const char* kDefaultRepositoryUrl =
    "https://raw.githubusercontent.com/logos-co/logos-modules-release/refs/heads/main/logos-repo.json";

namespace {

// ─── Small string utilities ──────────────────────────────────────────────────

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    size_t b = s.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

// nlohmann::json::value(key, default) returns `default` only when the key is
// absent; if the key is present with a `null` value, value() returns the null
// itself and any chained .value() call on it throws json::type_error 306
// ("cannot use value() with null"). The catalog has `manifest: null` rows in
// practice (an `index.json` produced by an early action run), so chains like
//   v.value("manifest", json::object()).value("version", "")
// will crash on those rows.
//
// objOrEmpty returns a reference to either the child JSON object or a static
// empty object — never a null. Use it at the top of any chain that descends
// into an optional nested object.
const json& objOrEmpty(const json& parent, const char* key) {
    static const json kEmpty = json::object();
    if (!parent.is_object()) return kEmpty;
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_object()) return kEmpty;
    return *it;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// ─── Semver parsing & matching ────────────────────────────────────────────────
//
// We implement the npm/Cargo-flavoured subset that covers everything in the
// validation regex used by `lgx`: caret, tilde, comparison operators, wildcard
// segments, hyphen-tagged pre-release, simple alternatives via `||`, and
// whitespace-separated conjunctions like ">=1.2 <2.0".
//
// This is NOT a full semver 2.0 reference impl — build metadata is ignored
// for comparison and pre-release ordering follows the basic rule "X-pre < X"
// only when both sides have the same major/minor/patch.

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string pre;   // pre-release identifier without leading '-'

    bool isValid() const { return major >= 0 && minor >= 0 && patch >= 0; }
};

bool parseSemVer(const std::string& v, SemVer& out) {
    // Strip optional leading 'v'/'V'
    size_t start = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    std::string s = v.substr(start);
    // Build metadata stripped at '+'
    size_t plus = s.find('+');
    if (plus != std::string::npos) s = s.substr(0, plus);
    size_t dash = s.find('-');
    std::string pre;
    if (dash != std::string::npos) { pre = s.substr(dash + 1); s = s.substr(0, dash); }
    auto parts = split(s, '.');
    if (parts.empty() || parts.size() > 3) return false;
    SemVer sv;
    auto parsePart = [](const std::string& p, int& dst) -> bool {
        if (p.empty()) return false;
        for (char c : p) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        try { dst = std::stoi(p); } catch (...) { return false; }
        return true;
    };
    if (!parsePart(parts[0], sv.major)) return false;
    if (parts.size() >= 2 && !parsePart(parts[1], sv.minor)) return false;
    if (parts.size() == 3 && !parsePart(parts[2], sv.patch)) return false;
    sv.pre = pre;
    out = sv;
    return true;
}

int compareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    // Pre-release: present sorts before absent.
    if (a.pre.empty() && b.pre.empty()) return 0;
    if (a.pre.empty()) return 1;
    if (b.pre.empty()) return -1;
    if (a.pre == b.pre) return 0;
    return a.pre < b.pre ? -1 : 1;
}

// Matches a single comparator like ">=1.2.0", "^1.2", "~1.2.3", "1.2.x".
// `version` must already be parsed.
bool matchSingle(const std::string& comparatorRaw, const SemVer& version) {
    std::string c = trim(comparatorRaw);
    if (c.empty()) return false;
    if (c == "*" || c == "x" || c == "X" || c == "latest") return true;

    // Operator parsing
    std::string op;
    size_t i = 0;
    if (c.substr(0, 2) == ">=" || c.substr(0, 2) == "<=") { op = c.substr(0, 2); i = 2; }
    else if (c[0] == '>' || c[0] == '<' || c[0] == '=' || c[0] == '^' || c[0] == '~') {
        op = std::string(1, c[0]); i = 1;
    } else {
        op = "=";
    }
    std::string body = trim(c.substr(i));
    if (body.empty()) return false;

    // Expand wildcard / partial versions.
    auto parts = split(body, '.');
    bool wildMajor = false, wildMinor = false, wildPatch = false;
    auto isWild = [](const std::string& p) {
        return p == "*" || p == "x" || p == "X" || p.empty();
    };
    if (parts.size() >= 1 && isWild(parts[0])) wildMajor = true;
    if (parts.size() >= 2 && isWild(parts[1])) wildMinor = true;
    if (parts.size() >= 3 && isWild(parts[2])) wildPatch = true;
    if (parts.size() < 3 && (op == "=" || op == "^" || op == "~")) {
        // Partial X-only / X.Y patterns mean "any patch", "any minor.patch"
        if (parts.size() < 2) wildMinor = true;
        if (parts.size() < 3) wildPatch = true;
    }

    auto fillZero = [&](std::vector<std::string>& p) {
        while (p.size() < 3) p.push_back("0");
        for (size_t k = 0; k < p.size(); ++k) {
            if (isWild(p[k])) p[k] = "0";
        }
    };
    auto pCopy = parts;
    fillZero(pCopy);
    SemVer body0;
    if (!parseSemVer(pCopy[0] + "." + pCopy[1] + "." + pCopy[2], body0)) return false;

    if (op == "=") {
        if (wildMajor) return true;
        if (wildMinor) return version.major == body0.major;
        if (wildPatch) return version.major == body0.major && version.minor == body0.minor;
        return compareSemVer(version, body0) == 0;
    }
    if (op == ">")  return compareSemVer(version, body0) >  0;
    if (op == "<")  return compareSemVer(version, body0) <  0;
    if (op == ">=") return compareSemVer(version, body0) >= 0;
    if (op == "<=") return compareSemVer(version, body0) <= 0;
    if (op == "^") {
        // Caret: compatible within the leftmost non-zero element.
        SemVer hi = body0;
        if (body0.major > 0)      { hi.major = body0.major + 1; hi.minor = 0; hi.patch = 0; }
        else if (body0.minor > 0) { hi.minor = body0.minor + 1; hi.patch = 0; }
        else                      { hi.patch = body0.patch + 1; }
        return compareSemVer(version, body0) >= 0 && compareSemVer(version, hi) < 0;
    }
    if (op == "~") {
        // Tilde: patch-level updates only when minor is specified, else
        // minor-level when only major is specified.
        SemVer hi = body0;
        if (parts.size() >= 2) { hi.minor = body0.minor + 1; hi.patch = 0; }
        else                   { hi.major = body0.major + 1; hi.minor = 0; hi.patch = 0; }
        return compareSemVer(version, body0) >= 0 && compareSemVer(version, hi) < 0;
    }
    return false;
}

bool semverRangeMatches(const std::string& range, const std::string& version) {
    if (range.empty()) return true;  // no constraint == match anything
    SemVer v;
    if (!parseSemVer(version, v)) return false;
    // Top-level disjunction is `||`.
    for (auto& alt : split(range, '|')) {
        // split by '|' splits "a||b" into ["a","","b"] — collapse the empty.
        // We'll re-split by whitespace below for conjunction.
        std::string a = trim(alt);
        if (a.empty()) continue;
        // Conjunction: whitespace-separated comparators must all match.
        std::istringstream iss(a);
        std::string tok;
        bool ok = true;
        bool sawTok = false;
        while (iss >> tok) {
            sawTok = true;
            if (!matchSingle(tok, v)) { ok = false; break; }
        }
        if (sawTok && ok) return true;
    }
    return false;
}

// ─── libcurl fetcher ──────────────────────────────────────────────────────────

class CurlGlobalInit {
public:
    CurlGlobalInit()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};

CurlGlobalInit& curlInit() {
    static CurlGlobalInit g;
    return g;
}

size_t curlWriteMem(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

size_t curlWriteFile(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* file = static_cast<std::ofstream*>(userp);
    file->write(static_cast<const char*>(contents),
                static_cast<std::streamsize>(size * nmemb));
    return file->good() ? size * nmemb : 0;
}

// Append a unique cache-busting query param.
//
// The metadata we GET (logos-repo.json from raw.githubusercontent.com,
// index.json from a GitHub release asset) is mutable but sits behind
// Fastly. After a catalog rebuild the edge can keep serving the OLD
// body for a window even with `Cache-Control: no-cache` (that means
// "revalidate", and the release-download 302 itself is edge-cached).
// A unique query string changes the cache key → guaranteed origin
// fetch → the client never shows a removed/renamed module just
// because it reloaded too soon after an upstream change. Unbounded
// freshness matters more than CDN offload for these tiny JSON files;
// the actual `.lgx` blobs (getToFile) are immutable per version and
// are intentionally left cacheable.
std::string cacheBustedUrl(const std::string& url) {
    static std::atomic<unsigned long long> seq{0};
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const unsigned long long n = seq.fetch_add(1, std::memory_order_relaxed);
    const char sep = (url.find('?') == std::string::npos) ? '?' : '&';
    return url + sep + "_lgpd_cb=" +
           std::to_string(static_cast<unsigned long long>(ns)) + "-" +
           std::to_string(n);
}

class HttpsFetcher : public Fetcher {
public:
    bool get(const std::string& url, std::string& out) override {
        curlInit();
        CURL* c = curl_easy_init();
        if (!c) return false;
        out.clear();
        // Cache-buster + no-cache headers so a reload right after an
        // upstream catalog change never re-serves a stale edge copy.
        const std::string busted = cacheBustedUrl(url);
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Cache-Control: no-cache, no-store, max-age=0");
        hdrs = curl_slist_append(hdrs, "Pragma: no-cache");
        curl_easy_setopt(c, CURLOPT_URL, busted.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteMem);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "lgpd/2.0");
        CURLcode res = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return res == CURLE_OK && code >= 200 && code < 300;
    }

    bool getToFile(const std::string& url, const std::string& path) override {
        curlInit();
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return false;
        CURL* c = curl_easy_init();
        if (!c) return false;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteFile);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &f);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "lgpd/2.0");
        CURLcode res = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
        f.close();
        return res == CURLE_OK && code >= 200 && code < 300;
    }
};

// ─── logos-repo.json + index.json parsers ─────────────────────────────────────

bool parseLogosRepoJson(const std::string& body, Repository& dst, std::string& err) {
    try {
        auto j = json::parse(body);
        if (!j.is_object()) { err = "logos-repo.json is not a JSON object"; return false; }
        auto must = [&](const char* k, bool isString = true) {
            if (!j.contains(k)) { err = std::string("missing field '") + k + "'"; return false; }
            if (isString && !j[k].is_string()) { err = std::string("field '") + k + "' is not a string"; return false; }
            return true;
        };
        if (!must("name")) return false;
        if (!must("displayName")) return false;
        if (!must("indexUrl")) return false;
        dst.name        = j["name"].get<std::string>();
        dst.displayName = j["displayName"].get<std::string>();
        dst.description = j.value("description", "");
        dst.homepage    = j.value("homepage", "");
        dst.indexUrl    = j["indexUrl"].get<std::string>();
        dst.trustedSignerDids.clear();
        if (j.contains("trustedSigners") && j["trustedSigners"].is_array()) {
            for (const auto& s : j["trustedSigners"]) {
                if (s.is_object() && s.contains("did") && s["did"].is_string()) {
                    dst.trustedSignerDids.push_back(s["did"].get<std::string>());
                }
            }
        }
        return true;
    } catch (const json::exception& e) {
        err = std::string("JSON parse error: ") + e.what();
        return false;
    }
}

bool isHttpsUrl(const std::string& url) {
    return url.rfind("https://", 0) == 0;
}

// Compare two version-entries-by-date-desc for `index.json#packages[].versions[]`.
// Null entries in versions[] would otherwise crash value() with type_error
// 306 — fall back to an empty date for those, which sorts them last.
struct VersionDateDesc {
    bool operator()(const json& a, const json& b) const {
        const std::string da = a.is_object() ? a.value("releasedAt", "") : std::string();
        const std::string db = b.is_object() ? b.value("releasedAt", "") : std::string();
        return da > db;
    }
};

} // namespace

// ─── RepositoryRegistry::Impl ─────────────────────────────────────────────────

struct RepositoryRegistry::Impl {
    std::string configPath;
    bool persistent = false;
    bool defaultDisabled = false;
    std::vector<Repository> userRepos;        // persisted (url + enabled only)
    Repository defaultRepo;                   // always present
    std::shared_ptr<Fetcher> fetcher;
    mutable std::mutex mu;

    Impl() {
        defaultRepo.url = kDefaultRepositoryUrl;
        defaultRepo.isDefault = true;
        defaultRepo.enabled = true;
        fetcher = std::make_shared<HttpsFetcher>();
    }

    void load() {
        if (!persistent) return;
        std::ifstream f(configPath);
        if (!f.is_open()) return;
        try {
            json j; f >> j;
            defaultDisabled = j.value("defaultDisabled", false);
            userRepos.clear();
            if (j.contains("repositories") && j["repositories"].is_array()) {
                for (const auto& r : j["repositories"]) {
                    if (!r.is_object() || !r.contains("url") || !r["url"].is_string()) continue;
                    Repository repo;
                    repo.url = r["url"].get<std::string>();
                    repo.enabled = r.value("enabled", true);
                    userRepos.push_back(std::move(repo));
                }
            }
        } catch (...) {
            // Ignore parse errors; treat as empty config.
        }
    }

    std::string save() {
        if (!persistent) return "registry is in-memory only (pass --config <path>)";
        json j;
        j["schemaVersion"] = 1;
        j["defaultDisabled"] = defaultDisabled;
        json arr = json::array();
        for (const auto& r : userRepos) {
            json e;
            e["url"] = r.url;
            e["enabled"] = r.enabled;
            arr.push_back(std::move(e));
        }
        j["repositories"] = std::move(arr);
        std::error_code ec;
        fs::create_directories(fs::path(configPath).parent_path(), ec);
        std::ofstream f(configPath);
        if (!f.is_open()) return "cannot write config file: " + configPath;
        f << j.dump(2);
        return f.good() ? std::string() : ("write failed: " + configPath);
    }

    void refreshOne(Repository& r) {
        r.resolveError.clear();
        if (!isHttpsUrl(r.url)) {
            r.resolveError = "unsupported URL scheme (https required in v1)";
            return;
        }
        std::string body;
        if (!fetcher->get(r.url, body)) {
            r.resolveError = "fetch failed: " + r.url;
            return;
        }
        std::string err;
        Repository parsed = r;  // keep url/enabled/isDefault
        parsed.name.clear();
        parsed.displayName.clear();
        parsed.description.clear();
        parsed.homepage.clear();
        parsed.indexUrl.clear();
        parsed.trustedSignerDids.clear();
        if (!parseLogosRepoJson(body, parsed, err)) {
            r.resolveError = "logos-repo.json: " + err;
            return;
        }
        r = std::move(parsed);
    }
};

RepositoryRegistry::RepositoryRegistry() : impl_(std::make_unique<Impl>()) {}

RepositoryRegistry::RepositoryRegistry(std::string configPath)
    : impl_(std::make_unique<Impl>()) {
    impl_->configPath = std::move(configPath);
    impl_->persistent = !impl_->configPath.empty();
    impl_->load();
}

RepositoryRegistry::~RepositoryRegistry() = default;

void RepositoryRegistry::setFetcher(std::shared_ptr<Fetcher> fetcher) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->fetcher = std::move(fetcher);
}

std::vector<Repository> RepositoryRegistry::list() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<Repository> out;
    Repository def = impl_->defaultRepo;
    def.enabled = !impl_->defaultDisabled;
    out.push_back(def);
    for (const auto& r : impl_->userRepos) out.push_back(r);
    return out;
}

std::string RepositoryRegistry::addRepository(const std::string& url) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->persistent) return "no config file (pass --config <path>)";
    if (url.empty()) return "url is empty";
    if (url == impl_->defaultRepo.url) return "cannot add the default repository";
    if (!isHttpsUrl(url)) return "unsupported URL scheme (https required in v1)";
    for (const auto& r : impl_->userRepos) {
        if (r.url == url) return "already registered: " + url;
    }
    Repository r;
    r.url = url;
    r.enabled = true;
    // Try to resolve metadata now so the caller learns about a bad URL
    // immediately instead of at first list time.
    impl_->refreshOne(r);
    if (!r.resolveError.empty()) return r.resolveError;
    impl_->userRepos.push_back(std::move(r));
    return impl_->save();
}

std::string RepositoryRegistry::removeRepository(const std::string& url) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->persistent) return "no config file (pass --config <path>)";
    if (url == impl_->defaultRepo.url) return "cannot remove the default repository";
    auto it = std::find_if(impl_->userRepos.begin(), impl_->userRepos.end(),
                           [&](const Repository& r) { return r.url == url; });
    if (it == impl_->userRepos.end()) return "not registered: " + url;
    impl_->userRepos.erase(it);
    return impl_->save();
}

std::string RepositoryRegistry::setEnabled(const std::string& url, bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (url == impl_->defaultRepo.url) {
        if (!impl_->persistent) return "no config file (pass --config <path>)";
        impl_->defaultDisabled = !enabled;
        return impl_->save();
    }
    if (!impl_->persistent) return "no config file (pass --config <path>)";
    for (auto& r : impl_->userRepos) {
        if (r.url == url) {
            r.enabled = enabled;
            return impl_->save();
        }
    }
    return "not registered: " + url;
}

std::string RepositoryRegistry::refresh() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<std::string> errs;
    impl_->refreshOne(impl_->defaultRepo);
    if (!impl_->defaultRepo.resolveError.empty()) {
        errs.push_back("default: " + impl_->defaultRepo.resolveError);
    }
    for (auto& r : impl_->userRepos) {
        impl_->refreshOne(r);
        if (!r.resolveError.empty()) errs.push_back(r.url + ": " + r.resolveError);
    }
    if (errs.empty()) return {};
    std::string out;
    for (auto& e : errs) { out += e; out += '\n'; }
    return out;
}

std::optional<Repository> RepositoryRegistry::findByUrlOrName(const std::string& s) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto match = [&](const Repository& r) {
        return r.url == s || (!r.name.empty() && r.name == s);
    };
    if (match(impl_->defaultRepo)) {
        Repository copy = impl_->defaultRepo;
        copy.enabled = !impl_->defaultDisabled;
        return copy;
    }
    for (const auto& r : impl_->userRepos) {
        if (match(r)) return r;
    }
    return std::nullopt;
}

bool RepositoryRegistry::isPersistent() const { return impl_->persistent; }
std::string RepositoryRegistry::configPath() const { return impl_->configPath; }

// ─── PackageDownloaderLib::Impl ───────────────────────────────────────────────

struct PackageDownloaderLib::Impl {
    RepositoryRegistry registry;
    std::shared_ptr<Fetcher> fetcher = std::make_shared<HttpsFetcher>();

    // Caches: url -> body
    std::unordered_map<std::string, std::string> indexJsonByRepoUrl;
    mutable std::mutex mu;

    Impl() {}
    explicit Impl(std::string configPath) : registry(std::move(configPath)) {}

    bool ensureMetadata() {
        // Lazy refresh — only do it once per process unless explicitly
        // re-requested. Best-effort: errors are tolerated, since some repos
        // may be unreachable while others work.
        // (refresh() is idempotent and re-fetches every time it is called.)
        registry.refresh();
        return true;
    }

    std::string fetchIndex(const Repository& r) {
        if (r.indexUrl.empty()) return {};
        {
            std::lock_guard<std::mutex> lock(mu);
            auto it = indexJsonByRepoUrl.find(r.url);
            if (it != indexJsonByRepoUrl.end()) return it->second;
        }
        std::string body;
        if (!fetcher->get(r.indexUrl, body)) return {};
        std::lock_guard<std::mutex> lock(mu);
        indexJsonByRepoUrl[r.url] = body;
        return body;
    }

    void clearCaches() {
        std::lock_guard<std::mutex> lock(mu);
        indexJsonByRepoUrl.clear();
    }
};

PackageDownloaderLib::PackageDownloaderLib()
    : impl_(std::make_unique<Impl>()) {}

PackageDownloaderLib::PackageDownloaderLib(std::string configPath)
    : impl_(std::make_unique<Impl>(std::move(configPath))) {}

PackageDownloaderLib::~PackageDownloaderLib() = default;

void PackageDownloaderLib::setFetcher(std::shared_ptr<Fetcher> fetcher) {
    impl_->fetcher = fetcher;
    impl_->registry.setFetcher(fetcher);
    impl_->clearCaches();
}

RepositoryRegistry& PackageDownloaderLib::registry() { return impl_->registry; }
const RepositoryRegistry& PackageDownloaderLib::registry() const { return impl_->registry; }

std::string PackageDownloaderLib::listRepositoriesJson() {
    impl_->ensureMetadata();
    json arr = json::array();
    for (const auto& r : impl_->registry.list()) {
        json e;
        e["url"] = r.url;
        e["enabled"] = r.enabled;
        e["isDefault"] = r.isDefault;
        e["name"] = r.name;
        e["displayName"] = r.displayName;
        e["description"] = r.description;
        e["homepage"] = r.homepage;
        e["indexUrl"] = r.indexUrl;
        e["trustedSignerDids"] = r.trustedSignerDids;
        e["resolveError"] = r.resolveError;
        arr.push_back(std::move(e));
    }
    return arr.dump();
}

std::string PackageDownloaderLib::getCatalogJson() {
    impl_->ensureMetadata();
    json out = json::array();
    for (const auto& r : impl_->registry.list()) {
        if (!r.enabled) continue;
        if (!r.resolveError.empty()) continue;
        std::string body = impl_->fetchIndex(r);
        if (body.empty()) continue;
        try {
            auto idx = json::parse(body);
            if (!idx.is_object() || !idx.contains("packages")
                || !idx["packages"].is_array()) continue;
            for (auto& pkg : idx["packages"]) {
                if (!pkg.is_object() || !pkg.contains("name")) continue;
                json entry;
                entry["repositoryUrl"]  = r.url;
                entry["repositoryName"] = r.name.empty() ? r.url : r.name;
                entry["repositoryDisplayName"] = r.displayName;
                entry["name"] = pkg["name"];
                // Pull the rest of the manifest "header" fields from the
                // first version's embedded manifest (these don't change
                // across versions of the same package).
                if (pkg.contains("versions") && pkg["versions"].is_array() && !pkg["versions"].empty()) {
                    // Null-safe: an index.json entry with `manifest: null`
                    // (early action runs produced these) would otherwise
                    // crash on the chained value() call below.
                    const json& firstManifest = objOrEmpty(pkg["versions"][0], "manifest");
                    entry["description"] = firstManifest.value("description", "");
                    entry["type"]        = firstManifest.value("type", "");
                    entry["category"]    = firstManifest.value("category", "");
                    entry["author"]      = firstManifest.value("author", "");
                    entry["icon"]        = firstManifest.value("icon", "");
                }
                // Versions sorted by releasedAt desc.
                auto versions = pkg.value("versions", json::array());
                std::stable_sort(versions.begin(), versions.end(), VersionDateDesc{});
                entry["versions"] = std::move(versions);
                out.push_back(std::move(entry));
            }
        } catch (...) {
            // Skip unparseable indexes; they show up in resolveError if you
            // call listRepositoriesJson().
        }
    }
    return out.dump();
}

std::string PackageDownloaderLib::getCatalogForRepoJson(const std::string& urlOrName) {
    auto repo = impl_->registry.findByUrlOrName(urlOrName);
    if (!repo) return "[]";
    impl_->ensureMetadata();
    std::string body = impl_->fetchIndex(*repo);
    if (body.empty()) return "[]";
    try {
        auto idx = json::parse(body);
        if (!idx.is_object() || !idx.contains("packages")) return "[]";
        return idx["packages"].dump();
    } catch (...) { return "[]"; }
}

std::string PackageDownloaderLib::refreshCatalogs() {
    impl_->clearCaches();
    return impl_->registry.refresh();
}

namespace {

// Find a version entry within a package by version+rootHash. Returns nullptr
// if no candidate matches.
const json* pickVersion(const json& pkg,
                        const std::string& version,
                        const std::string& rootHash) {
    if (!pkg.is_object() || !pkg.contains("versions") || !pkg["versions"].is_array())
        return nullptr;
    const json* best = nullptr;
    std::string bestDate;
    for (const auto& v : pkg["versions"]) {
        // Skip null/array/scalar entries — value() on a non-object throws
        // type_error 306. The catalog may have been generated by an older
        // index-builder that wrote partial entries; better to skip than crash.
        if (!v.is_object()) continue;
        std::string vVer  = objOrEmpty(v, "manifest").value("version", "");
        std::string vHash = v.value("rootHash", "");
        std::string vDate = v.value("releasedAt", "");
        if (!version.empty() && vVer != version) continue;
        if (!rootHash.empty() && vHash != rootHash) continue;
        if (!best || vDate > bestDate) { best = &v; bestDate = vDate; }
    }
    return best;
}

} // namespace

namespace {

// Verify that a freshly-downloaded `.lgx` is structurally sound AND is
// the artifact the catalog advertised.
//
// Two layers:
//  1. Structural — lgx_verify confirms the file is a well-formed .lgx
//     whose internal content hashes are self-consistent. Catches a
//     truncated / corrupt download (or a non-.lgx file served at the
//     URL) before it ever reaches the installer. Runs unconditionally.
//  2. Index→file binding — the index.json comes from one host, the
//     .lgx `url` from another; nothing otherwise stops a swapped file
//     (a downgrade attack, or a different-but-also-signed sibling
//     package). We bind them three ways:
//       * rootHash  — the package's content Merkle root must equal the
//                     hash the catalog pinned. This is the strongest
//                     check: it covers a content swap that kept the
//                     manifest byte-identical (manifest.json carries
//                     no content hashes).
//       * manifest  — name / version / main / dependencies / type must
//                     match the manifest the catalog embedded.
//       * signer    — if the catalog advertised a signer DID, the file
//                     must be signed by the SAME DID.
//
// Ed25519 trust (is the signer in our keyring) stays package_manager's
// job at install time — here we only confirm the file is what the
// index said it would be.
//
// `indexEntry` is the catalog version entry — `{ manifest, signature,
// rootHash, url, ... }`.
//
// Returns true (accept) on a match, or when the index carried nothing
// to compare a given facet against (legacy rows with `manifest: null`
// / no `rootHash` — we can't verify what wasn't advertised). Returns
// false (reject) on a real mismatch, with `errMsg` set.
bool verifyDownloadAgainstIndex(const std::string& lgxPath,
                                const json& indexEntry,
                                std::string& errMsg) {
    // ── 1. Structural soundness ──────────────────────────────────────
    {
        lgx_verify_result_t vr = lgx_verify(lgxPath.c_str());
        const bool ok = vr.valid;
        std::string firstErr;
        if (!ok && vr.errors && vr.errors[0])
            firstErr = vr.errors[0];
        lgx_free_verify_result(vr);
        if (!ok) {
            errMsg = "downloaded file failed .lgx structure verification"
                   + (firstErr.empty() ? std::string()
                                       : (": " + firstErr));
            return false;
        }
    }

    // Load once for the manifest + root-hash reads below.
    lgx_package_t pkg = lgx_load(lgxPath.c_str());
    if (!pkg) {
        errMsg = "downloaded file is not a readable .lgx package";
        return false;
    }

    // ── 2a. rootHash binding ─────────────────────────────────────────
    // Strongest of the three: a content fingerprint. An attacker who
    // rebuilds a clean (lgx_verify-passing) .lgx with a malicious
    // payload but an unchanged manifest is caught only here.
    const std::string advRootHash = indexEntry.value("rootHash", "");
    if (!advRootHash.empty()) {
        const char* rhRaw = lgx_get_root_hash(pkg);
        const std::string fileRootHash = rhRaw ? rhRaw : "";
        if (fileRootHash != advRootHash) {
            errMsg = "downloaded package content hash does not match the "
                     "catalog (expected " + advRootHash + ", got "
                   + (fileRootHash.empty() ? "none" : fileRootHash) + ")";
            lgx_free_package(pkg);
            return false;
        }
    }

    // ── 2b. Manifest binding ─────────────────────────────────────────
    // Skipped when the index row has no manifest (the `manifest: null`
    // rows objOrEmpty was written for).
    const json& advertised = objOrEmpty(indexEntry, "manifest");
    if (!advertised.empty()) {
        const char* mraw = lgx_get_manifest_json(pkg);
        std::string fileManifestStr = mraw ? mraw : "";

        json fileManifest;
        try { fileManifest = json::parse(fileManifestStr); }
        catch (...) {
            errMsg = "downloaded package has an unparseable manifest";
            lgx_free_package(pkg);
            return false;
        }

        // Compare the security-relevant fields rather than requiring
        // whole-manifest byte-equality: the index builder may
        // normalise the manifest copy it embeds (key order, an added
        // display-only field), and a benign cosmetic difference
        // shouldn't block an install. These are the fields an
        // index→file swap would have to alter to be an actual attack:
        //   name / version — identity
        //   main           — variant→entrypoint map (the code that loads)
        //   dependencies   — the transitive surface
        // `type` is included too — cheap, and a ui_qml/core swap is
        // load-path-relevant.
        static const char* kFields[] = {
            "name", "version", "main", "dependencies", "type"};
        for (const char* f : kFields) {
            const json a = advertised.contains(f)   ? advertised.at(f)   : json();
            const json b = fileManifest.contains(f) ? fileManifest.at(f) : json();
            if (a != b) {
                errMsg = std::string("downloaded package field '") + f
                       + "' does not match the catalog entry";
                lgx_free_package(pkg);
                return false;
            }
        }
    }

    lgx_free_package(pkg);

    // ── 2c. Signer binding ───────────────────────────────────────────
    // When the index advertised a signer DID, the file must be signed
    // by the SAME DID. We don't re-verify the Ed25519 here
    // (package_manager does, against the trust keyring) — we only bind
    // index→file so a swap to a differently-signed (or unsigned)
    // package is caught even under the WARN signature policy.
    const json& advSig = objOrEmpty(indexEntry, "signature");
    if (!advSig.empty()) {
        const std::string advDid = advSig.value("did", "");
        if (!advDid.empty()) {
            lgx_signature_info_t info =
                lgx_verify_signature(lgxPath.c_str(), nullptr);
            const bool fileSigned = info.is_signed;
            const std::string fileDid =
                info.signer_did ? info.signer_did : "";
            lgx_free_signature_info(info);
            if (!fileSigned || fileDid != advDid) {
                errMsg = "downloaded package signer does not match the "
                         "catalog (expected " + advDid + ")";
                return false;
            }
        }
    }

    return true;
}

} // namespace

std::string PackageDownloaderLib::downloadPackage(const std::string& repoUrlOrName,
                                                  const std::string& packageName,
                                                  const std::string& version,
                                                  const std::string& rootHash,
                                                  const std::string& outputDir) {
    impl_->ensureMetadata();

    // Build the list of repos to consider.
    std::vector<Repository> candidates;
    if (repoUrlOrName.empty()) {
        for (const auto& r : impl_->registry.list()) {
            if (r.enabled && r.resolveError.empty()) candidates.push_back(r);
        }
    } else {
        auto r = impl_->registry.findByUrlOrName(repoUrlOrName);
        if (!r) return {};
        candidates.push_back(*r);
    }

    for (const auto& repo : candidates) {
        std::string body = impl_->fetchIndex(repo);
        if (body.empty()) continue;
        json idx;
        try { idx = json::parse(body); } catch (...) { continue; }
        if (!idx.is_object() || !idx.contains("packages") || !idx["packages"].is_array())
            continue;
        for (const auto& pkg : idx["packages"]) {
            if (!pkg.is_object() || pkg.value("name", "") != packageName) continue;
            const json* v = pickVersion(pkg, version, rootHash);
            if (!v || !v->is_object()) continue;
            std::string url = v->value("url", "");
            if (url.empty()) continue;
            // Derive destination path.
            std::string destDir = outputDir;
            if (destDir.empty()) {
                const char* tmp = std::getenv("TMPDIR");
                if (!tmp) tmp = "/tmp";
                destDir = tmp;
            }
            std::string filename = fs::path(url).filename().string();
            if (filename.empty()) filename = packageName + ".lgx";
            std::string dest = (fs::path(destDir) / filename).string();
            if (!impl_->fetcher->getToFile(url, dest)) return {};
            // Bind the downloaded artifact to what the index advertised
            // — manifest fields + signer DID. The .lgx `url` and the
            // `index.json` come from independent hosts; without this a
            // swapped file (downgrade attack, sibling package) would
            // sail through to install. Deep integrity + Ed25519 trust
            // stay package_manager's job at install time; this is only
            // the index→file binding. On mismatch we delete the bad
            // artifact and fail the download.
            {
                std::string verr;
                if (!verifyDownloadAgainstIndex(dest, *v, verr)) {
                    std::error_code rmEc;
                    fs::remove(dest, rmEc);
                    std::cerr << "package_downloader: rejected " << packageName
                              << " from " << url << " — " << verr << "\n";
                    return {};
                }
            }
            return dest;
        }
    }
    return {};
}

namespace {

// Parse one manifest dependency entry — accepts a JSON string or object.
struct ParsedDep {
    std::string name;
    std::optional<std::string> versionRange;
    std::optional<std::string> signer;
    // Optional source-repo scope. When set, findBest only considers
    // packages from this repository URL — so callers that already know
    // exactly which repo entry they want (the package_manager_ui's
    // per-row install / upgrade path, scoped to the row the user
    // clicked) can pin it. Manifest-declared transitive deps don't set
    // this; they fall through to the cross-repo "best" pick.
    std::optional<std::string> repositoryUrl;
};

bool parseDep(const json& j, ParsedDep& out, std::string& err) {
    if (j.is_string()) { out.name = j.get<std::string>(); return true; }
    if (!j.is_object()) { err = "dependency entry must be string or object"; return false; }
    if (!j.contains("name") || !j["name"].is_string()) {
        err = "dependency object missing 'name'"; return false;
    }
    out.name = j["name"].get<std::string>();
    if (j.contains("version") && j["version"].is_string())
        out.versionRange = j["version"].get<std::string>();
    if (j.contains("signer") && j["signer"].is_string())
        out.signer = j["signer"].get<std::string>();
    if (j.contains("repositoryUrl") && j["repositoryUrl"].is_string())
        out.repositoryUrl = j["repositoryUrl"].get<std::string>();
    return true;
}

} // namespace

std::string PackageDownloaderLib::resolveDependenciesJson(const std::string& dependenciesJson,
                                                          const std::string& installedPackagesJson) {
    impl_->ensureMetadata();
    json input;
    try { input = json::parse(dependenciesJson); }
    catch (...) { return R"([{"error":"invalid JSON input"}])"; }
    if (!input.is_array())
        return R"([{"error":"input must be a JSON array"}])";

    // Fetch full merged catalog once.
    std::string catBody = getCatalogJson();
    json cat;
    try { cat = json::parse(catBody); } catch (...) { cat = json::array(); }

    // Installed-packages index, name → version. Used to short-circuit
    // transitive deps whose range is already satisfied by what's on
    // disk. Empty / unparseable installedPackagesJson disables the
    // optimisation — the resolver then picks every transitive from the
    // catalog (pre-installed-aware behaviour).
    std::unordered_map<std::string, std::string> installedByName;
    if (!installedPackagesJson.empty()) {
        try {
            json inst = json::parse(installedPackagesJson);
            if (inst.is_array()) {
                for (const auto& e : inst) {
                    if (!e.is_object()) continue;
                    const std::string n = e.value("name", "");
                    const std::string v = e.value("version", "");
                    if (!n.empty() && !v.empty()) installedByName[n] = v;
                }
            }
        } catch (...) { /* silent — best effort */ }
    }

    json out = json::array();
    std::unordered_map<std::string, bool> seen; // name|version|hash

    // Lookup helper: find best candidate across repos.
    auto findBest = [&](const ParsedDep& dep, json& chosen, std::string& chosenRepo, std::string& errMsg) -> bool {
        // Hold the best candidate as an owned value, NOT a pointer.
        //
        // The previous `const json* bestVer = &v` aliased an element of
        // the per-package `versions` sequence. With the ternary below,
        // `versions` is a *temporary copy* whose lifetime ends with the
        // enclosing `for (pkg : cat)` iteration. When the same package
        // name appears in more than one repository (e.g. the official
        // repo plus a user fork both publishing `wallet_module`), the
        // best candidate is found in repo A, the loop advances to
        // repo B's same-named package, repo A's `versions` copy is
        // destroyed, and `*bestVer` afterwards dereferences freed
        // memory. The corrupted read surfaced downstream as
        // `json type_error.306 (value() with null)` on the next
        // `chosen.value(...)`. Copying the chosen version out on the
        // spot removes the alias entirely.
        bool haveBest = false;
        json bestVer;
        std::string bestRepo;
        std::string bestDate;
        // Stable empty array so the ternary binds a real lvalue ref
        // (no materialised temporary) when `versions` is absent/null.
        const json emptyArr = json::array();
        for (const auto& pkg : cat) {
            if (!pkg.is_object() || pkg.value("name", "") != dep.name) continue;
            // Repo scope. When the caller pinned a repositoryUrl (the
            // per-row install path in package_manager_ui does this with
            // the row's source repo), skip packages from other repos
            // — otherwise two repos publishing the same `name` would
            // tiebreak by releasedAt and the resolver could pick the
            // wrong one. Empty pin = no scope = pre-fix cross-repo
            // behaviour, matching manifest-declared transitive deps.
            if (dep.repositoryUrl && pkg.value("repositoryUrl", "") != *dep.repositoryUrl)
                continue;
            // `versions` may legally be an array, missing, or
            // (defensively) null — only the array case is iterable.
            const json& versions = (pkg.contains("versions") && pkg["versions"].is_array())
                                   ? pkg["versions"] : emptyArr;
            for (const auto& v : versions) {
                if (!v.is_object()) continue;
                std::string ver = objOrEmpty(v, "manifest").value("version", "");
                if (dep.versionRange && !semverRangeMatches(*dep.versionRange, ver)) continue;
                if (dep.signer) {
                    std::string sigDid;
                    if (v.contains("signature") && v["signature"].is_object())
                        sigDid = v["signature"].value("did", "");
                    if (sigDid != *dep.signer) continue;
                }
                std::string date = v.value("releasedAt", "");
                if (!haveBest || date > bestDate) {
                    haveBest = true;
                    bestVer = v;            // copy — outlives this iteration
                    bestRepo = pkg.value("repositoryUrl", "");
                    bestDate = date;
                }
            }
        }
        if (!haveBest) {
            std::ostringstream oss;
            oss << "no candidate matches '" << dep.name << "'";
            if (dep.versionRange) oss << " @ " << *dep.versionRange;
            if (dep.signer) oss << " (signer=" << *dep.signer << ")";
            if (dep.repositoryUrl) oss << " (repo=" << *dep.repositoryUrl << ")";
            errMsg = oss.str();
            return false;
        }
        chosen = std::move(bestVer);
        chosenRepo = bestRepo;
        return true;
    };

    // BFS so deps-of-deps are added before their consumers. Each queue
    // entry carries an isTopLevel flag — true for the caller's input
    // array, false for deps the resolver pulled in. Top-level entries
    // are NEVER short-circuited by the installed-state check (the user
    // explicitly picked them), and they get `topLevel: true` in the
    // output so consumers can split the resolved list into "the action
    // I asked for" vs "the transitive deps that come along".
    struct QueueEntry { ParsedDep dep; bool isTopLevel; };
    std::vector<QueueEntry> queue;
    for (const auto& el : input) {
        ParsedDep d; std::string err;
        if (!parseDep(el, d, err)) {
            json e; e["error"] = err; out.push_back(std::move(e));
            return out.dump();
        }
        queue.push_back({std::move(d), /*isTopLevel=*/true});
    }

    while (!queue.empty()) {
        QueueEntry qe = std::move(queue.front());
        queue.erase(queue.begin());
        const ParsedDep& dep = qe.dep;

        // Installed-state short-circuit (transitive only). If an
        // installed copy's version meets the dep's range, the dep is
        // satisfied as-is; skip emitting an entry and skip recursing
        // into the chosen manifest (we don't have one — by definition
        // we didn't pick from the catalog). Top-level inputs bypass
        // this: the caller asked for them explicitly, so they must
        // always resolve to a catalog pick.
        if (!qe.isTopLevel) {
            auto it = installedByName.find(dep.name);
            if (it != installedByName.end()) {
                const bool inRange = !dep.versionRange
                                  || semverRangeMatches(*dep.versionRange, it->second);
                if (inRange) continue;
            }
        }

        json chosen; std::string chosenRepo; std::string err;
        if (!findBest(dep, chosen, chosenRepo, err)) {
            json e; e["error"] = err; e["name"] = dep.name; out.push_back(std::move(e));
            return out.dump();
        }
        const json& chosenManifest = objOrEmpty(chosen, "manifest");
        std::string ver = chosenManifest.value("version", "");
        std::string hash = chosen.value("rootHash", "");
        std::string key = dep.name + "|" + ver + "|" + hash;
        if (seen[key]) continue;
        seen[key] = true;
        json entry;
        entry["name"] = dep.name;
        entry["version"] = ver;
        entry["rootHash"] = hash;
        entry["repositoryUrl"] = chosenRepo;
        entry["url"] = chosen.value("url", "");
        entry["topLevel"] = qe.isTopLevel;
        out.push_back(std::move(entry));
        // Enqueue transitive deps from the chosen version's manifest.
        if (chosenManifest.contains("dependencies") && chosenManifest["dependencies"].is_array()) {
            for (const auto& sub : chosenManifest["dependencies"]) {
                ParsedDep d; std::string serr;
                if (parseDep(sub, d, serr))
                    queue.push_back({std::move(d), /*isTopLevel=*/false});
            }
        }
    }
    // Reverse so deps appear before their consumers.
    std::reverse(out.begin(), out.end());
    return out.dump();
}

bool PackageDownloaderLib::semverMatches(const std::string& range, const std::string& version) {
    return semverRangeMatches(range, version);
}

} // namespace lgpd
