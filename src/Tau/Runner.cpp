/**
 * @file Runner.cpp
 * @brief Directive executor — walks a DirectiveList and carries out each step.
 */

#include <Tau/Runner.hpp>
#include <Tau/Manifest.hpp>
#include <Tau/Cgroup.hpp>
#include <Tau/GitHub.hpp>
#include <Tau/Config.hpp>
#include <Tau/Util.hpp>

#include <Resource/File.hpp>
#include <Encoding/Regex.hpp>
#include <Security/Crypto.hpp>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <dirent.h>


extern "C" char **environ;

namespace Tau {

using namespace Collection;
using namespace Xi;

// ─── Constructor / Destructor ─────────────────────────────────────────────────

Runner::Runner(const RunnerOptions &opts) : _opts(opts) {
    // Seed variable map from environment, discarding empty variables
    for (char **ep = environ; ep && *ep; ++ep) {
        String entry(*ep);
        long long eq = entry.find("=");
        if (eq >= 0) {
            String key = entry.substring(0, (size_t)eq);
            String val = entry.substring((size_t)eq + 1);
            if (!key.isEmpty() && !val.isEmpty()) {
                _vars[key] = val;
            }
        }
    }
}


Runner::~Runner() {
    for (auto *sp : _spawns) {
        if (sp->pid > 0 && !sp->waited)
            ::kill(sp->pid, SIGKILL);
        sp->outRing.close();
        sp->errRing.close();
        delete sp;
    }
    _spawns.clear();

    // Unmount all tracked mounts in reverse order
    for (long long i = (long long)_mounts.length() - 1; i >= 0; --i) {
        String m = _mounts[(size_t)i];
        if (m.isEmpty()) continue;
        String fusermountCmd = String("fusermount -u -z '") + m + "' >/dev/null 2>&1";
        (void)::system(fusermountCmd.c_str());
        String umountCmd = String("umount -l '") + m + "' >/dev/null 2>&1";
        (void)::system(umountCmd.c_str());
    }

    // Kill any fuse2fs daemons associated with this runner's image paths
    for (size_t i = 0; i < _imagePaths.length(); ++i) {
        if (!_imagePaths[i].isEmpty()) {
            String killFuse = String("pkill -9 -f '") + _imagePaths[i] + "' >/dev/null 2>&1";
            (void)::system(killFuse.c_str());
        }
    }

    // Clean up temporary image mount directories
    for (size_t i = 0; i < _tempDirs.length(); ++i) {
        String d = _tempDirs[i];
        if (!d.isEmpty()) {
            removeDirRecursive(d);
        }
    }
}



// ─── Main run ─────────────────────────────────────────────────────────────────

bool Runner::run(const DirectiveList &directives) {
    if (!_opts.manifestPath.isEmpty()) {
        long long sl = rfind(_opts.manifestPath, '/');
        if (sl >= 0) {
            String dir = _opts.manifestPath.substring(0, (size_t)sl);
            if (!dir.isEmpty() && dir.find("/instances/") < 0) {
                (void)::chdir(dir.c_str());
            }
        }
    }


    bool ok = true;
    for (size_t i = 0; i < directives.length(); ++i) {
        auto *d = directives[i];
        if (!d) continue;
        logInfo("tau/run: index=", intStr((int)i), " kind=", intStr((int)d->kind));
        bool result = true;

        switch (d->kind) {
            case DirectiveKind::Wait:      result = execWait     (*static_cast<DWait*>(d));      break;
            case DirectiveKind::WaitExit:  result = execWaitExit (*static_cast<DWaitExit*>(d));  break;
            case DirectiveKind::Spawn:     result = execSpawn    (*static_cast<DSpawn*>(d));     break;
            case DirectiveKind::Memory:    result = execMemory   (*static_cast<DMemory*>(d));    break;
            case DirectiveKind::CPUSet:    result = execCPUSet   (*static_cast<DCPUSet*>(d));    break;
            case DirectiveKind::CPU:       result = execCPU      (*static_cast<DCPU*>(d));       break;
            case DirectiveKind::Chroot:    result = execChroot   (*static_cast<DChroot*>(d));    break;
            case DirectiveKind::Isolate:   result = execIsolate  (*static_cast<DIsolate*>(d));   break;
            case DirectiveKind::Veth:      result = execVeth     (*static_cast<DVeth*>(d));      break;

            case DirectiveKind::IP:        result = execIP       (*static_cast<DIP*>(d));        break;
            case DirectiveKind::NewNet:    result = execNewNet   (*static_cast<DNewNet*>(d));    break;
            case DirectiveKind::Forward:   result = execForward  (*static_cast<DForward*>(d));   break;
            case DirectiveKind::Macvlan:   result = execMacvlan  (*static_cast<DMacvlan*>(d));   break;
            case DirectiveKind::IPVlan:    result = execIPVlan   (*static_cast<DIPVlan*>(d));    break;
            case DirectiveKind::Bridge:    result = execBridge   (*static_cast<DBridge*>(d));    break;
            case DirectiveKind::Mount:     result = execMount    (*static_cast<DMount*>(d));     break;

            case DirectiveKind::Copy:      result = execCopy     (*static_cast<DCopy*>(d));      break;
            case DirectiveKind::Unlink:    result = execUnlink   (*static_cast<DUnlink*>(d));    break;
            case DirectiveKind::Mkdir:     result = execMkdir    (*static_cast<DMkdir*>(d));     break;
            case DirectiveKind::Symlink:   result = execSymlink  (*static_cast<DSymlink*>(d));   break;
            case DirectiveKind::Image:     result = execImage    (*static_cast<DImage*>(d));     break;
            case DirectiveKind::Local:     result = execLocal    (*static_cast<DLocal*>(d));     break;
            case DirectiveKind::Git:       result = execGit      (*static_cast<DGit*>(d));       break;
            case DirectiveKind::GHRelease: result = execGHRelease(*static_cast<DGHRelease*>(d));break;
            case DirectiveKind::Manifest:  result = execManifest (*static_cast<DManifest*>(d)); break;
            case DirectiveKind::Or:        result = execOr       (*static_cast<DOr*>(d));        break;
            case DirectiveKind::And:       result = execAnd      (*static_cast<DAnd*>(d));       break;
            case DirectiveKind::Nand:      result = execNand     (*static_cast<DNand*>(d));      break;
            case DirectiveKind::Nor:       result = execNor      (*static_cast<DNor*>(d));       break;
            case DirectiveKind::Xor:       result = execXor      (*static_cast<DXor*>(d));       break;
            case DirectiveKind::Var:
            case DirectiveKind::RegVar:
            case DirectiveKind::RallVar:   result = execVar      (*static_cast<DVarBase*>(d));   break;
            case DirectiveKind::NewVar:    result = execNewVar   (*static_cast<DNewVar*>(d));    break;
            case DirectiveKind::RmVar:     result = execRmVar    (*static_cast<DRmVar*>(d));     break;
            case DirectiveKind::Fail:      result = execFail     (*static_cast<DFail*>(d));      break;

            case DirectiveKind::Throw:     result = execThrow    (*static_cast<DThrow*>(d));     break;
            case DirectiveKind::Bin:       result = execBin      (*static_cast<DBin*>(d));       break;
            case DirectiveKind::BinDir:    result = execBinDir   (*static_cast<DBinDir*>(d));    break;
            default: result = true; break;
        }

        if (!result) {
            ::fprintf(stderr, "tau/run: directive kind=%d FAILED\n", (int)d->kind);
            logError("tau/run: directive kind=", (int)d->kind, " FAILED");
            ok = false;
        }
    }
    return ok;
}

// ─── waitAll ──────────────────────────────────────────────────────────────────

void Runner::waitAll() {
    for (auto *sp : _spawns) {
        if (!sp->waited) waitSpawn(sp);
    }
}

// ─── signalSpawn ──────────────────────────────────────────────────────────────

bool Runner::signalSpawn(const String &name, int sig) {
    for (auto *sp : _spawns) {
        if (sp->name == name && sp->pid > 0 && !sp->waited) {
            ::kill(sp->pid, sig);
            return true;
        }
    }
    return false;
}

bool Runner::resizeSpawn(const String &name, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return false;
    struct winsize ws = {};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    bool found = false;
    for (auto *sp : _spawns) {
        if (name.isEmpty() || name == "0" || name == "all" || sp->name == name) {
            if (sp->ptyMaster >= 0) {
                ::ioctl(sp->ptyMaster, TIOCSWINSZ, &ws);
                found = true;
            }
            if (sp->pid > 0 && !sp->waited) {
                ::kill(sp->pid, SIGWINCH);
            }
        }
    }
    return found;
}


// ─── Timing ───────────────────────────────────────────────────────────────────

bool Runner::execWait(const DWait &d) {
    if (d.seconds > 0) ::usleep((useconds_t)(d.seconds * 1000000.0));
    return true;
}

bool Runner::execWaitExit(const DWaitExit &d) {
    // Run the child block, tracking all spawns created inside
    size_t spawnsBefore = _spawns.length();
    run(d.children);

    // Collect spawns started by this block
    bool allOk = true;
    for (size_t i = spawnsBefore; i < _spawns.length(); ++i) {
        auto *sp = _spawns[i];
        if (!sp->waited) waitSpawn(sp);
        if (sp->exitCode != 0) allOk = false;
    }
    return allOk;
}

// ─── Spawn execution ──────────────────────────────────────────────────────────

bool Runner::execSpawn(const DSpawn &d) {
    if (!d.name.isEmpty()) {
        for (auto *existing : _spawns) {
            if (existing->name == d.name && existing->pid > 0 && !existing->waited) {
                // Spawn is already active and running — preserve it!
                return true;
            }
        }
    }

    bool allocPTY = !_opts.storeMode && d.waitForExit;

    ActiveSpawn *sp = launchSpawn(d, allocPTY);
    if (!sp) return false;

    notifySpawnChange(*sp);

    if (d.waitForExit) {
        waitSpawn(sp);
        return sp->exitCode == 0;
    }
    return true;
}






ActiveSpawn *Runner::launchSpawn(const DSpawn &d, bool allocPTY) {
    String spawnName = allocSpawnName(d.name);
    String command   = interp(d.command);

    // Prepare ring buffer paths
    String ringDir;
    if (!_opts.instanceDir.isEmpty()) {
        ringDir = _opts.instanceDir + "/spawns/" + spawnName;
        Resource::LinuxFS fs;
        fs.mkdir(ringDir);
    }

    auto *sp = new ActiveSpawn();
    sp->name    = spawnName;
    sp->command = command;

    if (!ringDir.isEmpty()) {
        sp->outRing.open(ringDir + "/out.ring", true);
        sp->errRing.open(ringDir + "/err.ring", true);
    }

    int masterFd = -1;

    // Fork
    if (allocPTY) {
        struct winsize ws = {};
        struct winsize *pws = nullptr;
        if (_opts.cols > 0 && _opts.rows > 0) {
            ws.ws_col = (unsigned short)_opts.cols;
            ws.ws_row = (unsigned short)_opts.rows;
            pws = &ws;
        }

        // Allocate a PTY pair
        pid_t pid = ::forkpty(&masterFd, nullptr, nullptr, pws);
        if (pid < 0) {
            logError("tau: forkpty failed: ", ::strerror(errno));
            delete sp;
            return nullptr;
        }
        if (pid == 0) {

            // Close all inherited fds above stderr so spawned processes
            // don't inherit the IPC socket or other parent-only handles.
            // Use close_range if available (Linux 5.9+), else fall back.
#ifdef __NR_close_range
            ::syscall(__NR_close_range, 3, ~0U, 0);
#else
            DIR *d = ::opendir("/proc/self/fd");
            if (d) {
                int dfd = ::dirfd(d);
                struct dirent *e;
                while ((e = ::readdir(d))) {
                    int f = ::atoi(e->d_name);
                    if (f > 2 && f != dfd) ::close(f);
                }
                ::closedir(d);
            }
#endif
            if (_isoCtx.hasPidNS || _isoCtx.isIsolated) {
                if (::unshare(CLONE_NEWPID) == 0) {
                    pid_t p = ::fork();
                    if (p > 0) {
                        int st;
                        ::waitpid(p, &st, 0);
                        ::_exit(WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 0));
                    }
                }
            }

            if (_isoCtx.isIsolated || _isoCtx.hasUserNS) {
                (void)::setresgid(0, 0, 0);
                (void)::setresuid(0, 0, 0);
                Namespace::setupContainerCaps();
                ::setenv("USER", "root", 1);
                ::setenv("LOGNAME", "root", 1);
                ::setenv("HOME", "/root", 1);
            }

            // Apply chroot inside child if configured
            if (_isoCtx.hasChrootd && !_isoCtx.chrootPath.isEmpty()) {
                if (::chroot(_isoCtx.chrootPath.c_str()) == 0) {
                    (void)::chdir("/");
                }
                ::setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin", 1);
                ::setenv("HOME", "/root", 1);
                ::setenv("USER", "root", 1);
            } else {
                ::setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin", 0);
            }

            if (_isoCtx.hasMountNS && (_isoCtx.hasPidNS || _isoCtx.isIsolated)) {
                (void)::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr);
            }

            // Export current runner variables to child environment
            for (auto &entry : _vars) {
                if (!entry.key.isEmpty() && !entry.value.isEmpty()) {
                    ::setenv(entry.key.c_str(), entry.value.c_str(), 1);
                }
            }

            ::setenv("TERM", "xterm-256color", 1);
            ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            ::_exit(127);
        }

        sp->pid      = pid;
        sp->ptyMaster = masterFd;
        ::fcntl(masterFd, F_SETFL, O_NONBLOCK);
    } else {
        // Standard fork with pipes for stdout/stderr
        int outPipe[2], errPipe[2];
        ::pipe(outPipe);
        ::pipe(errPipe);

        pid_t pid = ::fork();
        if (pid < 0) {
            logError("tau: fork failed: ", ::strerror(errno));
            ::close(outPipe[0]); ::close(outPipe[1]);
            ::close(errPipe[0]); ::close(errPipe[1]);
            delete sp;
            return nullptr;
        }

        if (pid == 0) {
            // Child: wire up stdout/stderr to the pipes first
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::dup2(errPipe[1], STDERR_FILENO);

            // Now close ALL fds above stderr — this clears the IPC socket
            // and any other parent-only file handles so they are NOT inherited.
#ifdef __NR_close_range
            ::syscall(__NR_close_range, 3, ~0U, 0);
#else
            DIR *d = ::opendir("/proc/self/fd");
            if (d) {
                int dfd = ::dirfd(d);
                struct dirent *e;
                while ((e = ::readdir(d))) {
                    int f = ::atoi(e->d_name);
                    if (f > 2 && f != dfd) ::close(f);
                }
                ::closedir(d);
            }
#endif

            if (_isoCtx.hasPidNS || _isoCtx.isIsolated) {
                if (::unshare(CLONE_NEWPID) == 0) {
                    pid_t p = ::fork();
                    if (p > 0) {
                        int st;
                        ::waitpid(p, &st, 0);
                        ::_exit(WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 0));
                    }
                }
            }

            if (_isoCtx.isIsolated || _isoCtx.hasUserNS) {
                (void)::setresgid(0, 0, 0);
                (void)::setresuid(0, 0, 0);
                Namespace::setupContainerCaps();
                ::setenv("USER", "root", 1);
                ::setenv("LOGNAME", "root", 1);
                ::setenv("HOME", "/root", 1);
            }

            if (_isoCtx.hasChrootd && !_isoCtx.chrootPath.isEmpty()) {
                if (::chroot(_isoCtx.chrootPath.c_str()) == 0) {
                    (void)::chdir("/");
                }
                ::setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin", 1);
                ::setenv("HOME", "/root", 1);
                ::setenv("USER", "root", 1);
            } else {
                ::setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin", 0);
            }

            if (_isoCtx.hasMountNS && (_isoCtx.hasPidNS || _isoCtx.isIsolated)) {
                (void)::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr);
            }

            // Export current runner variables to child environment
            for (auto &entry : _vars) {
                if (!entry.key.isEmpty() && !entry.value.isEmpty()) {
                    ::setenv(entry.key.c_str(), entry.value.c_str(), 1);
                }
            }

            ::setenv("TERM", "xterm-256color", 1);
            ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            ::_exit(127);
        }




        sp->pid = pid;
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        ::fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
        ::fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

        // Store fd info inside ActiveSpawn by repurposing ptyMaster field
        // (we abuse it as the out pipe fd; err pipe is ptyMaster+1 conceptually)
        // We'll manage them inline in waitSpawn using a poll loop
        sp->ptyMaster = outPipe[0];
        // errPipe[0]: store in a side-channel; keep errPipe in globals
        // For simplicity, wrap into a lambda that drains both — store fds
        // We track errFd via the ring directly:
        // The -ptyMaster trick: use negative for pipe-mode
        // Actually let's store outFd and errFd cleanly:
        // We'll store errPipe[0] right after ptyMaster in a new field.
        // Since ActiveSpawn doesn't have errFd, add inline:
        sp->ptyMaster = outPipe[0];
        // We track errPipe via a global: just close on death
        // For now, track outPipe only; stderr goes to host stderr
        ::dup2(errPipe[0], STDERR_FILENO); // mirror to host stderr for visibility
        ::close(errPipe[0]);
    }

    // Move into shared instance cgroup if cgroup limits were requested
    if (_isoCtx.hasCgroupLimits && !_opts.instanceName.isEmpty()) {
        if (Cgroup::create(_opts.instanceName)) {
            if (_isoCtx.hasMemory) Cgroup::setMemory(_opts.instanceName, "", _isoCtx.memoryBytes);
            if (_isoCtx.hasCPU)    Cgroup::setCPU(_opts.instanceName, "", _isoCtx.cpuQuota);
            if (_isoCtx.hasCPUSet) Cgroup::setCPUSet(_opts.instanceName, "", _isoCtx.cpusetCPUs);
            (void)Cgroup::addPID(_opts.instanceName, "", sp->pid);
        } else {
            logWarn("tau: cgroup limit ignored (requires cgroup v2 delegation or root)");
        }
    }


    _spawns.push(sp);
    return sp;
}

// ─── waitSpawn ────────────────────────────────────────────────────────────────

void Runner::waitSpawn(ActiveSpawn *sp) {
    if (!sp || sp->waited || sp->pid < 0) return;

    uint8_t detachByte = 0x02; // default ctrl+b
    if (_opts.detachKey.length() >= 6 && _opts.detachKey.startsWith("ctrl+")) {
        char ch = _opts.detachKey[5];
        if (ch >= 'a' && ch <= 'z') detachByte = (uint8_t)(ch - 'a' + 1);
        if (ch >= 'A' && ch <= 'Z') detachByte = (uint8_t)(ch - 'A' + 1);
    }

    struct termios orig_termios, raw_termios;
    bool is_tty = false;
    if (_opts.attachStdin && sp->ptyMaster >= 0) {
        if (::tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
            is_tty = true;
            raw_termios = orig_termios;
            ::cfmakeraw(&raw_termios);
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios);
        }
    }

    // Poll the PTY/pipe and drain into ring buffers while waiting
    struct pollfd fds[2];
    int nfds = 0;

    if (sp->ptyMaster >= 0) {
        fds[0].fd     = sp->ptyMaster;
        fds[0].events = POLLIN;
        nfds = 1;

        if (_opts.attachStdin) {
            fds[1].fd     = STDIN_FILENO;
            fds[1].events = POLLIN;
            nfds = 2;
        }
    }

    char buf[4096];
    bool detached = false;

    while (!sp->waited && !detached) {
        if (_opts.onPoll) _opts.onPoll();

        // If stop was requested externally, SIGKILL all spawns and break out
        if (_opts.shouldStop && _opts.shouldStop()) {
            for (auto *s : _spawns)
                if (s->pid > 0 && !s->waited) ::kill(s->pid, SIGKILL);
            // drain waitpid for this spawn
            int st;
            ::waitpid(sp->pid, &st, 0);
            sp->waited = true;
            sp->exitCode = -SIGKILL;
            break;
        }

        if (nfds > 0) ::poll(fds, nfds, 10);

        if (sp->ptyMaster >= 0 && (nfds == 0 || (fds[0].revents & (POLLIN | POLLHUP | POLLERR)))) {
            ssize_t n = ::read(sp->ptyMaster, buf, sizeof(buf));
            if (n > 0) {
                sp->outRing.write(buf, (size_t)n);
                if (_opts.attachStdin)
                    ::write(STDOUT_FILENO, buf, (size_t)n);
                if (_opts.onOutput)
                    _opts.onOutput(sp->name, buf, (size_t)n);
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(sp->ptyMaster);
                sp->ptyMaster = -1;
                fds[0].fd = -1;
            }
        }


        if (_opts.attachStdin && sp->ptyMaster >= 0 && nfds >= 2 && (fds[1].revents & POLLIN)) {
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                for (ssize_t i = 0; i < n; ++i) {
                    if ((uint8_t)buf[i] == detachByte) {
                        detached = true;
                        break;
                    }
                }
                if (detached) {
                    break;
                }
                ::write(sp->ptyMaster, buf, (size_t)n);
            }
        }

        int status;
        pid_t r = ::waitpid(sp->pid, &status, WNOHANG);
        if (r == sp->pid || r < 0) {
            sp->waited = true;
            if (r == sp->pid) {
                if (WIFEXITED(status))
                    sp->exitCode = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    sp->exitCode = -WTERMSIG(status);
            }
            break;
        }
    }

    if (is_tty) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }

    notifySpawnChange(*sp);

    // Cleanup cgroup
    if (!_opts.instanceName.isEmpty()) {
        Cgroup::remove(_opts.instanceName, sp->name);
    }
}

// ─── Resource limits ──────────────────────────────────────────────────────────

bool Runner::execMemory(const DMemory &d) {
    _isoCtx.hasMemory       = true;
    _isoCtx.memoryBytes     = d.bytes > 0 ? d.bytes : Manifest::parseSize(d.raw);
    _isoCtx.hasCgroupLimits = true;
    if (!_opts.instanceName.isEmpty() && Cgroup::create(_opts.instanceName)) {
        Cgroup::setMemory(_opts.instanceName, "", _isoCtx.memoryBytes);
    }
    return true;
}

bool Runner::execCPUSet(const DCPUSet &d) {
    _isoCtx.hasCPUSet       = true;
    _isoCtx.cpusetCPUs     = d.cpus;
    _isoCtx.hasCgroupLimits = true;
    if (!_opts.instanceName.isEmpty() && Cgroup::create(_opts.instanceName)) {
        Cgroup::setCPUSet(_opts.instanceName, "", _isoCtx.cpusetCPUs);
    }
    return true;
}

bool Runner::execCPU(const DCPU &d) {
    _isoCtx.hasCPU          = true;
    _isoCtx.cpuQuota        = d.quota;
    _isoCtx.hasCgroupLimits = true;
    if (!_opts.instanceName.isEmpty() && Cgroup::create(_opts.instanceName)) {
        Cgroup::setCPU(_opts.instanceName, "", _isoCtx.cpuQuota);
    }
    return true;
}


// ─── Isolation directives ─────────────────────────────────────────────────────

bool Runner::execChroot(const DChroot &d) {
    String path = resolvePath(d.path);
    _isoCtx.hasChrootd = true;
    _isoCtx.chrootPath = path;
    return true;
}

bool Runner::execIsolate(const DIsolate &d) {
    if (!d.enable) return true;
    if (_isoCtx.isIsolated) return true;
    _vars["USER"] = "root";
    _vars["LOGNAME"] = "root";
    _vars["HOME"] = "/root";
    return Namespace::isolate(_isoCtx);
}



bool Runner::execVeth(const DVeth &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    return Namespace::createVeth(interp(d.hostName), interp(d.peerName));
}

bool Runner::execIP(const DIP &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    String cmd = String("ip ") + interp(d.command);
    return ::system(cmd.c_str()) == 0;
}


bool Runner::execNewNet(const DNewNet &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    bool ok = Namespace::enterNet(d.allowList, _isoCtx);
    if (ok && _opts.onIPEvent) {
        _opts.onIPEvent("lo", "127.0.0.1");
    }
    return ok;
}

bool Runner::execForward(const DForward &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    DForward resolved = d;
    resolved.target     = interp(d.target);
    resolved.source     = interp(d.source);
    resolved.port       = interp(d.port);
    resolved.sourcePort = interp(d.sourcePort);
    resolved.sourceIP   = interp(d.sourceIP);
    resolved.ip         = interp(d.ip);
    resolved.protocol   = interp(d.protocol);

    bool ok = Namespace::setupForwarding(resolved);
    if (ok && _opts.onIPEvent) {
        if (!resolved.ip.isEmpty()) {
            _opts.onIPEvent(resolved.target, resolved.ip);
        } else if (!resolved.sourceIP.isEmpty() && resolved.sourceIP != "*") {
            _opts.onIPEvent(resolved.source, resolved.sourceIP);
        }
    }
    return ok;
}

bool Runner::execMacvlan(const DMacvlan &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    return Namespace::createMacvlan(interp(d.parent), interp(d.name), interp(d.mode), interp(d.mac));
}

bool Runner::execIPVlan(const DIPVlan &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    return Namespace::createIPVlan(interp(d.parent), interp(d.name), interp(d.mode));
}

bool Runner::execBridge(const DBridge &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    String name = interp(d.name);
    String addr = interp(d.address);
    bool ok = Namespace::createBridge(name, interp(d.attach), addr);
    if (ok && !addr.isEmpty() && _opts.onIPEvent) {
        _opts.onIPEvent(name, addr);
    }
    return ok;
}



// ─── Filesystem directives ────────────────────────────────────────────────────

bool Runner::execMount(const DMount &d) {
    // Any mount directive implies a user+mount namespace — enter it if not already done
    if (!Namespace::enterUser(_isoCtx)) {
        logError("tau: execMount: cannot enter user namespace");
        return false;
    }
    String target = resolvePath(d.target);
    bool targetExisted = pathExists(target);
    if (!targetExisted) {
        mkdirP(target);
        _tempDirs.push(target);
    }
    _mounts.push(target);

    String src = d.source;
    String resolvedSrc;
    if (!d.work.isEmpty() && src.find(":") >= 0) {
        Array<String> parts = src.split(":");
        for (size_t i = 0; i < parts.length(); ++i) {
            String p = interp(parts[i].trim());
            if (!p.isEmpty()) {
                String rp = resolvePath(p);
                if (!resolvedSrc.isEmpty()) resolvedSrc += ":";
                resolvedSrc += rp;
            }
        }
    } else {
        resolvedSrc = resolvePath(src);
    }

    return Namespace::doMount(resolvedSrc,
                              d.work.isEmpty() ? "" : resolvePath(d.work),
                              target,
                              d.read, d.write);
}


bool Runner::execCopy(const DCopy &d) {
    String src = resolvePath(d.source);
    String dst = resolvePath(d.target);
    return copyRecursive(src, dst);
}

bool Runner::execUnlink(const DUnlink &d) {
    String path = resolvePath(d.path);
    int rc = ::unlink(path.c_str());
    logInfo("tau/execUnlink path='", path, "' rc=", rc, " errno=", errno);
    if (rc != 0 && errno != ENOENT) {
        logError("tau: unlink ", path, ": ", ::strerror(errno));
        return false;
    }
    return true;
}

bool Runner::execMkdir(const DMkdir &d) {
    String path = resolvePath(d.path);
    if (d.path.startsWith("%") || d.path.startsWith("/tmp/tau-")) {
        _tempDirs.push(path);
    }
    return mkdirP(path);
}


bool Runner::execSymlink(const DSymlink &d) {
    String src = resolvePath(d.source);
    String dst = resolvePath(d.target);
    if (::symlink(src.c_str(), dst.c_str()) != 0 && errno != EEXIST) {
        logError("tau: symlink ", src, " to ", dst, ": ", ::strerror(errno));
        return false;
    }
    return true;
}


bool Runner::execImage(const DImage &d) {
    if (!Namespace::enterUser(_isoCtx)) {
        logError("tau: execImage: cannot enter user namespace");
        return false;
    }

    String imagePath = resolvePath(d.path);


    String baseDir = "/tmp/tau_img_" + hexEncode(Security::hash(imagePath, 8));
    String imgDir  = baseDir + "/img";
    String mergedDir = d.target.isEmpty() ? baseDir + "/merged" : resolvePath(d.target);

    // If already mounted, preserve existing mount!
    for (size_t i = 0; i < _mounts.length(); ++i) {
        if (_mounts[i] == mergedDir || _mounts[i] == imgDir) {
            _activeImgDir = baseDir;
            return true;
        }
    }

    mkdirP(baseDir);
    _tempDirs.push(baseDir);
    _imagePaths.push(imagePath);

    _activeImgDir = baseDir;
    bool exists = pathExists(imagePath);


    if (!exists) {
        if (!d.create) {
            logError("tau: image file does not exist: ", imagePath);
            _activeImgDir = "";
            return false;
        }
        int fd = ::open(imagePath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            logError("tau: failed to create image file: ", imagePath);
            _activeImgDir = "";
            return false;
        }
        ::close(fd);
    }

    if (!d.size.isEmpty()) {
        size_t sizeBytes = Manifest::parseSize(d.size);
        if (sizeBytes > 0)
            ::truncate(imagePath.c_str(), (off_t)sizeBytes);
    }

    if (d.type == "ext4" || d.type.isEmpty()) {
        String imgDir  = baseDir + "/img";
        String workDir = d.work.isEmpty() ? baseDir + "/work" : resolvePath(d.work);
        mkdirP(imgDir);
        mkdirP(workDir);

        String mergedDir = d.target.isEmpty() ? baseDir + "/merged" : resolvePath(d.target);
        bool targetExisted = pathExists(mergedDir);
        mkdirP(mergedDir);
        if (!targetExisted || d.target.isEmpty() || mergedDir.startsWith("/tmp/tau_") || mergedDir.startsWith("/tmp/tau-")) {
            _tempDirs.push(mergedDir);
        }

        if (!exists && !d.size.isEmpty()) {
            String formatCmd = String("mkfs.ext4 -F '") + imagePath + "' >/dev/null 2>&1";
            (void)::system(formatCmd.c_str());
        }

        bool mounted = false;
        if (::system("which fuse2fs >/dev/null 2>&1") == 0) {
            if (exists) {
                String fsckCmd = String("e2fsck -fy '") + imagePath + "' >/dev/null 2>&1";
                (void)::system(fsckCmd.c_str());
            }

            String fuseCmd = String("fuse2fs -o fakeroot,rw '") + imagePath +
                             "' '" + imgDir + "' >/dev/null 2>&1";
            mounted = (::system(fuseCmd.c_str()) == 0);
        }
        if (!mounted) {
            String mountCmd = String("mount -o loop '") + imagePath +
                              "' '" + imgDir + "' >/dev/null 2>&1";
            mounted = (::system(mountCmd.c_str()) == 0);
        }

        if (mounted) {
            _mounts.push(imgDir);
        }

        String lower = d.lower;
        if (!lower.isEmpty()) {
            Array<String> lowerParts = lower.split(":");
            String resolvedLower;
            for (size_t i = 0; i < lowerParts.length(); ++i) {
                String lp = interp(lowerParts[i].trim());
                if (!lp.isEmpty()) {
                    String rlp = resolvePath(lp);
                    if (!resolvedLower.isEmpty()) resolvedLower += ":";
                    resolvedLower += rlp;
                }
            }
            _mounts.push(mergedDir);
            Namespace::doMount(resolvedLower, workDir, mergedDir, d.read, d.write, imgDir);
        } else if (mounted) {
            _mounts.push(mergedDir);
            Namespace::doMount(imgDir, "", mergedDir, d.read, d.write);
        }
    }

    _activeImgDir = "";
    return true;
}







// ─── Dependency directives ────────────────────────────────────────────────────

bool Runner::execLocal(const DLocal &d) {
    String src = resolvePath(d.source);
    String dst = resolvePath(d.target.isEmpty() ? d.source : d.target);

    bool ok = false;
    Resource::LinuxFS fs;

    if (d.store) {
        // Copy source into store directory and symlink/copy to target
        String storeDir = Config::storePath() + "/local_" +
                          hexEncode(Security::hash(src, 8));
        fs.mkdir(storeDir);
        ok = copyRecursive(src, storeDir);
        if (ok && !d.target.isEmpty()) {
            fs.mkdir(dst);
            ok = copyRecursive(storeDir, dst);
        }
    } else {
        // Direct copy from source to target without store caching
        if (!d.target.isEmpty() && src != dst) {
            fs.mkdir(dst);
            ok = copyRecursive(src, dst);
        } else {
            ok = true;
        }
    }

    if (ok) {
        String depManifest = dst + "/tau.yml";
        if (pathExists(depManifest)) {
            Array<String> visited;
            ParseError err;
            ParsedManifest parsed = Manifest::load(depManifest, visited, err);
            if (err.ok) {
                String oldManifestPath = _opts.manifestPath;
                _opts.manifestPath = depManifest;
                run(parsed.directives);
                _opts.manifestPath = oldManifestPath;
            }
        }
    }
    return ok;
}

bool Runner::execGit(const DGit &d) {
    GHRepo repo;
    if (!GHRepo::parse(d.source, repo)) {
        // Try treating source as a raw git URL
        repo.owner = "";
        repo.repo  = d.source;
        repo.ref   = d.branch;
    } else {
        repo.ref = d.branch;
    }

    String commit = GitHub::resolveCommit(repo);
    String target = resolvePath(d.target);

    String gitUrl = d.source;
    if (!gitUrl.startsWith("http://") && !gitUrl.startsWith("https://") && !gitUrl.startsWith("git@")) {
        gitUrl = String("https://github.com/") + repo.owner + "/" + repo.repo + ".git";
    }

    bool ok = false;
    if (d.store) {
        String storeDir = Config::storePath() + "/git_" + hexEncode(Security::hash(gitUrl + ":" + commit, 8));
        ok = GitHub::cloneOrUpdate(gitUrl, commit, storeDir, true);

        if (ok && !target.isEmpty()) {
            long long lastSlash = rfind(target, '/');
            if (lastSlash > 0) mkdirP(target.substring(0, (size_t)lastSlash));
            struct stat st;
            if (::lstat(target.c_str(), &st) == 0) ::system(("rm -rf '" + target + "'").c_str());
            if (::symlink(storeDir.c_str(), target.c_str()) != 0) {
                copyRecursive(storeDir, target);
            }
        }
    } else {
        // Direct clone into target without store caching
        if (!target.isEmpty()) {
            long long lastSlash = rfind(target, '/');
            if (lastSlash > 0) mkdirP(target.substring(0, (size_t)lastSlash));
            ok = GitHub::cloneOrUpdate(gitUrl, commit, target, false);
        } else {
            ok = true;
        }
    }


    if (ok) {
        String depManifest = target + "/tau.yml";
        if (pathExists(depManifest)) {
            Array<String> visited;
            ParseError err;
            ParsedManifest parsed = Manifest::load(depManifest, visited, err);
            if (err.ok) {
                String oldManifestPath = _opts.manifestPath;
                _opts.manifestPath = depManifest;
                run(parsed.directives);
                _opts.manifestPath = oldManifestPath;
            }
        }
    }
    return ok;
}

bool Runner::execGHRelease(const DGHRelease &d) {
    GHRepo repo;
    if (!GHRepo::parse(d.source, repo)) return false;
    repo.ref = d.branch;

    GHRelease release = GitHub::resolveRelease(repo);
    if (release.tag.isEmpty()) {
        logError("tau: no matching release for ", d.source);
        return false;
    }

    String target = resolvePath(d.target);

    if (d.name == "source") {
        // Download repo snapshot
        String url = String("https://github.com/") + repo.owner + "/" +
                     repo.repo + "/archive/refs/tags/" + release.tag + ".tar.gz";
        GHAsset snap;
        snap.name        = repo.repo + "-" + release.tag + ".tar.gz";
        snap.downloadUrl = url;
        return GitHub::downloadAsset(snap, target);
    }

    // Match asset by name (supports regex)
    Encoding::Regex nameRe(d.name);
    for (size_t i = 0; i < release.assets.length(); ++i) {
        const GHAsset &asset = release.assets[i];
        bool match = false;
        if (d.name.startsWith("reg ")) {
            Encoding::Regex re(d.name.substring(4));
            match = re.matchAll(asset.name, 1).length() > 0;
        } else {
            match = (asset.name == d.name);
        }
        if (match) return GitHub::downloadAsset(asset, target);
    }

    logError("tau: no matching asset '", d.name, "' in release ", release.tag);
    return false;
}

bool Runner::execManifest(const DManifest &d) {
    // Already expanded during parsing (turned into DAnd block)
    // This handler is a no-op fallthrough
    (void)d;
    return true;
}

// ─── Logic blocks ─────────────────────────────────────────────────────────────

bool Runner::execOr(const DOr &d) {
    logInfo("tau/execOr children count=", d.children.length());
    for (auto *child : d.children) {
        if (!child) continue;
        logInfo("  execOr child kind=", (int)child->kind);
        DirectiveList single; single.push(child);
        if (run(single)) return true;
    }
    return false;
}

bool Runner::execAnd(const DAnd &d) {
    logInfo("tau/execAnd children count=", d.children.length());
    for (auto *child : d.children) {
        if (!child) continue;
        DirectiveList single; single.push(child);
        if (!run(single)) return false;
    }
    return true;
}

bool Runner::execNand(const DNand &d) {
    return !execAnd(*reinterpret_cast<const DAnd *>(&d));
}

bool Runner::execNor(const DNor &d) {
    return !execOr(*reinterpret_cast<const DOr *>(&d));
}

bool Runner::execXor(const DXor &d) {
    int trueCount = 0;
    for (auto *child : d.children) {
        if (!child) continue;
        DirectiveList single; single.push(child);
        if (run(single)) ++trueCount;
    }
    return (trueCount % 2) == 1;
}

// ─── Variable directives ──────────────────────────────────────────────────────

bool Runner::execVar(const DVarBase &d) {
    Array<String> keys;

    if (d.keyIsRegex) {
        Encoding::Regex re(d.key);
        for (auto &entry : _vars) {
            if (entry.key.isEmpty()) continue;
            if (re.matchAll(entry.key, 1).length() > 0) {
                keys.push(entry.key);
                if (!d.matchAll) break; // regvar stops on first match
            }
        }
    } else {
        keys.push(d.key);
    }

    if (keys.length() == 0) {
        if (d.op == VarOp::Assign) {
            String val = interp(d.value);
            if (val.isEmpty()) {
                _vars.remove(d.key);
                ::unsetenv(d.key.c_str());
            } else {
                _vars[d.key] = val;
                ::setenv(d.key.c_str(), val.c_str(), 1);
            }
            return true;
        }
        return false;
    }

    bool result = true;
    for (size_t ki = 0; ki < keys.length(); ++ki) {
        const String &key = keys[ki];
        String *current = _vars.get(key);
        String curVal   = current ? *current : String("");

        switch (d.op) {
            case VarOp::Assign: {
                String val = interp(d.value);
                if (val.isEmpty()) {
                    _vars.remove(key);
                    ::unsetenv(key.c_str());
                } else {
                    _vars[key] = val;
                    ::setenv(key.c_str(), val.c_str(), 1);
                }
                result = true;
                break;
            }

            case VarOp::CheckEquals:
                result = (curVal == interp(d.value));
                break;

            case VarOp::CheckIncludes:
                result = curVal.find(interp(d.value)) >= 0;
                break;

            case VarOp::CheckRegex: {
                Encoding::Regex re(interp(d.value));
                result = re.matchAll(curVal, 1).length() > 0;
                break;
            }

            case VarOp::CheckGT:
                result = Collection::parseDouble(curVal) >
                         Collection::parseDouble(interp(d.value));
                break;
            case VarOp::CheckGTE:
                result = Collection::parseDouble(curVal) >=
                         Collection::parseDouble(interp(d.value));
                break;
            case VarOp::CheckLT:
                result = Collection::parseDouble(curVal) <
                         Collection::parseDouble(interp(d.value));
                break;
            case VarOp::CheckLTE:
                result = Collection::parseDouble(curVal) <=
                         Collection::parseDouble(interp(d.value));
                break;
        }

        if (!result && !d.matchAll) break;
    }

    return result;
}

bool Runner::execNewVar(const DNewVar &d) {
    Array<String> allKeys;
    for (auto &entry : _vars) {
        if (!entry.key.isEmpty()) allKeys.push(entry.key);
    }

    for (size_t ki = 0; ki < allKeys.length(); ++ki) {
        const String &k = allKeys[ki];
        bool keep = false;
        for (size_t pi = 0; pi < d.patterns.length(); ++pi) {
            if (k == d.patterns[pi]) {
                keep = true;
                break;
            }
            Encoding::Regex re(d.patterns[pi]);
            if (re.matchAll(k, 1).length() > 0) {
                keep = true;
                break;
            }
        }
        if (!keep) {
            _vars.remove(k);
            ::unsetenv(k.c_str());
        }
    }
    return true;
}

bool Runner::execRmVar(const DRmVar &d) {
    Array<String> allKeys;
    for (auto &entry : _vars) {
        if (!entry.key.isEmpty()) allKeys.push(entry.key);
    }

    for (size_t ki = 0; ki < allKeys.length(); ++ki) {
        const String &k = allKeys[ki];
        bool removeKey = false;
        for (size_t pi = 0; pi < d.patterns.length(); ++pi) {
            if (k == d.patterns[pi]) {
                removeKey = true;
                break;
            }
            Encoding::Regex re(d.patterns[pi]);
            if (re.matchAll(k, 1).length() > 0) {
                removeKey = true;
                break;
            }
        }
        if (removeKey) {
            _vars.remove(k);
            ::unsetenv(k.c_str());
        }
    }
    return true;
}



// ─── Control directives ───────────────────────────────────────────────────────

bool Runner::execFail(const DFail &d) {
    if (!d.message.isEmpty())
        logError("tau: fail: ", interp(d.message));
    return false;
}

bool Runner::execThrow(const DThrow &d) {
    throw TauThrow{ interp(d.message) };
}

// ─── Packaging ────────────────────────────────────────────────────────────────

bool Runner::execBin(const DBin &d) {
    _bins.push(&d);
    return true;
}

bool Runner::execBinDir(const DBinDir &d) {
    String dir = resolvePath(d.path);
    Resource::LinuxFS fs;
    fs.mkdir(dir);

    String targetManifest = _opts.sourceManifestPath.isEmpty() ? _opts.manifestPath : _opts.sourceManifestPath;
    char realTarget[4096];
    if (::realpath(targetManifest.c_str(), realTarget)) {
        targetManifest = String(realTarget);
    }

    for (size_t i = 0; i < _bins.length(); ++i) {
        const DBin *b = _bins[i];
        if (b && !b->name.isEmpty()) {
            String scriptPath = dir + "/" + b->name;
            String content = "#!/bin/sh\n";
            content += "# Wrapper script generated by tau for bin: " + b->name + "\n";
            if (!b->description.isEmpty()) content += "# Description: " + b->description + "\n";
            if (!targetManifest.isEmpty()) {
                content += "exec tau run --headless --remove \"" + targetManifest + "\" -- \"$@\"\n";
            } else {
                content += "exec tau run --headless --remove -- \"$@\"\n";
            }
            fs.write(scriptPath, content);
            ::chmod(scriptPath.c_str(), 0755);
        }
    }
    return true;
}



// ─── Helpers ──────────────────────────────────────────────────────────────────

String Runner::resolvePath(const String &p) const {
    if (p.isEmpty()) return p;

    String current = interp(p);

    // @ is ONLY valid inside image: fields — it resolves to the image's internal
    // temp base dir (_activeImgDir), which is set during execImage and cleared after.
    // @ paths are internal and bypass both relative and chroot resolution.
    if (current.startsWith("@/") && !_activeImgDir.isEmpty()) {
        return _activeImgDir + current.substring(1);
    } else if (current == "@" && !_activeImgDir.isEmpty()) {
        return _activeImgDir;
    }

    // Resolve relative paths against the manifest directory
    if (current[0] != '/') {
        if (!_opts.manifestPath.isEmpty()) {
            String mPath = _opts.manifestPath;
            char rbuf[4096];
            if (::realpath(mPath.c_str(), rbuf)) mPath = String(rbuf);

            if (mPath.find("/instances/") < 0 && mPath.find("/manifests/") < 0) {
                long long sl = rfind(mPath, '/');

                if (sl >= 0) {
                    String dir = mPath.substring(0, (size_t)sl);
                    current = dir + "/" + current;
                }
            }
        }

        if (current[0] != '/') {
            char cwdBuf[4096];
            if (::getcwd(cwdBuf, sizeof(cwdBuf)))
                current = String(cwdBuf) + "/" + current;
        }
    }

    // chroot context: after chroot: is set, ALL absolute paths in directives
    // are relative to the chroot root — not just spawn commands.
    // e.g. mkdir: "/app" with chroot: "/tmp/myroot" → creates /tmp/myroot/app
    if (_isoCtx.hasChrootd && !_isoCtx.chrootPath.isEmpty()) {
        // Only prefix if not already rooted under the chroot path
        if (!current.startsWith(_isoCtx.chrootPath + "/") && current != _isoCtx.chrootPath) {
            current = _isoCtx.chrootPath + current;
        }
    }

    return current;
}

String Runner::interp(const String &s) const {
    return Manifest::interpolate(s, _vars);
}

String Runner::allocSpawnName(const String &hint) {
    if (!hint.isEmpty()) {
        // Ensure uniqueness
        String name = hint;
        int suffix = 1;
        while (true) {
            bool found = false;
            for (auto *sp : _spawns) {
                if (sp->name == name) { found = true; break; }
            }
            if (!found) return name;
            name = hint + intStr(suffix++);
        }
    }
    // Auto-number
    String name = intStr(_nextSpawnId++);
    return name;
}

bool Runner::reapSpawn(ActiveSpawn *sp) {
    if (!sp || sp->waited) return true;
    int status;
    pid_t r = ::waitpid(sp->pid, &status, WNOHANG);
    if (r == sp->pid) {
        sp->waited = true;
        if (WIFEXITED(status))
            sp->exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            sp->exitCode = -WTERMSIG(status);
        notifySpawnChange(*sp);
        return true;
    }
    return false;
}

void Runner::notifySpawnChange(const ActiveSpawn &sp) {
    if (!_opts.onSpawnChange) return;

    SpawnState state;
    state.name     = sp.name;
    state.pid      = sp.pid;
    state.command  = sp.command;
    state.hasPTY   = sp.ptyMaster >= 0;
    state.exitCode = sp.exitCode;

    if (sp.waited) {
        state.status = (sp.exitCode == 0) ? SpawnStatus::Exited : SpawnStatus::Failed;
    } else {
        state.status = SpawnStatus::Running;
    }

    _opts.onSpawnChange(state);
}

bool Runner::copyRecursive(const String &src, const String &dst) {
    struct stat st;
    if (::lstat(src.c_str(), &st) != 0) {
        logError("tau: copy: source not found: ", src);
        return false;
    }

    long long sl = rfind(dst, '/');
    if (sl > 0) mkdirP(dst.substring(0, (size_t)sl));

    if (S_ISLNK(st.st_mode)) {
        char linkTarget[4096];
        ssize_t len = ::readlink(src.c_str(), linkTarget, sizeof(linkTarget) - 1);
        if (len > 0) {
            linkTarget[len] = '\0';
            ::unlink(dst.c_str());
            return ::symlink(linkTarget, dst.c_str()) == 0;
        }
        return false;
    }

    if (S_ISREG(st.st_mode)) {
        int inFd = ::open(src.c_str(), O_RDONLY);
        if (inFd < 0) return false;
        int outFd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
        if (outFd < 0) { ::close(inFd); return false; }

        char buf[65536];
        ssize_t n;
        while ((n = ::read(inFd, buf, sizeof(buf))) > 0) {
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = ::write(outFd, buf + written, (size_t)(n - written));
                if (w <= 0) break;
                written += w;
            }
        }
        ::close(inFd);
        ::close(outFd);
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        mkdirP(dst, st.st_mode & 0777);
        DIR *dir = ::opendir(src.c_str());
        if (!dir) return false;
        struct dirent *ent;
        bool ok = true;
        while ((ent = ::readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
                continue;
            ok = copyRecursive(src + "/" + ent->d_name, dst + "/" + ent->d_name) && ok;
        }
        ::closedir(dir);
        return ok;
    }
    return true;
}


bool Runner::runShell(const String &cmd) {
    return ::system(cmd.c_str()) == 0;
}

Map<String, String> Runner::currentEnv() const {
    return _vars;
}

} // namespace Tau
