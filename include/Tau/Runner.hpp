/**
 * @file Runner.hpp
 * @brief Directive executor — walks a DirectiveList and carries out each step.
 *
 * The Runner is the heart of tau.  It is used in two contexts:
 *
 *  1. **tau-spawn** — executes a full instance manifest, managing spawns,
 *     isolation, mounts, and networking.
 *
 *  2. **tau-store** — executes a manifest inside the store sandbox
 *     (no networking, chroot into the store subdir).
 *
 * Execution model
 * ───────────────
 * - Directives run sequentially within a block.
 * - Each directive returns a boolean result (success / failure).
 * - `and:` propagates the first failure and stops.
 * - `or:` propagates the first success and stops.
 * - `nand:` / `nor:` / `xor:` invert as expected.
 * - `throw:` throws a TauThrow exception that unwinds to the top-level.
 * - Resource isolation directives (`memory:`, `cpu:`, `chroot:`, …)
 *   are applied to the current IsolationContext and affect all subsequent
 *   directives and spawns in the same block (and nested blocks).
 *
 * Variables
 * ─────────
 * - All env vars at startup seed the variable map.
 * - `var:` directives check or assign variables in the current scope.
 * - Scopes are stacked: a nested block inherits its parent's variables
 *   but writes are local to that block (shadowing).
 * - Spawns inherit the variable map at the time they are launched
 *   (converted to environment variables).
 */

#pragma once

#include "Directives.hpp"
#include "Instance.hpp"
#include "Namespace.hpp"
#include "IPC.hpp"

#include <Collection/String.hpp>
#include <Collection/Array.hpp>
#include <Collection/Map.hpp>
#include <Xi/Func.hpp>

#include <sys/types.h>

namespace Tau {

using namespace Collection;
using namespace Xi;

// ─── Exception for throw: directive ──────────────────────────────────────────
struct TauThrow {
    String message;
};

// ─── Spawn tracking (used inside Runner) ──────────────────────────────────────

struct ActiveSpawn {
    String      name;
    String      command;
    pid_t       pid       = -1;
    int    ptyMaster = -1;   ///< -1 if not a PTY spawn
    bool   hasPTY    = false;
    bool   waited    = false;
    int    exitCode  = 0;

    // Rings for scrollback
    RingBuffer outRing;
    RingBuffer errRing;
};

struct RegisteredBin {
    String name;
    String description;
    String manifestPath;
    int    depth = 0;
};

struct ImageSync {
    String upperDir;
    String mergedDir;
    String imgDir;
};

// ─── Runner options ────────────────────────────────────────────────────────────
struct RunnerOptions {
    String instanceName;       ///< Used for cgroup paths and instance state
    String instanceDir;        ///< Full path to instances/<name>/
    String manifestPath;       ///< Current manifest path
    String sourceManifestPath; ///< Original source manifest path before copying

    bool storeMode  = false;   ///< true when running inside tau-store

    bool attachStdin = true;   ///< Attach real stdin to first `wait: true` spawn
    bool detach      = false;  ///< true when running as background daemon supervisor
    String detachKey = "ctrl+b"; ///< Key sequence to detach (e.g. "ctrl+b")
    int  cols        = 0;      ///< Terminal columns (0 = auto/default)
    int  rows        = 0;      ///< Terminal rows (0 = auto/default)

    /// Callback invoked whenever a spawn is started/updated/exited
    Func<void(const SpawnState &)> onSpawnChange;
    /// Callback invoked periodically while waiting for a spawn (e.g. to pump IPC)
    Func<void()>                   onPoll;
    /// Callback checked each waitSpawn iteration — return true to abort waiting
    Func<bool()>                   shouldStop;
    /// Callback invoked when output is produced by a spawn (e.g. to broadcast to attached clients)
    Func<void(const String &spawnName, const char *data, size_t len)> onOutput;
    /// Callback invoked when an IP event occurs
    Func<void(const String &iface, const String &ip)> onIPEvent;
    /// Callback invoked when an IP add/del action occurs
    Func<void(const String &iface, const String &ip, const String &action)> onIPActionEvent;
    /// Callback invoked when an interface state (up/down/created) occurs
    Func<void(const String &iface, const String &state)> onIfaceEvent;
};


// ─── Runner ───────────────────────────────────────────────────────────────────
class Runner {
public:
    explicit Runner(const RunnerOptions &opts);
    ~Runner();

    /**
     * @brief Execute a directive list.
     *
     * This is the main entry point.  Returns true if the entire list
     * evaluates to true (i.e. no `and:` short-circuit failure).
     *
     * May throw TauThrow if a `throw:` directive is encountered.
     */
    bool run(const DirectiveList &directives);

    /**
     * @brief Wait for all active spawns (called after run() returns).
     *
     * Collects exit codes; updates instance state.
     */
    void waitAll();

    /** @brief Send a signal to a named active spawn. */
    bool signalSpawn(const String &name, int sig);

    /** @brief Resize the PTY of a named active spawn (or all spawns). */
    bool resizeSpawn(const String &name, int cols, int rows);


    /** @brief Format a user-friendly descriptive summary of a directive. */
    static String directiveSummary(const Directive *d);

    /** @brief Returns all currently active spawns. */
    const Array<ActiveSpawn *> &activeSpawns() const { return _spawns; }

    /** @brief Current variable environment. */
    const Map<String, String> &vars() const { return _vars; }
    void setVars(const Map<String, String> &v) { _vars = v; }
    Map<String, String> currentEnv() const; ///< vars → environment

private:

    RunnerOptions         _opts;
    IsolationContext      _isoCtx;
    Map<String, String>   _vars;          ///< Current variable scope
    Array<ActiveSpawn *>  _spawns;        ///< All spawns launched so far
    Array<String>         _mounts;        ///< Tracked mount targets to unmount on exit
    Array<String>         _tempDirs;      ///< Temporary directories to clean up on exit
    Array<String>         _imagePaths;    ///< Tracked image paths to terminate fuse2fs daemons on exit
    Array<String>         _createdVethHosts; ///< Tracked host veth interfaces to delete on exit
    Array<String>         _createdBridges;   ///< Bridges created by this instance
    Array<String>         _joinedBridgeTargets; ///< Targets joined to bridges by this instance
    Array<String>         _createdLoopDevs;  ///< Tracked loop devices to detach on exit
    Array<ImageSync>      _imageSyncs;    ///< Tracked upper/img sync pairs for persistent image overlay
    String                _activeImgDir;  ///< Active image mount directory for @/ expansion
    int                   _nextSpawnId = 0; ///< Auto-numbering for unnamed spawns
    int                   _spawnCount  = 0; ///< Total spawns for name uniqueness
    Array<RegisteredBin>  _bins;          ///< Registered binary definitions (with priority)
    Array<String>         _activeBinDirs; ///< Active bindir paths for PATH modification
    int                   _manifestDepth = 0; ///< Current sub-manifest nesting level

    // ─── Directive handlers (each returns the directive's boolean result) ──────

    bool execWait      (const DWait      &d);
    bool execWaitExit  (const DWaitExit  &d);
    bool execRetry     (const DRetry     &d);
    bool execSpawn     (const DSpawn     &d);
    bool execDotenv    (const DDotenv    &d);
    bool execMemory    (const DMemory    &d);
    bool execCPUSet    (const DCPUSet    &d);
    bool execCPU       (const DCPU       &d);
    bool execChroot    (const DChroot    &d);
    bool execIsolate   (const DIsolate   &d);
    bool execVeth      (const DVeth      &d);

    bool execIP        (const DIP        &d);
    bool execNewNet    (const DNewNet    &d);
    bool execForward   (const DForward   &d);
    bool execMacvlan   (const DMacvlan   &d);
    bool execIPVlan    (const DIPVlan    &d);
    bool execBridge    (const DBridge    &d);
    bool execMount     (const DMount     &d);

    bool execCopy      (const DCopy      &d);
    bool execUnlink    (const DUnlink    &d);
    bool execMkdir     (const DMkdir     &d);
    bool execSymlink   (const DSymlink   &d);
    bool execImage     (const DImage     &d);
    bool execDocker    (const DDocker    &d);
    bool execVM        (const DVM        &d);
    bool execLocal     (const DLocal     &d);
    bool execGit       (const DGit       &d);
    bool execGHRelease (const DGHRelease &d);
    bool execManifest  (const DManifest  &d);
    bool execOr        (const DOr        &d);
    bool execAnd       (const DAnd       &d);
    bool execNand      (const DNand      &d);
    bool execNor       (const DNor       &d);
    bool execXor       (const DXor       &d);
    bool execVar       (const DVarBase   &d);
    bool execNewVar    (const DNewVar    &d);
    bool execRmVar     (const DRmVar     &d);
    bool execFail      (const DFail      &d);

    bool execThrow     (const DThrow     &d);
    bool execBin       (const DBin       &d);
    bool execBinDir    (const DBinDir    &d);

    // ─── Helpers ─────────────────────────────────────────────────────────────

    /// Resolve a path relative to the manifest's directory,
    /// respecting any active chroot.
    String resolvePath(const String &p) const;

    /// Substitute $Var references using the current variable map.
    String interp(const String &s) const;

    /// Launch a child process as a spawn directive.
    ActiveSpawn *launchSpawn(const String &spawnName, const DSpawn &d, bool allocPTY);

    /// Unique spawn name: use given name, or next integer if empty.
    String allocSpawnName(const String &hint);

    /// Collect exit status of a single active spawn (non-blocking).
    bool reapSpawn(ActiveSpawn *sp);

    /// Emit a SpawnState for the instance update callback.
    void notifySpawnChange(const ActiveSpawn &sp);

    /// Block until the spawn exits; poll ring buffers while waiting.
    void waitSpawn(ActiveSpawn *sp);

    /// Copy-recursive (used by copy:)
    bool copyRecursive(const String &src, const String &dst);

    /// Run a shell command synchronously, return true on exit 0.
    bool runShell(const String &cmd);
};

} // namespace Tau

