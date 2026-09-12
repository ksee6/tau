/**
 * @file IPC.cpp
 * @brief Unix socket IPC server/client and mmap ring buffer implementation.
 */

#include <Tau/IPC.hpp>
#include <Tau/Util.hpp>
#include <Terminal/Format.hpp>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>
#include <atomic>


namespace Tau {

// ═══════════════════════════════════════════════════════════════════════════════
// RingBuffer
// ═══════════════════════════════════════════════════════════════════════════════

RingBuffer::RingBuffer() = default;
RingBuffer::~RingBuffer() { close(); }

bool RingBuffer::open(const String &path, bool writer) {
    close();
    _writer = writer;

    size_t totalSize = sizeof(RingHeader) + RING_DATA_SIZE;

    if (writer) {
        _fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (_fd < 0) {
            logError("tau/ring: open ", path, ": ", ::strerror(errno));
            return false;
        }
        if (::ftruncate(_fd, (off_t)totalSize) != 0) {
            ::close(_fd); _fd = -1;
            return false;
        }
        _mapped = ::mmap(nullptr, totalSize, PROT_READ | PROT_WRITE,
                         MAP_SHARED, _fd, 0);
    } else {
        _fd = ::open(path.c_str(), O_RDONLY);
        if (_fd < 0) {
            return false;
        }
        _mapped = ::mmap(nullptr, totalSize, PROT_READ,
                         MAP_SHARED, _fd, 0);
    }

    if (_mapped == MAP_FAILED) {
        logError("tau/ring: mmap failed: ", ::strerror(errno));
        ::close(_fd); _fd = -1;
        _mapped = nullptr;
        return false;
    }

    _mapSize = totalSize;

    if (writer) {
        header()->write_pos = 0;
        header()->_pad      = 0;
    }

    return true;
}

void RingBuffer::write(const char *buf, size_t len) {
    if (!_writer || !_mapped || len == 0) return;

    RingHeader *hdr = header();
    char       *dat = data();

    uint64_t wp = __atomic_load_n(&hdr->write_pos, __ATOMIC_RELAXED);

    for (size_t i = 0; i < len; ++i) {
        dat[wp % RING_DATA_SIZE] = buf[i];
        ++wp;
    }

    __atomic_store_n(&hdr->write_pos, wp, __ATOMIC_RELEASE);
}

String RingBuffer::tail(int nlines) const {
    if (!_mapped) return "";

    const RingHeader *hdr = reinterpret_cast<const RingHeader *>(_mapped);
    uint64_t wp = __atomic_load_n(&hdr->write_pos, __ATOMIC_ACQUIRE);

    if (wp == 0) return "";

    const char *dat = reinterpret_cast<const char *>(_mapped) + sizeof(RingHeader);

    uint64_t avail = (wp < RING_DATA_SIZE) ? wp : RING_DATA_SIZE;
    uint64_t start = (wp >= RING_DATA_SIZE) ? (wp % RING_DATA_SIZE) : 0;

    if (nlines <= 0 || (uint64_t)nlines * 80 >= avail) {
        String result;
        for (uint64_t i = 0; i < avail; ++i) {
            result.push(dat[(start + i) % RING_DATA_SIZE]);
        }
        return result;
    }

    int found = 0;
    uint64_t cutoff = 0;
    for (uint64_t i = avail; i > 0; --i) {
        char c = dat[(start + i - 1) % RING_DATA_SIZE];
        if (c == '\n') {
            ++found;
            if (found == nlines) {
                cutoff = i;
                break;
            }
        }
    }

    String result;
    for (uint64_t i = cutoff; i < avail; ++i) {
        result.push(dat[(start + i) % RING_DATA_SIZE]);
    }
    return result;
}

void RingBuffer::close() {
    if (_mapped) {
        ::munmap(_mapped, _mapSize);
        _mapped  = nullptr;
        _mapSize = 0;
    }
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// IPCServer
// ═══════════════════════════════════════════════════════════════════════════════

IPCServer::IPCServer() = default;
IPCServer::~IPCServer() { destroy(); }

bool IPCServer::listen(const String &sockPath) {
    _sockPath = sockPath;
    ::unlink(sockPath.c_str());

    _listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (_listenFd < 0) {
        logError("tau/ipc: socket(): ", ::strerror(errno));
        return false;
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(_listenFd,
               reinterpret_cast<struct sockaddr *>(&addr),
               sizeof(addr)) != 0) {
        logError("tau/ipc: bind(", sockPath, "): ", ::strerror(errno));
        ::close(_listenFd); _listenFd = -1;
        return false;
    }

    ::chmod(sockPath.c_str(), 0600);

    if (::listen(_listenFd, 8) != 0) {
        logError("tau/ipc: listen(): ", ::strerror(errno));
        ::close(_listenFd); _listenFd = -1;
        return false;
    }

    return true;
}

void IPCServer::setHandlers(const Handlers &h) {
    _handlers = h;
}

void IPCServer::_acceptNew() {
    for (;;) {
        int fd = ::accept4(_listenFd, nullptr, nullptr, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            logWarn("tau/ipc: accept4(): ", ::strerror(errno));
            break;
        }
        auto *c = new Client();
        c->fd = fd;
        _clients.push(c);
    }
}

void IPCServer::_dispatchCommand(Client *c, const String &cmd) {
    Array<String> parts = cmd.trim().split(" ");
    if (parts.length() == 0) return;

    String verb = parts[0];

    if (verb == "ATTACH" && parts.length() >= 2) {
        String spawnName = parts[1];
        if (_handlers.wake) _handlers.wake(spawnName);
        if (!_handlers.getPTY) { ::write(c->fd, "ERR no handler\n", 15); return; }
        int ptyFd = _handlers.getPTY(spawnName);
        if (ptyFd < 0) { ::write(c->fd, "OK (exited)\n", 12); return; }
        ::write(c->fd, "OK\n", 3);
        c->attached    = true;
        c->attachSpawn = spawnName;
        c->ptyFd       = ptyFd;
        return;
    }

    if (verb == "CAT" && parts.length() >= 2) {
        String spawnName = parts[1];
        if (_handlers.wake) _handlers.wake(spawnName);
        int nlines = (parts.length() >= 3) ? (int)Xi::parseLong(parts[2]) : 100;
        if (!_handlers.cat) { ::write(c->fd, ".\n", 2); return; }
        String out = _handlers.cat(spawnName, nlines);
        ::write(c->fd, out.c_str(), out.length());
        ::write(c->fd, "\n.\n", 3);
        return;
    }

    if (verb == "SIGNAL" && parts.length() >= 3) {
        String spawnName = parts[1];
        if (_handlers.wake) _handlers.wake(spawnName);
        int sig = (int)Xi::parseLong(parts[2]);
        bool ok = _handlers.signal ? _handlers.signal(spawnName, sig) : false;
        if (ok) ::write(c->fd, "OK\n", 3);
        else    ::write(c->fd, "ERR\n", 4);
        return;
    }

    if (verb == "WAKE") {
        String targetSpawn = parts.length() >= 2 ? parts[1] : "";
        bool ok = _handlers.wake ? _handlers.wake(targetSpawn) : false;
        if (ok) ::write(c->fd, "OK\n", 3);
        else    ::write(c->fd, "ERR\n", 4);
        return;
    }

    if (verb == "STOP" && parts.length() >= 4) {
        String spawnName = parts[1];
        int sig       = (int)Xi::parseLong(parts[2]);
        int timeoutMs = (int)Xi::parseLong(parts[3]);
        bool ok = _handlers.stop ? _handlers.stop(spawnName, sig, timeoutMs) : false;
        if (ok) ::write(c->fd, "OK\n", 3);
        else    ::write(c->fd, "ERR\n", 4);
        return;
    }

    if (verb == "RESIZE" && parts.length() >= 4) {
        String spawnName = parts[1];
        int cols = (int)Xi::parseLong(parts[2]);
        int rows = (int)Xi::parseLong(parts[3]);
        bool ok = _handlers.resize ? _handlers.resize(spawnName, cols, rows) : false;
        if (ok) ::write(c->fd, "OK\n", 3);
        else    ::write(c->fd, "ERR\n", 4);
        return;
    }

    if (verb == "LIST") {
        String out = _handlers.list ? _handlers.list() : "";
        ::write(c->fd, out.c_str(), out.length());
        ::write(c->fd, "\n.\n", 3);
        return;
    }

    if (verb == "SNAPSHOT") {
        String targetSpawn = parts.length() >= 2 ? parts[1] : "";
        String ts = _handlers.snapshot ? _handlers.snapshot(targetSpawn, false, false, false) : "";
        if (!ts.isEmpty()) {
            String resp = "OK " + ts + "\n";
            ::write(c->fd, resp.c_str(), resp.length());
        } else {
            ::write(c->fd, "ERR snapshot failed\n", 20);
        }
        return;
    }

    if (verb == "FREEZE") {
        String targetSpawn = parts.length() >= 2 ? parts[1] : "";
        bool isSoft = parts.length() >= 3 && (parts[2] == "1" || parts[2] == "true" || parts[2] == "soft");
        bool wol = parts.length() >= 4 ? (parts[3] == "1" || parts[3] == "true" || parts[3] == "wol") : true;
        if (!targetSpawn.isEmpty() && targetSpawn != "all") {
            isSoft = true; // targeting a specific spawn is implicitly soft
        }
        String ts = _handlers.snapshot ? _handlers.snapshot(targetSpawn, true, isSoft, wol) : "";
        if (!ts.isEmpty()) {
            String resp = "OK " + ts + "\n";
            ::write(c->fd, resp.c_str(), resp.length());
        } else {
            ::write(c->fd, "ERR freeze failed\n", 18);
        }
        return;
    }

    ::write(c->fd, "ERR unknown command\n", 20);
}

void IPCServer::_processClient(Client *c) {
    if (c->attached) {
        char buf[1024];
        ssize_t n = ::read(c->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            _removeClient(c);
            return;
        }
        if (n == 0) {
            _removeClient(c);
            return;
        }
        if (c->ptyFd >= 0) {
            ::write(c->ptyFd, buf, (size_t)n);
        }
        return;
    }

    char buf[1024];
    ssize_t n = ::read(c->fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        _removeClient(c);
        return;
    }
    if (n == 0) {
        _removeClient(c);
        return;
    }
    buf[n] = '\0';
    c->readBuf += String(buf);

    for (;;) {
        long long nl = c->readBuf.find("\n");
        if (nl < 0) break;
        String line = c->readBuf.substring(0, (size_t)nl);
        c->readBuf  = c->readBuf.substring((size_t)nl + 1);
        _dispatchCommand(c, line);
    }
}


void IPCServer::_sendFd(int connFd, int fdToSend) {
    char buf[4] = "OK\n";
    struct iovec iov = { buf, 3 };

    char cmsgBuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    ::memcpy(CMSG_DATA(cmsg), &fdToSend, sizeof(int));

    ::sendmsg(connFd, &msg, 0);
}

void IPCServer::_removeClient(Client *c) {
    if (c->fd >= 0) ::close(c->fd);
    for (size_t i = 0; i < _clients.length(); ++i) {
        if (_clients[i] == c) {
            _clients.splice(i, 1);
            break;
        }
    }
    delete c;
}

void IPCServer::update() {
    _acceptNew();

    for (size_t i = 0; i < _clients.length(); ) {
        _processClient(_clients[i]);
        if (i < _clients.length() && _clients[i]->fd >= 0)
            ++i;
    }
}

void IPCServer::fillPollFds(struct pollfd *pfds, nfds_t &npfds, nfds_t maxFds) {
    if (_listenFd >= 0 && npfds < maxFds) {
        pfds[npfds].fd      = _listenFd;
        pfds[npfds].events  = POLLIN;
        pfds[npfds].revents = 0;
        npfds++;
    }
    for (size_t i = 0; i < _clients.length(); ++i) {
        if (_clients[i]->fd >= 0 && npfds < maxFds) {
            pfds[npfds].fd      = _clients[i]->fd;
            pfds[npfds].events  = POLLIN;
            pfds[npfds].revents = 0;
            npfds++;
        }
    }
}

void IPCServer::broadcast(const String &spawnName, const char *data, size_t len) {
    if (!data || len == 0) return;
    for (size_t i = 0; i < _clients.length(); ++i) {
        Client *c = _clients[i];
        if (c && c->attached && c->attachSpawn == spawnName && c->fd >= 0) {
            ::write(c->fd, data, len);
        }
    }
}

void IPCServer::destroy() {
    for (size_t i = 0; i < _clients.length(); ++i) {
        if (_clients[i]->fd >= 0) ::close(_clients[i]->fd);
        delete _clients[i];
    }
    _clients.clear();

    if (_listenFd >= 0) {
        ::close(_listenFd);
        _listenFd = -1;
    }
    if (!_sockPath.isEmpty()) {
        ::unlink(_sockPath.c_str());
        _sockPath = "";
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// IPCClient
// ═══════════════════════════════════════════════════════════════════════════════

IPCClient::IPCClient() = default;
IPCClient::~IPCClient() { disconnect(); }

bool IPCClient::connect(const String &instanceDir, bool quiet) {
    disconnect();
    String sockPath = instanceDir + "/tau.sock";

    _fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (_fd < 0) return false;

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(_fd, reinterpret_cast<struct sockaddr *>(&addr),
                  sizeof(addr)) != 0) {
        if (!quiet)
            logError("tau: cannot connect to ", sockPath, ": ", ::strerror(errno));
        ::close(_fd); _fd = -1;
        return false;
    }

    struct timeval tv;
    tv.tv_sec  = 3;
    tv.tv_usec = 0;
    ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return true;
}

String IPCClient::_sendRecv(const String &cmd) {
    if (_fd < 0) return "";
    ::write(_fd, cmd.c_str(), cmd.length());
    ::write(_fd, "\n", 1);

    String result;
    char buf[256];
    for (;;) {
        ssize_t n = ::read(_fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        result += String(buf);
        if (result.find("\n.\n") >= 0) {
            long long dot = result.find("\n.\n");
            result = result.substring(0, (size_t)dot);
            break;
        }
        if (result.endsWith("OK\n") || result.startsWith("ERR"))
            break;
    }
    return result;
}

int IPCClient::_recvFd() {
    char buf[64] = {};
    struct iovec iov = { buf, sizeof(buf) - 1 };

    char cmsgBuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);

    ssize_t n = ::recvmsg(_fd, &msg, 0);
    if (n <= 0) return -1;
    buf[n] = '\0';
    if (::strncmp(buf, "OK", 2) != 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;

    int fd;
    ::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

static volatile sig_atomic_t g_clientWinch = 0;
static void handleClientWinch(int) {
    g_clientWinch = 1;
}

bool IPCClient::resize(const String &spawnName, int cols, int rows) {
    if (_fd < 0 || cols <= 0 || rows <= 0) return false;
    String cmd = "RESIZE " + (spawnName.isEmpty() ? "0" : spawnName) + " " + intStr(cols) + " " + intStr(rows);
    String res = _sendRecv(cmd);
    return res.startsWith("OK");
}


void IPCClient::attach(const String &spawnName, bool attachStdin, const String &detachKey, int cols, int rows) {
    if (_fd < 0) return;

    if (cols > 0 && rows > 0) {
        resize(spawnName, cols, rows);
    }

    String cmd = String("ATTACH ") + spawnName;
    String res;
    for (int retry = 0; retry < 500; ++retry) {
        res = _sendRecv(cmd);
        if (res.startsWith("OK") && res.find("exited") < 0) break;
        ::usleep(10000); // 10ms
    }
    if (res.isEmpty() || res.find("exited") >= 0) {
        return;
    }
    if (!res.startsWith("OK")) {
        logError("tau: ATTACH failed: ", res);
        return;
    }

    struct sigaction saOld, saNew = {};
    saNew.sa_handler = handleClientWinch;
    sigemptyset(&saNew.sa_mask);
    saNew.sa_flags = SA_RESTART;
    ::sigaction(SIGWINCH, &saNew, &saOld);


    uint8_t detachByte = 0;
    if (!detachKey.isEmpty() && detachKey != "false" && detachKey != "none" && detachKey != "0") {
        detachByte = 0x02; // default ctrl+b
        if (detachKey.length() >= 6 && detachKey.startsWith("ctrl+")) {
            char ch = detachKey[5];
            if (ch >= 'a' && ch <= 'z') detachByte = (uint8_t)(ch - 'a' + 1);
            if (ch >= 'A' && ch <= 'Z') detachByte = (uint8_t)(ch - 'A' + 1);
        }
    }

    struct termios orig_termios, raw_termios;
    bool is_tty = false;

    if (attachStdin) {
        is_tty = (::tcgetattr(STDIN_FILENO, &orig_termios) == 0);
        if (is_tty) {
            ::tcflush(STDIN_FILENO, TCIFLUSH);
            raw_termios = orig_termios;
            ::cfmakeraw(&raw_termios);
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios);
            ::tcflush(STDIN_FILENO, TCIFLUSH);
        }
    }


    struct pollfd fds[3];
    int nfds = 2;
    fds[0].fd     = _fd;
    fds[0].events = POLLIN;
    fds[1].fd     = STDOUT_FILENO;
    fds[1].events = 0;

    if (attachStdin) {
        fds[2].fd     = STDIN_FILENO;
        fds[2].events = POLLIN;
        nfds = 3;
    }

    char io[4096];
    bool running = true;
    [[maybe_unused]] bool is_detached = false;

    while (running) {
        if (g_clientWinch) {
            g_clientWinch = 0;
            struct winsize ws = {};
            if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
                resize(spawnName, ws.ws_col, ws.ws_row);
            } else if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
                resize(spawnName, ws.ws_col, ws.ws_row);
            }
        }

        int ready = ::poll(fds, (nfds_t)nfds, 100);
        if (ready < 0 && errno != EINTR) break;

        if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) running = false;

        // Socket -> stdout
        if (fds[0].revents & POLLIN) {
            ssize_t r = ::read(_fd, io, sizeof(io));
            if (r > 0) {
                ssize_t w = ::write(STDOUT_FILENO, io, (size_t)r);
                if (w < 0 && (errno == EPIPE || errno == EBADF)) running = false;
            } else {
                running = false;
            }

        }
        if (fds[0].revents & (POLLHUP | POLLERR)) running = false;

        // Stdin -> Socket (if attachStdin)
        if (attachStdin && nfds > 2 && fds[2].fd >= 0 && (fds[2].revents & POLLIN)) {
            ssize_t r = ::read(STDIN_FILENO, io, sizeof(io));
            if (r > 0) {
                if (detachByte != 0) {
                    for (ssize_t i = 0; i < r; ++i) {
                        if ((uint8_t)io[i] == detachByte) {
                            is_detached = true;
                            running = false;
                            break;
                        }
                    }
                }
                if (running) ::write(_fd, io, (size_t)r);
            } else {
                if (is_tty) running = false;
                else fds[2].fd = -1; // EOF on pipe input
            }
        }
    }

    ::sigaction(SIGWINCH, &saOld, nullptr);

    if (is_tty) {
        ::tcflush(STDIN_FILENO, TCIFLUSH);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }



}





void IPCClient::cat(const String &spawnName, int nlines) {
    String cmd = String("CAT ") + spawnName + " " + intStr(nlines);
    String result = _sendRecv(cmd);
    if (!result.isEmpty()) {
        ::write(STDOUT_FILENO, result.c_str(), result.length());
        ::write(STDOUT_FILENO, "\n", 1);
    }
}

bool IPCClient::signal(const String &spawnName, int sig) {
    String cmd = String("SIGNAL ") + spawnName + " " + intStr(sig);
    String result = _sendRecv(cmd);
    return result.startsWith("OK");
}

bool IPCClient::stop(const String &spawnName, int sig, int timeoutMs) {
    String cmd = String("STOP ") + spawnName + " " +
                 intStr(sig) + " " + intStr(timeoutMs);
    String result = _sendRecv(cmd);
    return result.startsWith("OK");
}

String IPCClient::list() {
    return _sendRecv("LIST");
}

String IPCClient::snapshot(const String &spawnName) {
    String cmd = "SNAPSHOT";
    if (!spawnName.isEmpty()) cmd += " " + spawnName;
    String resp = _sendRecv(cmd);
    if (resp.startsWith("OK")) {
        Array<String> parts = resp.split(" ");
        if (parts.length() >= 2) return parts[1].trim();
        return "true";
    }
    return "";
}

String IPCClient::freeze(const String &spawnName, bool isSoft, bool wol) {
    String cmd = "FREEZE";
    if (!spawnName.isEmpty()) {
        cmd += " " + spawnName + " " + (isSoft ? "1" : "0") + " " + (wol ? "1" : "0");
    } else if (isSoft || !wol) {
        cmd += " all " + String(isSoft ? "1" : "0") + " " + String(wol ? "1" : "0");
    }
    String resp = _sendRecv(cmd);
    if (resp.startsWith("OK")) {
        Array<String> parts = resp.split(" ");
        if (parts.length() >= 2) return parts[1].trim();
        return "true";
    }
    return "";
}

bool IPCClient::wake(const String &spawnName) {
    String cmd = "WAKE";
    if (!spawnName.isEmpty()) cmd += " " + spawnName;
    String resp = _sendRecv(cmd);
    return resp.startsWith("OK");
}

void IPCClient::disconnect() {
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
}

} // namespace Tau
