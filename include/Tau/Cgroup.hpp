/**
 * @file Cgroup.hpp
 * @brief cgroup v2 resource-limit helpers for tau spawns.
 *
 * tau places each instance under a cgroup hierarchy:
 *   /sys/fs/cgroup/tau/<instanceName>/<spawnName>/
 *
 * Three limits are supported (others are unlimited by default):
 *   - memory.max    (from `memory:` directive)
 *   - cpu.max       (from `cpu:` directive)   format: "quota period"
 *   - cpuset.cpus   (from `cpuset:` directive)
 *
 * The cgroup is created before fork() so that the child can move itself
 * into it with a single write to cgroup.procs.
 *
 * If cgroup v2 is not available (e.g. running in a container that does not
 * expose it), all operations silently succeed — resource limits are best-effort.
 */

#pragma once

#include <Collection/String.hpp>

#include <sys/types.h>

namespace Tau {

using namespace Collection;
using namespace Xi;

class Cgroup {
public:
    /**
     * @brief Check whether cgroup v2 is mounted at /sys/fs/cgroup.
     */
    static bool available();

    /**
     * @brief Create the cgroup directory for an instance/spawn pair.
     *
     * Path: /sys/fs/cgroup/tau/<instanceName>/<spawnName>
     * Creates parent directories as needed.
     *
     * @return true on success (or if cgroup v2 is unavailable).
     */
    static bool create(const String &instanceName, const String &spawnName = "");

    /**
     * @brief Apply memory limits (baseline allocation and maximum limit).
     *
     * @param instanceName   Instance identifier.
     * @param spawnName      Spawn identifier (optional; empty = shared instance cgroup).
     * @param bytes          Baseline allocation / soft limit (0 = none).
     * @param bytesMax       Maximum bytes (0 = use baseline, or "max" if both 0).
     * @return true on success.
     */
    static bool setMemory(const String &instanceName,
                          const String &spawnName = "",
                          size_t        bytes = 0,
                          size_t        bytesMax = 0);

    /**
     * @brief Apply CPU quota and weight (baseline allocation and maximum quota).
     *
     * @param instanceName  Instance identifier.
     * @param spawnName     Spawn identifier (optional; empty = shared instance cgroup).
     * @param quota         Baseline allocation (100 = 1 full core; -1 = default weight).
     * @param quotaMax      Maximum quota (-1 = use baseline, or "max" if both -1).
     * @return true on success.
     */
    static bool setCPU(const String &instanceName,
                       const String &spawnName = "",
                       long long     quota = -1,
                       long long     quotaMax = -1);

    /**
     * @brief Freeze or unfreeze all processes in this cgroup.
     *
     * Writes "1" (freeze) or "0" (unfreeze) to cgroup.freeze.
     *
     * @return true on success.
     */
    static bool freeze(const String &instanceName,
                       const String &spawnName = "",
                       bool          frozen = true);

    /**
     * @brief Apply a cpuset restriction.
     *
     * @param instanceName  Instance identifier.
     * @param spawnName     Spawn identifier (optional; empty = shared instance cgroup).
     * @param cpus          cpuset string, e.g. "0,1,4" or "0-3".
     * @return true on success.
     */
    static bool setCPUSet(const String &instanceName,
                          const String &spawnName = "",
                          const String &cpus = "");

    /**
     * @brief Move a process into this cgroup.
     *
     * Writes <pid> to cgroup.procs.
     *
     * @return true on success.
     */
    static bool addPID(const String &instanceName,
                       const String &spawnName,
                       pid_t         pid);

    /**
     * @brief Remove the cgroup directory (after all processes have left).
     *
     * Writes "" to cgroup.kill if available, then rmdir's the directory.
     * Silently ignores ENOENT.
     */
    static void remove(const String &instanceName, const String &spawnName = "");

    /**
     * @brief Remove the instance cgroup directory (after all spawns removed).
     */
    static void removeInstance(const String &instanceName);

private:
    static String cgroupPath(const String &instanceName,
                             const String &spawnName = "");

    static bool writeFile(const String &path, const String &value);
};

} // namespace Tau
