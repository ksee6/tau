/**
 * @file IPC.hpp
 * @brief tau-spawn ↔ tau-client IPC: Unix socket + mmap ring buffers.
 *
 * Design
 * ──────
 * tau-spawn creates a Unix domain socket at:
 *   TAU_PATH/instances/<name>/tau.sock
 *
 * Each spawn's stdout/stderr are tee'd into a pair of mmap'd circular files:
 *   TAU_PATH/instances/<name>/spawns/<spawnname>/out.ring
 *   TAU_PATH/instances/<name>/spawns/<spawnname>/err.ring
 *
 * Ring file layout (RING_BUFFER_SIZE + sizeof(RingHeader) bytes):
 *   [0..15]  RingHeader { uint64_t write_pos; uint64_t _pad; }
 *   [16..]   data bytes (circular, write_pos mod RING_DATA_SIZE gives slot)
 *
 * Socket protocol (newline-terminated text commands):
 *   C→S: "ATTACH <spawnname>\n"
 *        S replies with "OK\n", then sends the PTY master fd via SCM_RIGHTS.
 *        Raw bytes flow bidirectionally until the client disconnects.
 *
 *   C→S: "CAT <spawnname> <nlines>\n"
 *        S replies with the last <nlines> lines from out.ring, terminated
 *        by ".\n" (a single dot on its own line, SMTP-style).
 *
 *   C→S: "SIGNAL <spawnname> <signum>\n"
 *        S replies "OK\n" on success or "ERR <reason>\n".
 *
 *   C→S: "LIST\n"
 *        S replies with a YAML dump of the instance state, terminated by ".\n".
 *
 *   C→S: "STOP <spawnname> <signum> <timeout_ms>\n"
 *        S sends signum, waits up to timeout_ms, then SIGKILL. Replies "OK\n".
 */

#pragma once

#include "Config.hpp"
#include <Xi/String.hpp>
#include <Xi/Array.hpp>
#include <Xi/Func.hpp>

#include <cstdint>
#include <sys/types.h>
#include <poll.h>

namespace Tau {

using namespace Xi;

// ─── Ring buffer ──────────────────────────────────────────────────────────────

/// Size of the data region inside each .ring file.
static constexpr size_t RING_DATA_SIZE = RING_BUFFER_SIZE - 16;

#pragma pack(push, 1)
struct RingHeader {
    uint64_t write_pos;  ///< Absolute bytes written (monotonically increasing).
    uint64_t _pad;
};
#pragma pack(pop)

/**
 * @class RingBuffer
 * @brief mmap-backed circular log buffer for a single spawn stream.
 *
 * One RingBuffer instance is held by tau-spawn per spawn per stream.
 * Readers (tau cat) map the file independently and read without locking;
 * the single-writer / multiple-reader model is safe because:
 *   - write_pos is updated after data is written (store-release).
 *   - Readers check write_pos first (load-acquire) then read data.
 */
class RingBuffer {
public:
    RingBuffer();
    ~RingBuffer();

    /**
     * @brief Open (and create if needed) the ring file.
     * @param path Absolute path to the .ring file.
     * @param writer true = writer mode (tau-spawn), false = reader mode.
     * @return true on success.
     */
    bool open(const String &path, bool writer);

    /** @brief Write bytes into the ring (writer only). */
    void write(const char *data, size_t len);

    /**
     * @brief Read the last `nlines` lines from the ring.
     * @param nlines  Number of lines to retrieve (0 = all).
     * @return Lines joined by '\n'.
     */
    String tail(int nlines) const;

    /** @brief Unmap and close. */
    void close();

    bool isOpen() const { return _mapped != nullptr; }

private:
    void    *_mapped  = nullptr;   ///< mmap pointer (file-size bytes).
    size_t   _mapSize = 0;
    int      _fd      = -1;
    bool     _writer  = false;

    RingHeader *header() const {
        return reinterpret_cast<RingHeader *>(_mapped);
    }
    char *data() const {
        return reinterpret_cast<char *>(_mapped) + sizeof(RingHeader);
    }
};

// ─── IPC Server (runs inside tau-spawn) ───────────────────────────────────────

/**
 * @class IPCServer
 * @brief Listens on tau.sock and dispatches incoming commands.
 *
 * Runs non-blocking; integrate with tau-spawn's event loop via update().
 */
class IPCServer {
public:
    /**
     * @brief Callbacks registered by tau-spawn.
     */
    struct Handlers {
        /// Return the PTY master fd for a named spawn (-1 if not found).
        Func<int(const String &spawnName)> getPTY;
        /// Return tail of stdout ring for a spawn.
        Func<String(const String &spawnName, int nlines)> cat;
        /// Send a signal to a named spawn. Return true on success.
        Func<bool(const String &spawnName, int sig)>      signal;
        /// Stop a spawn gracefully (sig → timeout → SIGKILL).
        Func<bool(const String &spawnName, int sig, int timeoutMs)> stop;
        /// Resize a spawn's PTY window.
        Func<bool(const String &spawnName, int cols, int rows)> resize;
        /// Dump instance state as YAML.
        Func<String()>                                     list;
        /// Take snapshot (or freeze) and return timestamp string (empty on error).
        Func<String(const String &spawnName, bool isFreeze, bool isSoft, bool wol)> snapshot;
        /// Wake a soft-frozen spawn.
        Func<bool(const String &spawnName)>                wake;
    };


    IPCServer();
    ~IPCServer();

    /**
     * @brief Create and bind the socket.
     * @param sockPath  Absolute path for the Unix socket file.
     * @return true on success.
     */
    bool listen(const String &sockPath);

    /** @brief Register handler callbacks. */
    void setHandlers(const Handlers &h);

    /**
     * @brief Non-blocking poll: accept pending connections, read/dispatch
     *        available commands, send responses.  Call from event loop.
     */
    void update();

    /** @brief Populate pollfd array with listener and client sockets for zero-latency wakeup. */
    void fillPollFds(struct pollfd *pfds, nfds_t &npfds, nfds_t maxFds);

    /** @brief Broadcast raw output data to any attached clients for this spawn. */
    void broadcast(const String &spawnName, const char *data, size_t len);

    /** @brief Close the socket and all active client connections. */
    void destroy();

private:
    int      _listenFd = -1;
    String   _sockPath;
    Handlers _handlers;

    struct Client {
        int    fd       = -1;
        String readBuf;          ///< Partial command accumulator.
        bool   attached = false; ///< In raw-attach mode (no more commands).
        String attachSpawn;      ///< Spawn name we are attached to.
        int    ptyFd    = -1;    ///< PTY master fd (only valid if attached).
    };

    Array<Client *> _clients;

    void _acceptNew();
    void _processClient(Client *c);
    void _dispatchCommand(Client *c, const String &cmd);
    void _sendFd(int connFd, int fdToSend);
    void _removeClient(Client *c);
};

// ─── IPC Client (runs inside tau, tau attach, tau cat, tau stop) ──────────────

/**
 * @class IPCClient
 * @brief Blocking client that connects to tau-spawn's socket.
 */
class IPCClient {
public:
    IPCClient();
    ~IPCClient();

    /**
     * @brief Connect to a named instance's socket.
     * @param instanceDir  Directory containing tau.sock.
     * @return true on success.
     */
    bool connect(const String &instanceDir, bool quiet = false);

    /**
     * @brief Send RESIZE command to set terminal window size on the spawn's PTY.
     */
    bool resize(const String &spawnName, int cols, int rows);

    /**
     * @brief Send ATTACH command and connect to the PTY stream.
     * If attachStdin is true, enters raw mode and pumps stdin↔pty.
     * If attachStdin is false, streams pty output to stdout (read-only tail).
     * @param spawnName    Spawn to attach to.
     * @param attachStdin  Whether to forward stdin input.
     * @param detachKey    Key sequence string (e.g. "ctrl+b", "false" for none).
     * @param cols         Initial column width (0 = auto/leave current).
     * @param rows         Initial row height (0 = auto/leave current).
     */
    void attach(const String &spawnName, bool attachStdin = false, const String &detachKey = "ctrl+b", int cols = 0, int rows = 0);



    /**
     * @brief Send CAT command and print last nlines of stdout.
     * @param spawnName  Spawn to read.
     * @param nlines     Number of lines (default 100).
     */
    void cat(const String &spawnName, int nlines);

    /**
     * @brief Send SIGNAL command.
     * @return true if tau-spawn acknowledged OK.
     */
    bool signal(const String &spawnName, int sig);

    /**
     * @brief Send STOP command.
     * @param spawnName   Spawn to stop.
     * @param sig         Initial signal.
     * @param timeoutMs   Milliseconds before SIGKILL (0 = infinity).
     */
    bool stop(const String &spawnName, int sig, int timeoutMs);

    /**
     * @brief Send LIST command and return YAML string.
     */
    String list();

    /**
     * @brief Send SNAPSHOT command and return timestamp string (empty on error).
     */
    String snapshot(const String &spawnName = "");

    /**
     * @brief Send FREEZE command and return timestamp string (empty on error).
     */
    String freeze(const String &spawnName = "", bool isSoft = false, bool wol = false);

    /**
     * @brief Send WAKE command to resume a soft-frozen spawn.
     */
    bool wake(const String &spawnName = "");

    /** @brief Close connection. */
    void disconnect();

private:
    int    _fd = -1;

    /** @brief Send a command line and read lines until ".\n" or "OK\n". */
    String _sendRecv(const String &cmd);

    /** @brief Receive a file descriptor via SCM_RIGHTS. */
    int _recvFd();
};

} // namespace Tau
