/**
 * @file Store.hpp
 * @brief tau-store: manifest hashing, store-run lifecycle, and GC.
 *
 * The store is content-addressed: a manifest's store directory is
 * determined by a BLAKE2b hash of its content *after* injecting
 * exact git commit SHAs and local file timestamps.
 *
 * Hash inputs
 * ───────────
 * 1. The full manifest YAML text.
 * 2. For each `git:` directive: the resolved commit SHA is injected
 *    as a comment before the directive before hashing.
 * 3. For each `local:` directive: the file's mtime (nanoseconds) is
 *    injected before hashing.
 *
 * Store run isolation
 * ───────────────────
 * When tau-store executes a manifest:
 *  - The store subdir is the new root (chroot).
 *  - All network interfaces are removed (empty network namespace).
 *  - Only execute permission is granted; no writes outside the store dir.
 *  - Failure during a run does NOT delete the store entry — the next
 *    `tau-store` run may succeed.
 *
 * Garbage collection
 * ──────────────────
 * After all manifests are processed, any store/<hash>/ directory that
 * is not referenced by any current manifest is removed.
 */

#pragma once

#include "Manifest.hpp"
#include <Collection/String.hpp>
#include <Collection/Array.hpp>
#include <Collection/Map.hpp>
#include <Security/Crypto.hpp>

namespace Tau {

using namespace Collection;
using namespace Xi;

// ─── Store entry ──────────────────────────────────────────────────────────────
struct StoreEntry {
    String hash;           ///< Hex-encoded BLAKE2b-32 hash
    String storeDir;       ///< Absolute path: TAU_PATH/store/<hash>/
    String manifestPath;   ///< Original manifest path
    bool   ran = false;    ///< Whether the store run succeeded
};

// ─── Store ────────────────────────────────────────────────────────────────────
class Store {
public:
    /**
     * @brief Compute the store hash for a manifest.
     *
     * Reads the manifest file, resolves git commits and local timestamps,
     * injects them as comments, then hashes with BLAKE2b-32.
     *
     * @param manifestPath  Absolute path to the manifest YAML.
     * @return Hex-encoded hash string (64 chars), or "" on error.
     */
    static String computeHash(const String &manifestPath);

    /**
     * @brief Check whether a manifest has already been run in the store.
     *
     * @param hash  Output of computeHash().
     * @return true if TAU_PATH/store/<hash>/ exists and contains a
     *         sentinel file "ran" with content "1".
     */
    static bool isRan(const String &hash);

    /**
     * @brief Mark a store entry as successfully run.
     */
    static void markRan(const String &hash);

    /**
     * @brief Run a manifest inside the store sandbox.
     *
     * Called by tau-store for each unrun manifest.
     *
     * Steps:
     *  1. Create TAU_PATH/store/<hash>/ (if not present).
     *  2. Fork a child that:
     *     a. Enters user + mount + net namespaces.
     *     b. Chroots into the store dir.
     *     c. Drops capabilities.
     *     d. Runs the manifest's directives.
     *  3. On child exit 0, calls markRan().
     *  4. On non-zero exit, logs the failure and continues (no GC).
     *
     * @param entry  The store entry to process.
     * @return true if the run succeeded.
     */
    static bool runManifest(const StoreEntry &entry);

    /**
     * @brief Remove store directories not referenced by any manifest.
     *
     * @param activeHashes  Set of hashes that should be kept.
     */
    static void garbageCollect(const Array<String> &activeHashes);

    /**
     * @brief Main entry point for `tau-store`.
     *
     * Scans TAU_PATH/manifests/, computes hashes, runs unrun manifests,
     * then garbage-collects unreferenced store entries.
     *
     * @return 0 on full success, 1 if any manifest failed.
     */
    static int run();

private:
    /**
     * @brief Inject git commit SHAs into a manifest YAML string.
     *
     * For each `git:` entry, resolves the commit SHA via the GitHub API
     * (using the cache) and injects it as a YAML comment directly above
     * the directive line.
     */
    static String injectCommits(const String &yaml,
                                const String &manifestDir);

    /**
     * @brief Inject local file timestamps into a manifest YAML string.
     *
     * For each `local:` entry, appends the source file's mtime as a comment.
     */
    static String injectTimestamps(const String &yaml,
                                   const String &manifestDir);

    static String hashContent(const String &content);
};

} // namespace Tau
