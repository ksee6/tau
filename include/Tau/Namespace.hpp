/**
 * @file Namespace.hpp
 * @brief Linux namespace and user-namespace helpers for tau isolation.
 *
 * All container isolation in tau uses UNPRIVILEGED user namespaces
 * (no SUID required for the container itself; tau-store is SUID only
 * for writing to /var/tau/store).
 *
 * Isolation layers (applied in order as directives are encountered):
 *  1. User namespace  — always created; maps UID 0 inside → real UID outside
 *  2. Mount namespace — created together with user ns; allows private mounts
 *  3. PID namespace   — optional, triggered by `newpid: true`
 *  4. Net namespace   — optional, triggered by `newnet:`
 *
 * For store runs (via tau-store, SUID root), we additionally drop all
 * capabilities after setup so the manifest cannot escape.
 */

#pragma once

#include <Xi/String.hpp>
#include <Xi/Array.hpp>

#include <sched.h>
#include <sys/types.h>

namespace Tau {

using namespace Xi;

struct DForward;


// ─── Namespace isolation context ──────────────────────────────────────────────

/**
 * @struct IsolationContext
 * @brief Tracks which namespaces have been entered, for orderly cleanup
 *        and for informational reporting.
 */
struct IsolationContext {
    bool      hasUserNS       = false;
    bool      hasMountNS      = false;
    bool      hasPidNS        = false;
    bool      hasNetNS        = false;
    bool      hasChrootd      = false;
    bool      isIsolated      = false;

    String    chrootPath;     ///< Effective chroot path (empty = host root)

    bool      hasMemory       = false;
    size_t    memoryBytes     = 0;
    size_t    memoryBytesMax  = 0;

    bool      hasCPU          = false;
    long long cpuQuota        = -1;
    long long cpuQuotaMax     = -1;

    bool      hasCPUSet       = false;
    String    cpusetCPUs;

    bool      hasAutofreeze   = false;
    bool      autofreezeWOL   = true;
    double    autofreezeCPU   = 5.0;
    double    autofreezeTimer = 20.0;

    bool      hasCgroupLimits = false;
};

// ─── Namespace helpers ────────────────────────────────────────────────────────

class Namespace {
public:
    /**
     * @brief Enter full container isolation (user + mount + PID namespaces).
     *
     * Workloads run with UID 0 (root), full container capabilities, and PID 1.
     */
    static bool isolate(IsolationContext &ctx);

    /**
     * @brief Setup full Linux capabilities inside the user namespace container.
     */
    static bool setupContainerCaps();

    /**
     * @brief Enter a new user + mount namespace.
     *
     * Writes uid_map and gid_map so that UID 0 inside maps to the calling
     * process's real UID outside. Idempotent — does nothing if already in
     * a user namespace.
     *
     * @param ctx  Updated on success.
     * @return true on success.
     */
    static bool enterUser(IsolationContext &ctx);

    /**
     * @brief Enter a new PID namespace (requires user ns first).
     *
     * The calling process becomes PID 1 in the new namespace after the
     * next fork(). The child (not the caller) runs as PID 1.
     *
     * @return true on success.
     */
    static bool enterPID(IsolationContext &ctx);


    /**
     * @brief Enter a new network namespace.
     *
     * @param allowList  Interface names to move into the new ns (e.g. veth).
     *                   The loopback interface is always brought up.
     * @return true on success.
     */
    static bool enterNet(const Array<String> &allowList,
                         IsolationContext    &ctx);

    /**
     * @brief Perform a chroot into the given path.
     *
     * Also sets up minimal /proc, /tmp, and /dev stubs inside the rootfs
     * if they do not already exist.
     *
     * @param path  Absolute host path to the new root.
     * @param ctx   Updated on success.
     * @return true on success.
     */
    static bool doChroot(const String &path, IsolationContext &ctx);

    /**
     * @brief Initialize host namespace helper before unsharing user namespace.
     */
    /**
     * @brief Initialize host namespace helper before unsharing user namespace.
     */
    static void initHostHelper();
    static bool linkExists(const String &iface);
    static bool hostCreateVeth(const String &hostName, const String &peerName);
    static bool hostMoveNetns(const String &iface, pid_t targetPid);
    static bool hostRunIP(const String &cmd);
    static void hostCleanupLink(const String &iface);
    static bool hostCreateBridge(const String &bridgeName, const String &address);
    static bool hostJoinBridge(const String &bridgeName, const String &target);
    static bool hostUnjoinBridge(const String &target);
    static bool hostDeleteBridgeIfEmpty(const String &bridgeName, const Array<String> &instanceLinks);
    static void shutdownHostHelper();

    /**
     * @brief Create a virtual ethernet pair on the host.
     *
     * Equivalent to: `ip link add <hostName> type veth peer name <peerName>`
     *
     * @param hostName  Name of the host-side interface.
     * @param peerName  Name of the container-side interface.
     * @return true on success.
     */
    static bool createVeth(const String &hostName, const String &peerName);

    /**
     * @brief Create a macvlan interface.
     */
    static bool createMacvlan(const String &parent, const String &name, const String &mode, const String &mac);

    /**
     * @brief Create an ipvlan interface.
     */
    static bool createIPVlan(const String &parent, const String &name, const String &mode);

    /**
     * @brief Create a bridge interface and optionally attach a link.
     */
    static bool createBridge(const String &name, const String &attach, const String &address);

    /**
     * @brief Configure port forwarding and firewall rules inside namespace.
     */
    static bool setupForwarding(const DForward &d);


    /**
     * @brief Perform a bind or overlay mount.
     *
     * If workDir is empty → bind mount.
     * If workDir is non-empty → overlay mount (source=lower, workDir=work,
     * target=upper/merged).
     *
     * @param source  Lower dir (overlay) or source (bind).
     * @param workDir Work dir (overlay only; empty → bind).
     * @param target  Mount point.
     * @param read    Allow read access.
     * @param write   Allow write access.
     * @return true on success.
     */
    static bool doMount(const String &source,
                        const String &workDir,
                        const String &target,
                        bool read = true,
                        bool write = true,
                        const String &upperDir = "");

    /**
     * @brief Write uid_map and gid_map for the current user namespace.
     *
     * Maps inside-UID 0 → outside-UID <realUID>.
     * Must be called after unshare(CLONE_NEWUSER).
     *
     * @param realUID  The caller's real UID (from getuid()).
     * @param realGID  The caller's real GID (from getgid()).
     * @return true on success.
     */
    static bool writeIDMaps(uid_t realUID, gid_t realGID);

    /**
     * @brief Drop all capabilities (for store sandbox).
     *
     * Called inside tau-store's child process after chroot so the manifest
     * script cannot use elevated privileges.
     */
    static bool dropCapabilities();

    /**
     * @brief Ensure minimal /proc /tmp /dev exist inside chrootPath.
     */
    static bool ensureRootfsStubs(const String &chrootPath);

    /**
     * @brief Unmount /dev and device nodes inside chrootPath.
     */
    static bool cleanupRootfsStubs(const String &chrootPath);

    /**
     * @brief Mount a fresh procfs at <chrootPath>/proc.
     */
    static bool mountProc(const String &chrootPath);
};

} // namespace Tau
