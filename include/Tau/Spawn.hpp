/**
 * @file Spawn.hpp
 * @brief tau-spawn: the per-instance daemon process.
 *
 * tau-spawn is daemonless from the user's perspective — it is launched
 * by `tau run` and runs in the background automatically when --detach
 * is used (or implicitly when the instance has detached spawns).
 *
 * Responsibilities
 * ────────────────
 *  1. Execute the fully-resolved instance manifest via Runner.
 *  2. Track all child spawn PIDs and their ring buffers.
 *  3. Serve the IPC socket so that `tau attach/cat/stop/ls` work.
 *  4. Persist instance state to instance.yml on every change.
 *  5. Reap zombie children (SIGCHLD handler).
 *  6. Watch instance.yml for external edits and hot-reload the manifest.
 *
 * Process lifecycle
 * ─────────────────
 *  tau run ./m.yml           → forks tau-spawn, then either:
 *    --detach: parent exits immediately after fork
 *    attached: parent enters IPCClient::attach() to forward stdin/stdout
 *
 *  tau-spawn exit conditions:
 *    - All spawns have exited AND no IPC clients are connected.
 *    - SIGTERM received (graceful shutdown: SIGTERM all children, wait).
 *    - SIGKILL received.
 */

#pragma once

#include "Instance.hpp"
#include "IPC.hpp"
#include "Runner.hpp"
#include "Manifest.hpp"

#include <Collection/String.hpp>
#include <Collection/Array.hpp>

namespace Tau {

using namespace Collection;
using namespace Xi;

// ─── Spawn process options ────────────────────────────────────────────────────
struct SpawnOptions {
    String         instanceName;       ///< Resolved unique name for this instance
    String         instanceDir;        ///< TAU_PATH/instances/<name>/
    String         manifestPath;       ///< Main / fallback manifest path
    Array<String>  manifestPaths;      ///< All entries passed on CLI: entry_0, entry_1, ..., entry_N
    String         sourceManifestPath; ///< Original source manifest path
    Array<String>  args;               ///< Extra args forwarded to spawned processes

    bool headless     = false;         ///< Do not create instance dir, no copy, no watch
    bool removeOnRead = false;         ///< Delete last entry after reading (implies headless)
    bool globalMode   = false;         ///< Copy last entry to instances/<name>/instance.yml and watch from there
    bool copyYaml     = true;          ///< Copy last yaml to instance dir (default: true)
    bool watch        = true;          ///< Watch last yaml for changes (default: true)
    String headDir;                    ///< Custom instance directory path
    String enterSlot;                  ///< Optional eslot name to enter (--enter <eslot-name>)

    bool attachStdin  = true;          ///< Forward stdin to first spawn (non-detach)
    bool detach       = false;         ///< Become a background daemon immediately
    String detachKey  = "ctrl+b";      ///< Key combo to detach (for IPCClient)
    int  cols         = 0;             ///< Terminal columns (0 = auto)
    int  rows         = 0;             ///< Terminal rows (0 = auto)
};


// ─── SpawnProcess ─────────────────────────────────────────────────────────────
/**
 * @class SpawnProcess
 * @brief Core of tau-instance: event loop, IPC server, SIGCHLD reaping.
 *
 * Usage (inside src/main_instance.cpp):
 * @code
 *   SpawnOptions opts = ...;
 *   SpawnProcess sp(opts);
 *   return sp.run();  // blocks until all spawns exit + no IPC clients
 * @endcode
 */
class SpawnProcess {
public:
    explicit SpawnProcess(const SpawnOptions &opts);
    ~SpawnProcess();

    /**
     * @brief Main event loop.
     *
     * - Executes the manifest via Runner.
     * - Serves the IPC socket.
     * - Reaps children via SIGCHLD.
     * - Updates instance.yml on state changes.
     * - Returns when all spawns have exited and no clients are connected.
     *
     * @return 0 on clean exit, 1 on error.
     */
    int run();

    /**
     * @brief Signal handler target (set up via sigaction).
     *
     * Writes a byte to the self-pipe so the event loop wakes up.
     */
    static void onSIGCHLD(int);
    static void onSIGTERM(int);

private:
    SpawnOptions            _opts;
    IPCServer               _ipc;
    InstanceState           _state;
    Runner                 *_runner = nullptr;
    Array<ParsedManifest *> _templates;
    String                  _watchedEntryPath;

    // Self-pipe for signal delivery into the poll() loop.
    int _sigPipe[2] = {-1, -1};
    int _netlinkFd  = -1;
    static volatile bool _termRequested;
    long long      _manifestMTimeNsec = 0;
    long long      _instYamlMTimeNsec = 0;
    uint64_t       _startupTime       = 0;


    // ─── Event loop helpers ───────────────────────────────────────────────────

    /** @brief Set up SIGCHLD and SIGTERM handlers via sigaction. */
    void setupSignals();

    /** @brief Set up Netlink route socket for IP/link kernel events. */
    void setupNetlink();

    /** @brief Drain and broadcast kernel Netlink IP and interface events. */
    void processNetlinkEvents();

    /** @brief Drain _sigPipe; reap any zombie children. */
    void reapChildren();

    /** @brief Evaluate CPU usage and idle countdown for autofreeze spawns. */
    void evaluateAutofreeze();

    /** @brief Save instance.yml after any state change. */
    void persistState();

    /** @brief Build IPC server handlers that close over this instance. */
    IPCServer::Handlers makeHandlers();

    /** @brief Check if tau-spawn should exit. */
    bool shouldExit() const;

    /** @brief Check and execute manifest hot reload. */
    void checkReload();

    /** @brief SIGTERM: send SIGTERM to all children, then wait. */

    void gracefulShutdown();
};

} // namespace Tau

