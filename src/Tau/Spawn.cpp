/**
 * @file Spawn.cpp
 * @brief tau-spawn: per-instance daemon with event loop, IPC, and SIGCHLD.
 */

#include <Tau/Spawn.hpp>
#include <Tau/Namespace.hpp>
#include <Tau/Cgroup.hpp>
#include <Tau/Manifest.hpp>
#include <Tau/Monitor.hpp>
#include <Tau/Snapshot.hpp>
#include <Tau/Util.hpp>


#include <Resource/File.hpp>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>
#include <net/if.h>


namespace Tau {

using namespace Collection;
using namespace Xi;

// ─── Static signal state ──────────────────────────────────────────────────────

volatile bool SpawnProcess::_termRequested = false;

void SpawnProcess::onSIGCHLD(int) {
    // Written to signal pipe; actual reaping happens in the event loop.
    // The self-pipe is a static; we write 1 byte to wake up poll().
    // (The fd is set up per-instance, so we use a global trick here.)
    // In practice: poll() on SIGCHLD unblocking is sufficient since
    // poll() returns EINTR on signal delivery.
}

void SpawnProcess::onSIGTERM(int) {
    _termRequested = true;
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

SpawnProcess::SpawnProcess(const SpawnOptions &opts)
    : _opts(opts) {
    _sigPipe[0] = _sigPipe[1] = -1;
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    _startupTime = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

SpawnProcess::~SpawnProcess() {
    for (auto *t : _templates) delete t;
    _templates.clear();
    delete _runner;
    _runner = nullptr;
    if (_sigPipe[0] >= 0) { ::close(_sigPipe[0]); _sigPipe[0] = -1; }
    if (_sigPipe[1] >= 0) { ::close(_sigPipe[1]); _sigPipe[1] = -1; }
    if (_netlinkFd >= 0)  { ::close(_netlinkFd);  _netlinkFd  = -1; }
}

// ─── setupSignals ─────────────────────────────────────────────────────────────


void SpawnProcess::setupSignals() {
    // Self-pipe for waking up poll() on SIGCHLD
    ::pipe(_sigPipe);
    ::fcntl(_sigPipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(_sigPipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction sa = {};
    sa.sa_handler = onSIGCHLD;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGCHLD, &sa, nullptr);

    struct sigaction st = {};
    st.sa_handler = onSIGTERM;
    st.sa_flags   = SA_RESTART;
    ::sigemptyset(&st.sa_mask);
    ::sigaction(SIGTERM, &st, nullptr);

    // Ignore SIGHUP (sent when terminal closes)
    signal(SIGHUP, SIG_IGN);
}

// ─── setupNetlink ─────────────────────────────────────────────────────────────

void SpawnProcess::setupNetlink() {
    if (_netlinkFd >= 0) return;
    _netlinkFd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (_netlinkFd >= 0) {
        struct sockaddr_nl sa = {};
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
        if (::bind(_netlinkFd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            ::close(_netlinkFd);
            _netlinkFd = -1;
        }
    }
}

void SpawnProcess::processNetlinkEvents() {
    if (_netlinkFd < 0) return;
    char buf[8192];
    while (true) {
        ssize_t len = ::recv(_netlinkFd, buf, sizeof(buf), MSG_DONTWAIT);
        if (len <= 0) break;

        struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(nlh, (size_t)len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE) break;
            if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR) {
                struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
                struct rtattr *rth = IFA_RTA(ifa);
                int rtl = IFA_PAYLOAD(nlh);
                char ifname[IF_NAMESIZE] = {};
                ::if_indextoname(ifa->ifa_index, ifname);
                char ipStr[INET6_ADDRSTRLEN] = {};
                for (; RTA_OK(rth, rtl); rth = RTA_NEXT(rth, rtl)) {
                    if (rth->rta_type == IFA_ADDRESS || rth->rta_type == IFA_LOCAL) {
                        if (ifa->ifa_family == AF_INET) {
                            ::inet_ntop(AF_INET, RTA_DATA(rth), ipStr, sizeof(ipStr));
                        } else if (ifa->ifa_family == AF_INET6) {
                            ::inet_ntop(AF_INET6, RTA_DATA(rth), ipStr, sizeof(ipStr));
                        }
                    }
                }
                if (ifname[0] && ipStr[0]) {
                    String action = (nlh->nlmsg_type == RTM_NEWADDR) ? "add" : "del";
                    String ipData = "action=" + action + " iface=" + String(ifname) + " ip=" + String(ipStr) + "/" + intStr(ifa->ifa_prefixlen);
                    Monitor::broadcast("IP", _opts.instanceName, _startupTime, ipData);
                }
            } else if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
                struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
                char ifname[IF_NAMESIZE] = {};
                ::if_indextoname(ifi->ifi_index, ifname);
                bool isUp = (ifi->ifi_flags & IFF_UP) != 0;
                if (ifname[0]) {
                    String state = (nlh->nlmsg_type == RTM_DELLINK) ? "deleted" : (isUp ? "up" : "down");
                    Monitor::broadcast("IFACE", _opts.instanceName, _startupTime, "iface=" + String(ifname) + " state=" + state);
                }
            }
        }
    }
}

// ─── makeHandlers ─────────────────────────────────────────────────────────────

IPCServer::Handlers SpawnProcess::makeHandlers() {
    IPCServer::Handlers h;

    h.getPTY = [this](const String &name) -> int {
        if (!_runner) return -1;
        if (name == "0" || name.isEmpty()) {
            for (auto *sp : _runner->activeSpawns()) {
                if (sp->ptyMaster >= 0 && !sp->waited) return sp->ptyMaster;
            }
            if (_runner->activeSpawns().length() > 0) {
                return _runner->activeSpawns()[0]->ptyMaster;
            }
        }
        for (auto *sp : _runner->activeSpawns()) {
            if (sp->name == name) return sp->ptyMaster;
        }
        return -1;
    };

    h.cat = [this](const String &name, int nlines) -> String {
        if (!_runner) return "";
        for (auto *sp : _runner->activeSpawns()) {
            if (sp->name == name) return sp->outRing.tail(nlines);
        }
        return "";
    };

    h.signal = [this](const String &name, int sig) -> bool {
        if (!_runner) return false;
        return _runner->signalSpawn(name, sig);
    };

    h.stop = [this](const String &name, int sig, int timeoutMs) -> bool {
        (void)timeoutMs;
        if (!_runner) return false;
        _runner->signalSpawn(name, sig);
        _runner->signalSpawn(name, SIGKILL);
        _termRequested = true;
        return true;
    };

    h.resize = [this](const String &name, int cols, int rows) -> bool {
        if (!_runner) return false;
        return _runner->resizeSpawn(name, cols, rows);
    };

    h.list = [this]() -> String {
        return Instance::toYAML(_state);
    };

    h.snapshot = [this](bool isFreeze) -> String {
        String ts;
        bool ok = Snapshot::take(_opts.instanceDir, _state, isFreeze, ts);
        if (ok && isFreeze) {
            _state.status = InstanceStatus::Frozen;
            persistState();
            _termRequested = true;
        }
        return ok ? ts : "";
    };

    return h;
}


// ─── persistState ─────────────────────────────────────────────────────────────

void SpawnProcess::persistState() {
    if (_opts.instanceDir.isEmpty()) return;
    Instance::save(_opts.instanceDir, _state);
}

// ─── reapChildren ─────────────────────────────────────────────────────────────

void SpawnProcess::reapChildren() {
    // Drain self-pipe
    char buf[64];
    while (::read(_sigPipe[0], buf, sizeof(buf)) > 0) {}

    if (!_runner) return;

    // Non-blocking waitpid for any child
    for (;;) {
        int status;
        pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;

        // Find matching spawn
        for (auto *sp : _runner->activeSpawns()) {
            if (sp->pid == pid) {
                sp->waited = true;
                if (WIFEXITED(status))
                    sp->exitCode = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    sp->exitCode = -WTERMSIG(status);

                if (sp->ptyMaster >= 0) {
                    ::close(sp->ptyMaster);
                    sp->ptyMaster = -1;
                }

                // Update instance state
                for (size_t i = 0; i < _state.spawns.length(); ++i) {
                    if (_state.spawns[i].pid == pid) {
                        _state.spawns[i].status = (sp->exitCode == 0)
                            ? SpawnStatus::Exited : SpawnStatus::Failed;
                        _state.spawns[i].exitCode = sp->exitCode;
                        break;
                    }
                }

                persistState();
                break;
            }
        }
    }
}


// ─── shouldExit ───────────────────────────────────────────────────────────────

bool SpawnProcess::shouldExit() const {
    if (_termRequested) return true;
    if (!_runner) return true;

    // Exit when all spawns have exited
    for (auto *sp : _runner->activeSpawns()) {
        if (!sp->waited) return false;
    }
    return true;
}


// ─── gracefulShutdown ─────────────────────────────────────────────────────────

void SpawnProcess::gracefulShutdown() {
    if (!_runner) return;

    // Send SIGTERM to all children and their process groups
    for (auto *sp : _runner->activeSpawns()) {
        if (sp->pid > 0 && !sp->waited) {
            ::kill(-sp->pid, SIGTERM);
            ::kill(sp->pid, SIGTERM);
        }
    }

    // Wait up to DEFAULT_STOP_TIMEOUT_SECS seconds, then SIGKILL
    int waited = 0;
    while (waited < DEFAULT_STOP_TIMEOUT_SECS * 10) {
        bool anyRunning = false;
        for (auto *sp : _runner->activeSpawns()) {
            if (!sp->waited) { anyRunning = true; break; }
        }
        if (!anyRunning) break;

        ::usleep(100000);  // 100ms
        ++waited;
        reapChildren();
    }

    // SIGKILL stragglers and their process groups
    for (auto *sp : _runner->activeSpawns()) {
        if (sp->pid > 0 && !sp->waited) {
            ::kill(-sp->pid, SIGKILL);
            ::kill(sp->pid, SIGKILL);
        }
    }

    _runner->waitAll();
}

// ─── run ──────────────────────────────────────────────────────────────────────

int SpawnProcess::run() {
    // Ensure instance directory exists
    Resource::LinuxFS fs;
    fs.mkdir(_opts.instanceDir);

    // Set up signals
    setupSignals();

    // Start host namespace helper (must be done before entering any unshared namespaces)
    Namespace::initHostHelper();

    // Initialise instance state
    _state.name         = _opts.instanceName;
    _state.spawnPID     = ::getpid();
    _state.manifestPath = _opts.manifestPath;
    _state.status       = InstanceStatus::Running;
    persistState();

    // Start IPC server
    String sockPath = _opts.instanceDir + "/tau.sock";
    if (!_ipc.listen(sockPath)) {
        logError("tau-instance: cannot start IPC server");
        return 1;
    }
    _ipc.setHandlers(makeHandlers());

    // Set up runner
    RunnerOptions runOpts;
    runOpts.instanceName  = _opts.instanceName;
    runOpts.instanceDir   = _opts.instanceDir;
    runOpts.manifestPath  = _opts.manifestPath;
    runOpts.sourceManifestPath = _opts.sourceManifestPath;
    runOpts.detach = _opts.detach;
    runOpts.cols   = _opts.cols;
    runOpts.rows   = _opts.rows;

    runOpts.onSpawnChange = [this](const SpawnState &sp) {
        bool found = false;
        for (size_t i = 0; i < _state.spawns.length(); ++i) {
            if (_state.spawns[i].name == sp.name) {
                _state.spawns[i] = sp;
                found = true;
                break;
            }
        }
        if (!found) _state.spawns.push(sp);
        persistState();

        // Update instance.yml with spawn runtime pid / exitcode
        String instYmlPath = _opts.instanceDir + "/instance.yml";
        if (pathExists(instYmlPath)) {
            Array<String> visited;
            ParseError perr;
            ParsedManifest pm = Manifest::load(instYmlPath, visited, perr);
            if (perr.ok) {
                for (auto *dir : pm.directives) {
                    if (dir->kind == DirectiveKind::Spawn) {
                        auto *spDir = static_cast<DSpawn*>(dir);
                        String sname = spDir->name.isEmpty() ? "0" : spDir->name;
                        if (sname == sp.name) {
                            if (sp.status == SpawnStatus::Exited || sp.status == SpawnStatus::Failed) {
                                spDir->exitCode = sp.exitCode;
                            } else if (sp.pid > 0) {
                                spDir->pid = sp.pid;
                            }
                        }
                    }
                }
                String updatedYaml = Manifest::toYAML(pm.directives);
                Resource::LinuxFS fs;
                fs.write(instYmlPath, updatedYaml);
            }
        }
        logInfo("tau-instance: updated spawn ", sp.name, " (total spawns=", intStr(_state.spawns.length()), ")");
    };

    runOpts.onPoll = [this]() {
        _ipc.update();
        setupNetlink();
        processNetlinkEvents();
        reapChildren();
        checkReload();
    };

    runOpts.shouldStop = [this]() -> bool {
        return _termRequested;
    };

    runOpts.onOutput = [this](const String &spawnName, const char *data, size_t len) {
        _ipc.broadcast(spawnName, data, len);
    };

    runOpts.onIPEvent = [this](const String &iface, const String &ip) {
        Monitor::broadcast("IP", _opts.instanceName, _startupTime, "iface=" + iface + " ip=" + ip);
    };

    runOpts.onIPActionEvent = [this](const String &iface, const String &ip, const String &action) {
        Monitor::broadcast("IP", _opts.instanceName, _startupTime, "action=" + action + " iface=" + iface + " ip=" + ip);
    };

    runOpts.onIfaceEvent = [this](const String &iface, const String &state) {
        Monitor::broadcast("IFACE", _opts.instanceName, _startupTime, "iface=" + iface + " state=" + state);
    };

    _runner = new Runner(runOpts);
    Monitor::broadcast("START", _opts.instanceName, _startupTime);

    Array<String> allEntries = _opts.manifestPaths;
    if (allEntries.length() == 0 && !_opts.manifestPath.isEmpty()) {
        allEntries.push(_opts.manifestPath);
    }
    if (allEntries.length() == 0) {
        logError("tau-instance: no manifest entries specified");
        _state.status = InstanceStatus::Failed;
        persistState();
        _ipc.destroy();
        return 1;
    }

    String lastEntryPath = allEntries[allEntries.length() - 1];

    if (_opts.removeOnRead) {
        _opts.headless = true;
    }

    if (_opts.headless) {
        _watchedEntryPath = "";
    } else if (!_opts.instanceDir.isEmpty()) {
        mkdirP(_opts.instanceDir);
        if (_opts.copyYaml || _opts.globalMode) {
            String dest = _opts.instanceDir + "/manifest.yml";
            String content = fs.read(lastEntryPath);
            fs.write(dest, content);
            lastEntryPath = dest;
            if (_opts.watch) {
                _watchedEntryPath = dest;
            } else {
                _watchedEntryPath = "";
            }
        } else {
            String symlinkDest = _opts.instanceDir + "/manifest.yml";
            ::unlink(symlinkDest.c_str());
            ::symlink(lastEntryPath.c_str(), symlinkDest.c_str());
            if (_opts.watch) {
                _watchedEntryPath = lastEntryPath;
            } else {
                _watchedEntryPath = "";
            }
        }
    } else {
        if (_opts.watch) {
            _watchedEntryPath = lastEntryPath;
        } else {
            _watchedEntryPath = "";
        }
    }

    // Load templates (all except last entry)
    for (size_t i = 0; i < allEntries.length() - 1; ++i) {
        Array<String> visited;
        ParseError terr;
        auto *pm = new ParsedManifest();
        *pm = Xi::Move(Manifest::load(allEntries[i], visited, terr));
        if (!terr.ok) {
            logError("tau-instance: template error in ", allEntries[i], ": ", terr.message);
            delete pm;
            _state.status = InstanceStatus::Failed;
            persistState();
            _ipc.destroy();
            return 1;
        }
        _templates.push(pm);
    }

    // Load last entry
    Array<String> visited;
    ParseError lastErr;
    logInfo("tau-instance: STARTING MANIFEST PATH=", lastEntryPath);
    ParsedManifest lastParsed = Manifest::load(lastEntryPath, visited, lastErr);

    if (!lastErr.ok) {
        logError("tau-instance: manifest error: ", lastErr.message);
        _state.status = InstanceStatus::Failed;
        persistState();
        _ipc.destroy();
        return 1;
    }

    if (_opts.removeOnRead) {
        ::unlink(lastEntryPath.c_str());
    }

    DirectiveList initialDirectives = Manifest::applySlots(_templates, lastParsed);

    // Resolve all %... and $Vars pre-run, replace with hardcoded paths in instance manifest
    Map<String, String> runnerVars = _runner->currentEnv();
    String allArgsStr;
    for (size_t ai = 0; ai < _opts.args.length(); ++ai) {
        runnerVars[intStr((int)(ai + 1))] = _opts.args[ai];
        if (ai > 0) allArgsStr += " ";
        allArgsStr += _opts.args[ai];
    }
    runnerVars["args"] = allArgsStr;
    runnerVars["*"] = allArgsStr;
    runnerVars["@"] = allArgsStr;

    Manifest::resolveVariables(initialDirectives, runnerVars);
    _runner->setVars(runnerVars);
    if (!_opts.headless && !_opts.instanceDir.isEmpty()) {
        String resolvedYaml = Manifest::toYAML(initialDirectives);
        fs.write(_opts.instanceDir + "/instance.yml", resolvedYaml);
    }

    struct stat mst;
    if (!_opts.headless && !_watchedEntryPath.isEmpty() && ::stat(_watchedEntryPath.c_str(), &mst) == 0) {
#if defined(__APPLE__)
        _manifestMTimeNsec = (long long)mst.st_mtimespec.tv_sec * 1000000000LL + mst.st_mtimespec.tv_nsec;
#else
        _manifestMTimeNsec = (long long)mst.st_mtim.tv_sec * 1000000000LL + mst.st_mtim.tv_nsec;
#endif
    }
    String instYmlPath = _opts.instanceDir + "/instance.yml";
    if (!_opts.headless && ::stat(instYmlPath.c_str(), &mst) == 0) {
#if defined(__APPLE__)
        _instYamlMTimeNsec = (long long)mst.st_mtimespec.tv_sec * 1000000000LL + mst.st_mtimespec.tv_nsec;
#else
        _instYamlMTimeNsec = (long long)mst.st_mtim.tv_sec * 1000000000LL + mst.st_mtim.tv_nsec;
#endif
    }

    bool runOk = true;
    try {
        runOk = _runner->run(initialDirectives);
    } catch (const TauThrow &t) {
        logError("tau-instance: throw: ", t.message);
        runOk = false;
    }

    if (!runOk) {
        _state.status = InstanceStatus::Failed;
        persistState();
        Monitor::broadcast("STOP", _opts.instanceName, _startupTime, "exit_code=1");
        _ipc.destroy();
        return 1;
    }


    while (!shouldExit()) {
        struct pollfd pfds[64];
        nfds_t npfds = 0;

        pfds[npfds].fd      = _sigPipe[0];
        pfds[npfds].events  = POLLIN;
        pfds[npfds].revents = 0;
        npfds++;

        if (_netlinkFd < 0) setupNetlink();
        if (_netlinkFd >= 0 && npfds < 64) {
            pfds[npfds].fd      = _netlinkFd;
            pfds[npfds].events  = POLLIN;
            pfds[npfds].revents = 0;
            npfds++;
        }

        if (_runner) {
            for (auto *sp : _runner->activeSpawns()) {
                if (sp->ptyMaster >= 0 && !sp->waited && npfds < 64) {
                    pfds[npfds].fd      = sp->ptyMaster;
                    pfds[npfds].events  = POLLIN;
                    pfds[npfds].revents = 0;
                    npfds++;
                }
            }
        }

        _ipc.fillPollFds(pfds, npfds, 64);

        int ready = ::poll(pfds, npfds, 100);
        reapChildren();
        processNetlinkEvents();

        _ipc.update();

        if (ready > 0) {
            if (_runner) {
                size_t idx = 1;
                for (auto *sp : _runner->activeSpawns()) {
                    if (sp->ptyMaster >= 0 && !sp->waited && idx < npfds) {
                        if (pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                            char buf[4096];
                            ssize_t n = ::read(sp->ptyMaster, buf, sizeof(buf));
                            if (n > 0) {
                                sp->outRing.write(buf, (size_t)n);
                                _ipc.broadcast(sp->name, buf, (size_t)n);
                            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                                ::close(sp->ptyMaster);
                                sp->ptyMaster = -1;
                            }
                        }
                        ++idx;
                    }
                }
            }
        }


        _ipc.update();
        checkReload();

        if (_termRequested) {
            gracefulShutdown();
            break;
        }
    }

    if (!_termRequested) {
        _runner->waitAll();
    }

    bool wasFrozen = (_state.status == InstanceStatus::Frozen);
    if (!wasFrozen) {
        _state.status = runOk ? InstanceStatus::Stopped : InstanceStatus::Failed;
    }
    persistState();

    Monitor::broadcast("STOP", _opts.instanceName, _startupTime, "exit_code=" + intStr(runOk ? 0 : 1));

    _ipc.destroy();
    delete _runner;
    _runner = nullptr;

    // Remove instance cgroup on clean exit
    if (!wasFrozen) {
        Cgroup::removeInstance(_opts.instanceName);
    }

    // Remove instance directory if not explicitly preserved (and not frozen!)
    if (!wasFrozen) {
        (void)removeDirRecursive(_opts.instanceDir);
    }

    Namespace::shutdownHostHelper();

    return runOk ? 0 : 1;
}

void SpawnProcess::checkReload() {
    if (_opts.headless || !_runner) return;

    String fileToReload;
    long long curNsec = 0;
    struct stat mst;

    // Check watched source entry path
    if (!_watchedEntryPath.isEmpty() && ::stat(_watchedEntryPath.c_str(), &mst) == 0) {
#if defined(__APPLE__)
        curNsec = (long long)mst.st_mtimespec.tv_sec * 1000000000LL + mst.st_mtimespec.tv_nsec;
#else
        curNsec = (long long)mst.st_mtim.tv_sec * 1000000000LL + mst.st_mtim.tv_nsec;
#endif
        if (curNsec > 0 && _manifestMTimeNsec > 0 && curNsec > (_manifestMTimeNsec + 50000000LL)) {
            fileToReload = _watchedEntryPath;
            _manifestMTimeNsec = curNsec;
        }
    }

    // Also check instanceDir/instance.yml if different
    String instYml = _opts.instanceDir + "/instance.yml";
    if (fileToReload.isEmpty() && instYml != _watchedEntryPath && ::stat(instYml.c_str(), &mst) == 0) {
#if defined(__APPLE__)
        long long instNsec = (long long)mst.st_mtimespec.tv_sec * 1000000000LL + mst.st_mtimespec.tv_nsec;
#else
        long long instNsec = (long long)mst.st_mtim.tv_sec * 1000000000LL + mst.st_mtim.tv_nsec;
#endif
        if (instNsec > 0 && _instYamlMTimeNsec > 0 && instNsec > (_instYamlMTimeNsec + 50000000LL)) {
            fileToReload = instYml;
            _instYamlMTimeNsec = instNsec;
        }
    }

    if (!fileToReload.isEmpty()) {
        Array<String> reloadVisited;
        ParseError reloadErr;
        ParsedManifest reloaded = Manifest::load(fileToReload, reloadVisited, reloadErr);
        if (reloadErr.ok) {
            DirectiveList reloadedDirectives = (fileToReload == instYml)
                ? Manifest::cloneDirectives(reloaded.directives)
                : Manifest::applySlots(_templates, reloaded);
            Map<String, String> liveVars = _runner->currentEnv();
            Manifest::resolveVariables(reloadedDirectives, liveVars);
            String updatedYaml = Manifest::toYAML(reloadedDirectives);
            Resource::LinuxFS fs;
            fs.write(instYml, updatedYaml);

            struct stat rmst;
            if (::stat(instYml.c_str(), &rmst) == 0) {
#if defined(__APPLE__)
                _instYamlMTimeNsec = (long long)rmst.st_mtimespec.tv_sec * 1000000000LL + rmst.st_mtimespec.tv_nsec;
#else
                _instYamlMTimeNsec = (long long)rmst.st_mtim.tv_sec * 1000000000LL + rmst.st_mtim.tv_nsec;
#endif
            }

            try {
                _runner->run(reloadedDirectives);
            } catch (const TauThrow &t) {
                logError("tau-instance: reload throw: ", t.message);
            }
            for (auto *d : reloadedDirectives) delete d;
        }
    }
}

} // namespace Tau


