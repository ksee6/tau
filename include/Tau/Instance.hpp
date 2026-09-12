/**
 * @file Instance.hpp
 * @brief Runtime instance state — persisted in instances/<name>/instance.yml.
 *
 * An instance is a running (or stopped) `tau run` invocation.
 * tau-spawn owns the instance file; it writes updates atomically
 * (write to .tmp then rename).
 *
 * External tools (tau ls, tau attach) read the file without holding a lock —
 * the atomic rename ensures they never see a partial write.
 *
 * Instance YAML schema example:
 * ```yaml
 * name: myapp
 * pid: 12345          # PID of tau-spawn itself
 * status: running     # running | stopped | failed
 * manifest: /abs/path/to/manifest.yml
 * spawns:
 *   - name: 0
 *     pid: 12346
 *     status: running
 *     command: ./server
 *     pty: true
 *   - name: worker
 *     pid: 12347
 *     status: exited
 *     exit_code: 0
 *     command: ./worker
 *     pty: false
 * ```
 */

#pragma once

#include <Xi/String.hpp>
#include <Xi/Array.hpp>
#include <Xi/Map.hpp>

namespace Tau {

using namespace Xi;

// ─── Spawn state ──────────────────────────────────────────────────────────────
enum class SpawnStatus {
    Running,
    Exited,
    Failed,
    Stopped,
    Frozen,
};

struct SpawnState {
    String      name;
    pid_t       pid        = -1;
    SpawnStatus status     = SpawnStatus::Stopped;
    int         exitCode   = 0;
    String      command;
    bool        hasPTY     = false;

    String statusString() const;
};

// ─── Instance state ───────────────────────────────────────────────────────────
enum class InstanceStatus {
    Running,
    Stopped,
    Failed,
    Frozen,
};

struct InstanceState {
    String         name;
    pid_t          spawnPID   = -1;  ///< PID of the tau-spawn process
    InstanceStatus status     = InstanceStatus::Stopped;
    String         manifestPath;
    Array<SpawnState> spawns;

    String statusString() const;

    /** @brief Find a spawn by name. Returns nullptr if not found. */
    SpawnState *findSpawn(const String &spawnName);
    const SpawnState *findSpawn(const String &spawnName) const;

    /** @brief First spawn (for default :spawnname resolution). */
    SpawnState *firstSpawn();
};

// ─── Serialisation ────────────────────────────────────────────────────────────
class Instance {
public:
    /**
     * @brief Load instance state from a directory.
     * @param instanceDir  Path like TAU_PATH/instances/<name>.
     * @return true if loaded successfully.
     */
    static bool load(const String &instanceDir, InstanceState &out);

    /**
     * @brief Persist instance state atomically.
     *
     * Writes to instance.yml.tmp then renames to instance.yml.
     */
    static bool save(const String &instanceDir, const InstanceState &state);

    /**
     * @brief Serialise state to a YAML string.
     */
    static String toYAML(const InstanceState &state);

    /**
     * @brief Parse instance YAML back into an InstanceState.
     */
    static bool fromYAML(const String &yaml, InstanceState &out);

    /**
     * @brief List all instance names found in the instances directory.
     */
    static Array<String> listAll(const String &instancesDir);

    /**
     * @brief Resolve "name:spawnname" syntax.
     *
     * If spawnname is empty, uses the first spawn in the instance.
     * Returns false if the instance or spawn is not found.
     */
    static bool resolve(const String &spec,
                        const String &instancesDir,
                        String &outInstance,
                        String &outSpawn);
};

} // namespace Tau
