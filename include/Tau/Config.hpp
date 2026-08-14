#pragma once

#include <Collection/String.hpp>
#include <Collection/Array.hpp>

namespace Tau {

using namespace Collection;
using namespace Xi;

// ─── Version ──────────────────────────────────────────────────────────────────
static constexpr const char *TAU_VERSION = "0.1.0";

// ─── Environment variable names ───────────────────────────────────────────────
static constexpr const char *ENV_TAU_PATH      = "TAU_PATH";
static constexpr const char *ENV_TAU_PATH_TEMP = "TAU_PATH_TEMP";
static constexpr const char *ENV_TAU_GLOBAL    = "TAU_GLOBAL";

// ─── Default paths ────────────────────────────────────────────────────────────
static constexpr const char *DEFAULT_TAU_GLOBAL = "/var/tau";
static constexpr const char *DEFAULT_TAU_PATH   = "~/.cache/tau";

// ─── Ring-buffer size for spawn scrollback ────────────────────────────────────
static constexpr size_t RING_BUFFER_SIZE = 1u * 1024u * 1024u; // 1 MB per stream

// ─── GitHub cache timing ─────────────────────────────────────────────────────
static constexpr int GITHUB_CACHE_MIN_SECS = 5  * 60;  // 5 minutes
static constexpr int GITHUB_CACHE_MAX_SECS = 30 * 60;  // 30 minutes

// ─── Default stop timeout ─────────────────────────────────────────────────────
static constexpr int DEFAULT_STOP_TIMEOUT_SECS = 20;

// ─── Config singleton ─────────────────────────────────────────────────────────

/**
 * @class Config
 * @brief Resolved tau path configuration.
 *
 * Call Config::init() once at startup. After that, all accessors are
 * const and safe to call from any code path.
 */
class Config {
public:
    /**
     * @brief Initialise from environment variables and defaults.
     */
    static void init();

    /** @brief Primary tau root (~/.cache/tau for user, TAU_GLOBAL for root). */
    static const String &tauPath();

    /** @brief Temporary tau root (/run/user/<user>/tau for user, /run/tau for root). */
    static const String &tauPathTemp();

    /** @brief Global tau root (/var/tau). */
    static const String &tauGlobal();

    /** @brief The store subdirectory (under TAU_PATH). */
    static String storePath();

    /** @brief The manifests subdirectory (under TAU_PATH). */
    static String manifestsPath();

    /** @brief The instances subdirectory (under TAU_PATH_TEMP). */
    static String instancesPath();

    /** @brief The monitors subdirectory (under TAU_PATH_TEMP). */
    static String monitorsPath();

    /** @brief The GitHub cache subdirectory (under TAU_PATH). */
    static String githubCachePath();

    /** @brief Path for a specific instance directory (under TAU_PATH_TEMP/instances). */
    static String instanceDir(const String &name);

    /** @brief Path for the global tau.yml. */
    static String globalManifestPath();

    /**
     * @brief Check if TAU_GLOBAL/manifests is writable AND TAU_GLOBAL/tau-store is executable.
     * If so, returns TAU_GLOBAL/manifests; otherwise returns manifestsPath().
     */
    static String targetManifestsDir();

    /**
     * @brief Ensure required directory structure exists.
     */
    static void ensureLayout();

private:
    static String _tauPath;
    static String _tauPathTemp;
    static String _tauGlobal;
    static bool   _initialised;

    static String expandHome(const String &path);
};

} // namespace Tau

