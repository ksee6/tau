/**
 * @file GitHub.hpp
 * @brief GitHub API integration: repo info, release artifacts, branch/tag
 *        resolution with version operators, and disk-based cache.
 *
 * Cache strategy
 * ──────────────
 * Responses are stored in TAU_PATH/githubCache/<hash(url)>.json.
 * Each cached file has a "expires_at" epoch-seconds field.
 * Per-manifest wait times are randomised [5min, 30min] so concurrent
 * tau-store runs do not all hit GitHub at the same time.
 *
 * Rate limits
 * ───────────
 * If a GITHUB_TOKEN env var is set it is used as a Bearer token,
 * raising the API limit from 60 → 5000 req/hr.
 *
 * Branch / tag matching operators
 * ────────────────────────────────
 *   "main"          — exact name
 *   ">v1.0.0"       — latest tag greater than v1.0.0 (semver)
 *   ">=v1.0.0"      — latest tag ≥ v1.0.0
 *   "<v2.0.0"       — latest tag less than v2.0.0
 *   "<=v2.0.0"      — latest tag ≤ v2.0.0
 *   "reg abc.*"     — first tag matching regex
 */

#pragma once

#include <Xi/String.hpp>
#include <Xi/Array.hpp>
#include <Xi/Map.hpp>

namespace Tau {

using namespace Xi;

// ─── Parsed GitHub coordinates ────────────────────────────────────────────────
struct GHRepo {
    String owner;
    String repo;
    String ref;        ///< branch / tag / commit / operator expression

    /**
     * @brief Parse "user/repo#branch" or "user/repo" from a string.
     * @return true on success.
     */
    static bool parse(const String &spec, GHRepo &out);
};

// ─── Release artifact ─────────────────────────────────────────────────────────
struct GHAsset {
    String name;
    String downloadUrl;
    long long size = 0;
};

struct GHRelease {
    String tag;
    String commitSHA;
    Array<GHAsset> assets;
};

// ─── GitHub API client ────────────────────────────────────────────────────────
class GitHub {
public:
    /**
     * @brief Resolve a branch/tag spec to an exact commit SHA.
     *
     * Supports the >, >=, <, <=, reg operators.
     * Uses disk cache; fetches fresh data if expired.
     *
     * @param repo   Parsed repo coordinates.
     * @return Resolved commit SHA, or "" on failure.
     */
    static String resolveCommit(const GHRepo &repo);

    /**
     * @brief Resolve a tag spec to a GHRelease.
     *
     * Used by the `ghrelease:` directive.
     *
     * @param repo   Parsed repo coordinates (repo.ref = tag spec).
     * @return Matched release, or GHRelease with empty tag on failure.
     */
    static GHRelease resolveRelease(const GHRepo &repo);

    /**
     * @brief List all tags for a repo (cached).
     *
     * @return Array of tag strings, most recent first.
     */
    static Array<String> listTags(const GHRepo &repo);

    /**
     * @brief Clone or update a git repository.
     *
     * Uses the system `git` binary.
     *
     * @param url     Clone URL.
     * @param commit  Exact commit SHA to check out.
     * @param target  Local directory to clone into.
     * @param store   If true, runs as a store manifest (network disabled after
     *                clone). If false, clones directly.
     * @return true on success.
     */
    static bool cloneOrUpdate(const String &url,
                              const String &commit,
                              const String &target,
                              bool          store);

    /**
     * @brief Download a release asset to a local path.
     *
     * @param asset   The asset to download.
     * @param target  Destination directory.
     * @return true on success.
     */
    static bool downloadAsset(const GHAsset &asset, const String &target);

    /**
     * @brief Shorthand: parse "user/repo#branch" and clone/update.
     *
     * Used by `tau github user/repo#branch`.
     */
    static bool fetchRepo(const String &spec,
                          const String &target,
                          bool          global);

private:
    /** @brief HTTP GET with GitHub auth header, returns JSON body. */
    static String apiGet(const String &path);

    /** @brief Load cached response for url; empty string if expired/missing. */
    static String loadCache(const String &url);

    /** @brief Save response to cache with an expiry timestamp. */
    static void saveCache(const String &url, const String &body);

    /**
     * @brief Compare two semver strings.
     * @return -1 / 0 / +1 (a < b / a == b / a > b).
     */
    static int  semverCompare(const String &a, const String &b);

    /**
     * @brief Select a tag from a list according to the operator expression.
     *
     * Expression examples: ">v1.0.0", "<=v2.3", "reg beta.*"
     */
    static String selectTag(const Array<String> &tags, const String &expr);

    static String cacheKey(const String &url);
};

} // namespace Tau
