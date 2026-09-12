/**
 * @file Runner.cpp
 * @brief Directive executor — walks a DirectiveList and carries out each step.
 */

#include <Tau/Runner.hpp>
#include <Tau/Manifest.hpp>
#include <Tau/Cgroup.hpp>
#include <Tau/GitHub.hpp>
#include <Tau/Docker.hpp>
#include <Tau/Config.hpp>
#include <Tau/Util.hpp>

#include <Resource/File.hpp>
#include <Data/Regex.hpp>
#include <Sec/Hash.hpp>

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
#include <sys/prctl.h>
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
        if (sp->pid > 0 && !sp->waited) {
            ::kill(-sp->pid, SIGKILL);
            ::kill(sp->pid, SIGKILL);
        }
        sp->outRing.close();
        sp->errRing.close();
        delete sp;
    }
    _spawns.clear();

    // First, clean up /dev and /proc stubs on all chroot and mount paths
    if (!_isoCtx.chrootPath.isEmpty()) {
        Namespace::cleanupRootfsStubs(_isoCtx.chrootPath);
    }
    for (size_t i = 0; i < _mounts.length(); ++i) {
        Namespace::cleanupRootfsStubs(_mounts[i]);
    }
    for (size_t i = 0; i < _tempDirs.length(); ++i) {
        Namespace::cleanupRootfsStubs(_tempDirs[i]);
    }

    logInfo("tau: Runner::cleanup() _imageSyncs=", intStr(_imageSyncs.length()), " _mounts=", intStr(_mounts.length()));
    // Sync any modified upperdirs back into their respective image dirs before unmounting
    for (size_t i = 0; i < _imageSyncs.length(); ++i) {
        String srcDir = _imageSyncs[i].upperDir;
        if (!srcDir.isEmpty() && !_imageSyncs[i].imgDir.isEmpty() && pathExists(srcDir) && pathExists(_imageSyncs[i].imgDir)) {
            String syncOutCmd = String("cp -a --no-preserve=ownership '") + srcDir + "/.' '" + _imageSyncs[i].imgDir + "/'";
            int scrc = ::system(syncOutCmd.c_str());
            logInfo("tau: cleanup syncOut upperDir rc=", intStr(scrc), " cmd=", syncOutCmd);
            String rmLost = String("rm -rf '") + _imageSyncs[i].imgDir + "/lost+found' '" + _imageSyncs[i].imgDir + "/work' 2>/dev/null";
            (void)::system(rmLost.c_str());
        } else {
            String mergedDir = _imageSyncs[i].mergedDir;
            if (!mergedDir.isEmpty() && !_imageSyncs[i].imgDir.isEmpty() && pathExists(mergedDir) && pathExists(_imageSyncs[i].imgDir)) {
                String syncMergedCmd = String("cp -a --no-preserve=ownership '") + mergedDir + "/.' '" + _imageSyncs[i].imgDir + "/'";
                int scrc = ::system(syncMergedCmd.c_str());
                logInfo("tau: cleanup syncMerged mergedDir rc=", intStr(scrc), " cmd=", syncMergedCmd);
                String rmLost = String("rm -rf '") + _imageSyncs[i].imgDir + "/lost+found' '" + _imageSyncs[i].imgDir + "/work' 2>/dev/null";
                (void)::system(rmLost.c_str());
            }
        }
    }
    ::sync();

    // Unmount all tracked mounts in reverse order
    for (long long i = (long long)_mounts.length() - 1; i >= 0; --i) {
        String m = _mounts[(size_t)i];
        if (m.isEmpty() || m == "/" || m == "/proc" || m == "/sys" || m == "/dev" || m == "/run" || m == "/etc" || m == "/home" || m == "/root" || m == "/usr" || m == "/var") continue;
        int urc = ::umount2(m.c_str(), 0);
        logInfo("tau: cleanup umount2 '", m, "' rc=", intStr(urc), " err=", ::strerror(errno));
        if (urc != 0) {
            String fusermountCmd = String("fusermount -u '") + m + "' >/dev/null 2>&1";
            if (::system(fusermountCmd.c_str()) != 0) {
                fusermountCmd = String("fusermount -u -z '") + m + "' >/dev/null 2>&1";
                (void)::system(fusermountCmd.c_str());
                (void)::umount2(m.c_str(), MNT_DETACH);
            }
        }
    }
    ::sync();

    // Detach any loop devices attached by this runner
    for (size_t i = 0; i < _createdLoopDevs.length(); ++i) {
        if (!_createdLoopDevs[i].isEmpty()) {
            String cmd = String("losetup -d '") + _createdLoopDevs[i] + "' >/dev/null 2>&1";
            (void)::system(cmd.c_str());
        }
    }

    // Un-join all bridge targets joined by this runner
    for (size_t i = 0; i < _joinedBridgeTargets.length(); ++i) {
        if (!_joinedBridgeTargets[i].isEmpty()) {
            Namespace::hostUnjoinBridge(_joinedBridgeTargets[i]);
        }
    }

    // Delete any host veth interfaces created by this runner
    for (size_t i = 0; i < _createdVethHosts.length(); ++i) {
        if (!_createdVethHosts[i].isEmpty()) {
            Namespace::hostCleanupLink(_createdVethHosts[i]);
        }
    }

    // Delete bridges created by this runner IF no other foreign interfaces remain attached
    for (size_t i = 0; i < _createdBridges.length(); ++i) {
        if (!_createdBridges[i].isEmpty()) {
            Namespace::hostDeleteBridgeIfEmpty(_createdBridges[i], _createdVethHosts);
        }
    }

    // Detach any loop devices attached by this runner
    for (size_t i = 0; i < _createdLoopDevs.length(); ++i) {
        if (!_createdLoopDevs[i].isEmpty()) {
            String cmd = String("losetup -d '") + _createdLoopDevs[i] + "' >/dev/null 2>&1";
            (void)::system(cmd.c_str());
        }
    }

    // Clean up temporary image mount directories
    for (size_t i = 0; i < _tempDirs.length(); ++i) {
        String d = _tempDirs[i];
        if (!d.isEmpty()) {
            (void)::umount2(d.c_str(), MNT_DETACH);
            removeDirRecursive(d);
        }
    }
}



static const char *directiveKindName(DirectiveKind k) {
    switch (k) {
        case DirectiveKind::Wait:      return "wait";
        case DirectiveKind::WaitExit:  return "wait_exit";
        case DirectiveKind::Spawn:     return "spawn";
        case DirectiveKind::Memory:    return "memory";
        case DirectiveKind::CPUSet:    return "cpuset";
        case DirectiveKind::CPU:       return "cpu";
        case DirectiveKind::Chroot:    return "chroot";
        case DirectiveKind::Isolate:   return "isolate";
        case DirectiveKind::Veth:      return "veth";
        case DirectiveKind::IP:        return "ip";
        case DirectiveKind::NewNet:    return "newnet";
        case DirectiveKind::Forward:   return "forward";
        case DirectiveKind::Macvlan:   return "macvlan";
        case DirectiveKind::IPVlan:    return "ipvlan";
        case DirectiveKind::Bridge:    return "bridge";
        case DirectiveKind::Mount:     return "mount";
        case DirectiveKind::Copy:      return "copy";
        case DirectiveKind::Unlink:    return "unlink";
        case DirectiveKind::Mkdir:     return "mkdir";
        case DirectiveKind::Symlink:   return "symlink";
        case DirectiveKind::Image:     return "image";
        case DirectiveKind::Docker:    return "docker";
        case DirectiveKind::VM:        return "vm";
        case DirectiveKind::Local:     return "clone";
        case DirectiveKind::Git:       return "git";
        case DirectiveKind::GHRelease: return "gh_release";
        case DirectiveKind::Manifest:  return "manifest";
        case DirectiveKind::Or:        return "or";
        case DirectiveKind::And:       return "and";
        case DirectiveKind::Nand:      return "nand";
        case DirectiveKind::Nor:       return "nor";
        case DirectiveKind::Xor:       return "xor";
        case DirectiveKind::Var:       return "var";
        case DirectiveKind::RegVar:    return "reg_var";
        case DirectiveKind::RallVar:   return "rall_var";
        case DirectiveKind::NewVar:    return "new_var";
        case DirectiveKind::RmVar:     return "rm_var";
        case DirectiveKind::Fail:      return "fail";
        case DirectiveKind::Throw:     return "throw";
        case DirectiveKind::Bin:       return "bin";
        case DirectiveKind::BinDir:    return "bindir";
        default:                       return "unknown";
    }
}

String Runner::directiveSummary(const Directive *d) {
    if (!d) return "null";
    switch (d->kind) {
        case DirectiveKind::Spawn: {
            auto *sd = static_cast<const DSpawn*>(d);
            return String("spawn '") + sd->command + "'";
        }
        case DirectiveKind::Mount: {
            auto *md = static_cast<const DMount*>(d);
            return String("mount '") + md->source + "' -> '" + md->target + "'";
        }
        case DirectiveKind::Image: {
            auto *id = static_cast<const DImage*>(d);
            return String("image '") + id->path + "'";
        }
        case DirectiveKind::Docker: {
            auto *dk = static_cast<const DDocker*>(d);
            return String("docker '") + (!dk->image.isEmpty() ? dk->image : dk->source) + "' -> '" + dk->target + "'";
        }
        case DirectiveKind::VM: {
            auto *vmd = static_cast<const DVM*>(d);
            String drvStr = vmd->drives.length() > 0 ? vmd->drives[0].source : "none";
            return String("vm drive='") + drvStr + "'";
        }
        case DirectiveKind::Veth: {
            auto *vd = static_cast<const DVeth*>(d);
            return String("veth '") + vd->hostName + "' <-> '" + vd->peerName + "'";
        }
        case DirectiveKind::IP: {
            auto *ipd = static_cast<const DIP*>(d);
            return String("ip ") + ipd->command;
        }
        case DirectiveKind::NewNet: {
            auto *nnd = static_cast<const DNewNet*>(d);
            String ifaces;
            for (size_t i = 0; i < nnd->allowList.length(); ++i) {
                if (i > 0) ifaces += ", ";
                ifaces += nnd->allowList[i];
            }
            return String("newnet [") + ifaces + "]";
        }
        case DirectiveKind::Forward: {
            auto *fd = static_cast<const DForward*>(d);
            return String("forward ") + (fd->sourcePort.isEmpty() ? fd->port : fd->sourcePort) +
                   " -> " + fd->port + (!fd->target.isEmpty() ? (" (" + fd->target + ")") : "");
        }
        case DirectiveKind::Chroot: {
            auto *cd = static_cast<const DChroot*>(d);
            return String("chroot '") + cd->path + "'";
        }
        case DirectiveKind::Isolate: {
            return "isolate";
        }
        case DirectiveKind::Copy: {
            auto *cpd = static_cast<const DCopy*>(d);
            return String("copy '") + cpd->source + "' -> '" + cpd->target + "'";
        }
        case DirectiveKind::Symlink: {
            auto *sld = static_cast<const DSymlink*>(d);
            return String("symlink '") + sld->source + "' -> '" + sld->target + "'";
        }
        case DirectiveKind::Mkdir: {
            auto *mkd = static_cast<const DMkdir*>(d);
            return String("mkdir '") + mkd->path + "'";
        }
        case DirectiveKind::Unlink: {
            auto *uld = static_cast<const DUnlink*>(d);
            return String("unlink '") + uld->path + "'";
        }
        case DirectiveKind::Git: {
            auto *gd = static_cast<const DGit*>(d);
            return String("git clone '") + gd->source + "' (" + gd->branch + ")";
        }
        case DirectiveKind::GHRelease: {
            auto *ghd = static_cast<const DGHRelease*>(d);
            return String("ghrelease '") + ghd->source + "' (" + ghd->name + ")";
        }
        case DirectiveKind::Local: {
            auto *ld = static_cast<const DLocal*>(d);
            return String("local '") + ld->source + "' -> '" + ld->target + "'";
        }
        case DirectiveKind::Var:
        case DirectiveKind::RegVar:
        case DirectiveKind::RallVar: {
            auto *vd = static_cast<const DVarBase*>(d);
            return String("var '") + vd->key + " = " + vd->value + "'";
        }
        case DirectiveKind::Fail: {
            auto *fd = static_cast<const DFail*>(d);
            return String("fail: ") + fd->message;
        }
        case DirectiveKind::Throw: {
            auto *td = static_cast<const DThrow*>(d);
            return String("throw: ") + td->message;
        }
        default:
            return directiveKindName(d->kind);
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

    if (!_opts.enterSlot.isEmpty()) {
        String slotName = _opts.enterSlot;
        _opts.enterSlot = "";
        DEntry entryDir;
        entryDir.name = slotName;
        execEntry(entryDir);
    }

    bool ok = true;
    for (size_t i = 0; i < directives.length(); ++i) {
        auto *d = directives[i];
        if (!d) continue;
        bool result = true;

        switch (d->kind) {
            case DirectiveKind::Wait:      result = execWait     (*static_cast<DWait*>(d));      break;
            case DirectiveKind::WaitExit:  result = execWaitExit (*static_cast<DWaitExit*>(d));  break;
            case DirectiveKind::Retry:     result = execRetry    (*static_cast<DRetry*>(d));     break;
            case DirectiveKind::Spawn:     result = execSpawn    (*static_cast<DSpawn*>(d));     break;
            case DirectiveKind::Dotenv:    result = execDotenv   (*static_cast<DDotenv*>(d));    break;
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
            case DirectiveKind::Docker:    result = execDocker   (*static_cast<DDocker*>(d));    break;
            case DirectiveKind::VM:        result = execVM       (*static_cast<DVM*>(d));        break;
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
            case DirectiveKind::ESlot:     result = execESlot    (*static_cast<DESlot*>(d));     break;
            case DirectiveKind::NoESlot:   result = execNoESlot  (*static_cast<DNoESlot*>(d));   break;
            case DirectiveKind::Entry:     result = execEntry    (*static_cast<DEntry*>(d));     break;
            case DirectiveKind::RmVar:     result = execRmVar    (*static_cast<DRmVar*>(d));     break;
            case DirectiveKind::Fail:      result = execFail     (*static_cast<DFail*>(d));      break;
            case DirectiveKind::Throw:     result = execThrow    (*static_cast<DThrow*>(d));     break;
            case DirectiveKind::Bin:       result = execBin      (*static_cast<DBin*>(d));       break;
            case DirectiveKind::BinDir:    result = execBinDir   (*static_cast<DBinDir*>(d));    break;
            default: result = true; break;
        }

        if (!result) {
            if (d->kind != DirectiveKind::Fail && d->kind != DirectiveKind::Throw) {
                logError("tau: [", directiveSummary(d), "] failed");
            }
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
            ::kill(-sp->pid, sig);
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

bool Runner::execRetry(const DRetry &d) {
    int attempts = 0;
    while (true) {
        if (_opts.shouldStop && _opts.shouldStop()) return false;
        attempts++;
        bool ok = false;
        size_t spawnsBefore = _spawns.length();
        try {
            ok = run(d.children);
            for (size_t i = spawnsBefore; i < _spawns.length(); ++i) {
                auto *sp = _spawns[i];
                if (!sp->waited) waitSpawn(sp);
                if (sp->exitCode != 0) ok = false;
            }
        } catch (const TauThrow &t) {
            logWarn("tau/retry: child threw: ", t.message);
            ok = false;
        }

        if (ok) return true;

        if (d.times > 0 && attempts >= d.times) {
            return false;
        }

        // Clean up failed spawns from this attempt so next attempt can re-launch them
        while (_spawns.length() > spawnsBefore) {
            auto *sp = _spawns[_spawns.length() - 1];
            _spawns.pop();
            delete sp;
        }

        double sleepSec = d.backoff;
        if (sleepSec > 0) {
            ::usleep((useconds_t)(sleepSec * 1000000.0));
        }
    }
}

bool Runner::execDotenv(const DDotenv &d) {
    String resolvedPath = resolvePath(interp(d.path));
    int fd = ::open(resolvedPath.c_str(), O_RDONLY);
    if (fd < 0) {
        logWarn("tau/dotenv: cannot open ", resolvedPath, ": ", ::strerror(errno));
        return false;
    }
    char buf[16384];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return true;
    buf[n] = '\0';

    String content(buf);
    Array<String> lines = content.split("\n");
    for (size_t i = 0; i < lines.length(); ++i) {
        String line = lines[i].trim();
        if (line.isEmpty() || line.startsWith("#")) continue;
        long long eq = line.find("=");
        if (eq < 0) continue;
        String key = line.substring(0, (size_t)eq).trim();
        String val = line.substring((size_t)eq + 1).trim();
        if (val.startsWith("\"") && val.endsWith("\"") && val.length() >= 2) {
            val = val.substring(1, val.length() - 1);
        } else if (val.startsWith("'") && val.endsWith("'") && val.length() >= 2) {
            val = val.substring(1, val.length() - 1);
        }
        if (!key.isEmpty()) {
            _vars[key] = val;
            ::setenv(key.c_str(), val.c_str(), 1);
        }
    }
    return true;
}

// ─── Spawn execution ──────────────────────────────────────────────────────────

bool Runner::execSpawn(const DSpawn &d) {
    String sname = d.name.isEmpty() ? allocSpawnName("") : d.name;

    // Check currently active spawns in memory
    for (auto *existing : _spawns) {
        if (existing->name == sname && existing->pid > 0 && !existing->waited) {
            return true;
        }
    }

    // Check persisted instance state to rehook or skip exited spawns
    if (!_opts.instanceDir.isEmpty()) {
        InstanceState state;
        if (Instance::load(_opts.instanceDir, state)) {
            const SpawnState *st = state.findSpawn(sname);
            if (st) {
                if (st->status == SpawnStatus::Exited && st->exitCode == 0) {
                    return true;
                }
                if (st->pid > 0 && ::kill(st->pid, 0) == 0) {
                    auto *sp = new ActiveSpawn();
                    sp->name = st->name;
                    sp->pid = st->pid;
                    sp->command = st->command;
                    sp->hasPTY = st->hasPTY;
                    _spawns.push(sp);
                    return true;
                }
            }
        }
    }

    bool allocPTY = !_opts.storeMode && d.waitForExit;

    ActiveSpawn *sp = launchSpawn(sname, d, allocPTY);
    if (!sp) return false;

    notifySpawnChange(*sp);

    if (d.waitForExit) {
        waitSpawn(sp);
        if (sp->exitCode != 0) {
            logError("tau: spawn '", sp->name, "' ('", sp->command, "') exited with status ", intStr(sp->exitCode));
            return false;
        }
        return true;
    }
    return true;
}






ActiveSpawn *Runner::launchSpawn(const String &spawnName, const DSpawn &d, bool allocPTY) {
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
    sp->hasPTY  = allocPTY;

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
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);

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
                    if (p == 0) {
                        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
                        ::setsid();
                        (void)::ioctl(0, TIOCSCTTY, 1);
                        (void)::tcsetpgrp(0, ::getpgrp());
                    }
                }
            } else {
                (void)::ioctl(0, TIOCSCTTY, 1);
                (void)::tcsetpgrp(0, ::getpgrp());
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
                Namespace::ensureRootfsStubs(_isoCtx.chrootPath);

                char slaveName[128] = {};
                if (::ttyname_r(0, slaveName, sizeof(slaveName)) == 0 && slaveName[0] != '\0') {
                    const char *targetNodes[] = { "/dev/console", "/dev/tty1", "/dev/tty" };
                    for (const char *tn : targetNodes) {
                        String fullTarget = _isoCtx.chrootPath + tn;
                        ::unlink(fullTarget.c_str());
                        int fd = ::open(fullTarget.c_str(), O_CREAT | O_WRONLY, 0666);
                        if (fd >= 0) ::close(fd);
                        (void)::mount(slaveName, fullTarget.c_str(), nullptr, MS_BIND, nullptr);
                    }
                }

                if (::chroot(_isoCtx.chrootPath.c_str()) == 0) {
                    (void)::chdir("/");
                }
                ::setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin", 1);
                ::setenv("HOME", "/root", 1);
                ::setenv("USER", "root", 1);
            } else {
                String fullPath;
                for (size_t bi = 0; bi < _activeBinDirs.length(); ++bi) {
                    if (!_activeBinDirs[bi].isEmpty()) {
                        fullPath += _activeBinDirs[bi] + ":";
                    }
                }
                const char *existingPath = ::getenv("PATH");
                if (existingPath && *existingPath) {
                    fullPath += existingPath;
                } else {
                    fullPath += "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin";
                }
                ::setenv("PATH", fullPath.c_str(), 1);
            }

            if (_isoCtx.hasMountNS && (_isoCtx.hasPidNS || _isoCtx.isIsolated)) {
                (void)::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr);
            }

            // Export current runner variables to child environment
            if (!_opts.sourceManifestPath.isEmpty()) {
                ::setenv("TAU_INSTANCE_MANIFEST", _opts.sourceManifestPath.c_str(), 1);
            } else if (!_opts.manifestPath.isEmpty()) {
                ::setenv("TAU_INSTANCE_MANIFEST", _opts.manifestPath.c_str(), 1);
            }
            for (auto &entry : _vars) {
                if (!entry.key.isEmpty() && !entry.value.isEmpty()) {
                    if (entry.key == "PATH" && _isoCtx.hasChrootd) continue;
                    ::setenv(entry.key.c_str(), entry.value.c_str(), 1);
                }
            }

            ::setenv("TERM", "xterm-256color", 1);
            if (_isoCtx.isIsolated || _isoCtx.hasChrootd) {
                ::setenv("container", "lxc", 1);
                ::setenv("container_ttys", "0", 1);
            }
            ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            ::perror("tau/spawn: execl /bin/sh failed");
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
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);

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
                    if (p == 0) {
                        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
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
                Namespace::ensureRootfsStubs(_isoCtx.chrootPath);
                if (::chroot(_isoCtx.chrootPath.c_str()) == 0) {
                    (void)::chdir("/");
                }
                ::setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin", 1);
                ::setenv("HOME", "/root", 1);
                ::setenv("USER", "root", 1);
            } else {
                String fullPath;
                for (size_t bi = 0; bi < _activeBinDirs.length(); ++bi) {
                    if (!_activeBinDirs[bi].isEmpty()) {
                        fullPath += _activeBinDirs[bi] + ":";
                    }
                }
                const char *existingPath = ::getenv("PATH");
                if (existingPath && *existingPath) {
                    fullPath += existingPath;
                } else {
                    fullPath += "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin";
                }
                ::setenv("PATH", fullPath.c_str(), 1);
            }

            if (_isoCtx.hasMountNS && (_isoCtx.hasPidNS || _isoCtx.isIsolated)) {
                (void)::mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr);
            }

            // Export current runner variables to child environment
            if (!_opts.sourceManifestPath.isEmpty()) {
                ::setenv("TAU_INSTANCE_MANIFEST", _opts.sourceManifestPath.c_str(), 1);
            } else if (!_opts.manifestPath.isEmpty()) {
                ::setenv("TAU_INSTANCE_MANIFEST", _opts.manifestPath.c_str(), 1);
            }
            for (auto &entry : _vars) {
                if (!entry.key.isEmpty() && !entry.value.isEmpty()) {
                    if (entry.key == "PATH" && _isoCtx.hasChrootd) continue;
                    ::setenv(entry.key.c_str(), entry.value.c_str(), 1);
                }
            }

            ::setenv("TERM", "xterm-256color", 1);
            if (_isoCtx.isIsolated || _isoCtx.hasChrootd) {
                ::setenv("container", "lxc", 1);
                ::setenv("container_ttys", "0", 1);
            }
            ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            ::perror("tau/spawn: execl /bin/sh failed");
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
    _isoCtx.memoryBytes     = d.bytes > 0 ? d.bytes : (!d.raw.isEmpty() ? Manifest::parseSize(d.raw) : 0);
    _isoCtx.memoryBytesMax  = d.bytesMax > 0 ? d.bytesMax : (!d.rawMax.isEmpty() ? Manifest::parseSize(d.rawMax) : 0);
    _isoCtx.hasCgroupLimits = true;
    if (!_opts.instanceName.isEmpty() && Cgroup::create(_opts.instanceName)) {
        Cgroup::setMemory(_opts.instanceName, "", _isoCtx.memoryBytes, _isoCtx.memoryBytesMax);
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
    _isoCtx.cpuQuotaMax     = d.quotaMax;
    _isoCtx.hasCgroupLimits = true;
    if (!_opts.instanceName.isEmpty() && Cgroup::create(_opts.instanceName)) {
        Cgroup::setCPU(_opts.instanceName, "", _isoCtx.cpuQuota, _isoCtx.cpuQuotaMax);
    }
    return true;
}


// ─── Isolation directives ─────────────────────────────────────────────────────

bool Runner::execChroot(const DChroot &d) {
    String path = resolvePath(d.path);
    Namespace::ensureRootfsStubs(path);
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
    String host = interp(d.hostName);
    String peer = interp(d.peerName);
    if (!host.isEmpty()) {
        Namespace::hostCleanupLink(host);
    }
    bool ok = false;
    if (_isoCtx.hasNetNS) {
        String cmd = String("ip link add ") + host + " type veth peer name " + peer + " 2>/dev/null";
        ok = (::system(cmd.c_str()) == 0);
    } else {
        ok = Namespace::hostCreateVeth(host, peer);
    }
    if (ok) {
        if (!host.isEmpty()) _createdVethHosts.push(host);
        if (_opts.onIfaceEvent) {
            if (!host.isEmpty()) _opts.onIfaceEvent(host, "created");
            if (!peer.isEmpty()) _opts.onIfaceEvent(peer, "created");
        }
    }
    return ok;
}

bool Runner::execIP(const DIP &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    String raw = interp(d.command).trim();
    int rc = 0;
    if (_isoCtx.hasNetNS) {
        String cmd = String("ip ") + raw;
        rc = ::system(cmd.c_str());
    } else {
        rc = Namespace::hostRunIP(raw) ? 0 : 1;
    }
    if (rc != 0) return false;

    Array<String> tokens = raw.split(" ");
    Array<String> words;
    for (size_t i = 0; i < tokens.length(); ++i) {
        String t = tokens[i].trim();
        if (!t.isEmpty()) words.push(t);
    }

    if (words.length() >= 4 && (words[0] == "link" || words[0] == "l") && words[1] == "set") {
        String iface = words[2];
        String state = words[3]; // "up" or "down"
        if ((state == "up" || state == "down") && _opts.onIfaceEvent) {
            _opts.onIfaceEvent(iface, state);
        }
    } else if (words.length() >= 4 && (words[0] == "addr" || words[0] == "a" || words[0] == "address")) {
        String action = words[1]; // "add" or "del"
        if (action == "add" || action == "del") {
            String ipWithCidr = words[2];
            String iface;
            for (size_t wi = 3; wi < words.length(); ++wi) {
                if (words[wi] == "dev" && wi + 1 < words.length()) {
                    iface = words[wi + 1];
                    break;
                }
            }
            if (!iface.isEmpty() && !ipWithCidr.isEmpty()) {
                if (_opts.onIPActionEvent) {
                    _opts.onIPActionEvent(iface, ipWithCidr, action);
                } else if (_opts.onIPEvent && action == "add") {
                    _opts.onIPEvent(iface, ipWithCidr);
                }
            }
        }
    }
    return true;
}


bool Runner::execNewNet(const DNewNet &d) {
    if (!Namespace::enterUser(_isoCtx)) return false;
    bool ok = Namespace::enterNet(d.allowList, _isoCtx);
    if (ok) {
        if (_opts.onIfaceEvent) {
            _opts.onIfaceEvent("lo", "up");
        }
        if (_opts.onIPActionEvent) {
            _opts.onIPActionEvent("lo", "127.0.0.1", "add");
        } else if (_opts.onIPEvent) {
            _opts.onIPEvent("lo", "127.0.0.1");
        }
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
    String brSource = interp(!d.source.isEmpty() ? d.source : d.name);
    String brTarget = interp(!d.target.isEmpty() ? d.target : d.attach);
    String addr = interp(d.address);

    if (brSource.isEmpty()) {
        logError("tau: bridge directive missing source/name");
        return false;
    }

    bool bridgeExists = Namespace::linkExists(brSource);
    bool ok = true;
    if (!bridgeExists) {
        ok = Namespace::hostCreateBridge(brSource, addr);
        if (ok) {
            _createdBridges.push(brSource);
        }
    } else if (!addr.isEmpty()) {
        Namespace::hostRunIP("addr add " + addr + " dev " + brSource);
    }

    if (ok && !brTarget.isEmpty()) {
        bool jOk = Namespace::hostJoinBridge(brSource, brTarget);
        if (jOk) {
            _joinedBridgeTargets.push(brTarget);
        }
    }

    if (ok && !addr.isEmpty() && _opts.onIPEvent) {
        _opts.onIPEvent(brSource, addr);
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


// ─── Filesystem and Partition Utilities ──────────────────────────────────────

static bool formatFilesystem(const String &targetPath, const String &fstype, const String &label, const String &sourceDir = "") {
    String fs = fstype.toLowerCase().trim();
    if (fs.isEmpty() || fs == "gpt" || fs == "mbr" || fs == "dos" || fs == "raw") fs = "ext4";
    String cmd;
    if (fs == "ext4" || fs == "ext3" || fs == "ext2") {
        cmd = String("mkfs.") + fs + " -F -O ^has_journal,^metadata_csum_seed ";
        if (!label.isEmpty()) cmd += "-L '" + label + "' ";
        cmd += "'" + targetPath + "' >/dev/null 2>&1";
    } else if (fs == "xfs") {
        cmd = "mkfs.xfs -f ";
        if (!label.isEmpty()) cmd += "-L '" + label + "' ";
        cmd += "'" + targetPath + "' >/dev/null 2>&1";
    } else if (fs == "btrfs") {
        cmd = "mkfs.btrfs -f ";
        if (!label.isEmpty()) cmd += "-L '" + label + "' ";
        cmd += "'" + targetPath + "' >/dev/null 2>&1";
    } else if (fs == "vfat" || fs == "fat32" || fs == "fat" || fs == "fat16") {
        cmd = "mkfs.vfat -F 32 ";
        if (!label.isEmpty()) cmd += "-n '" + label + "' ";
        cmd += "'" + targetPath + "' >/dev/null 2>&1";
    } else if (fs == "ntfs") {
        cmd = "mkfs.ntfs -F ";
        if (!label.isEmpty()) cmd += "-L '" + label + "' ";
        cmd += "'" + targetPath + "' >/dev/null 2>&1";
    } else if (fs == "f2fs") {
        cmd = "mkfs.f2fs -f ";
        if (!label.isEmpty()) cmd += "-l '" + label + "' ";
        cmd += "'" + targetPath + "' >/dev/null 2>&1";
    } else if (fs == "swap") {
        cmd = "mkswap ";
        if (!label.isEmpty()) cmd += "-L '" + label + "' ";
        cmd += "'" + targetPath + "' >/dev/null 2>&1";
    } else if (fs == "squashfs" && !sourceDir.isEmpty()) {
        cmd = String("mksquashfs '") + sourceDir + "' '" + targetPath + "' -noappend >/dev/null 2>&1";
    } else if (fs == "erofs" && !sourceDir.isEmpty()) {
        cmd = String("mkfs.erofs '") + targetPath + "' '" + sourceDir + "' >/dev/null 2>&1";
    } else {
        cmd = String("mkfs.") + fs + " -F '" + targetPath + "' >/dev/null 2>&1";
    }
    return (::system(cmd.c_str()) == 0);
}

static bool checkFilesystem(const String &targetPath, const String &fstype) {
    String fs = fstype.toLowerCase().trim();
    if (fs == "ext4" || fs == "ext3" || fs == "ext2" || fs.isEmpty() || fs == "gpt" || fs == "raw") {
        String cmd = String("e2fsck -fy '") + targetPath + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    } else if (fs == "vfat" || fs == "fat32" || fs == "fat") {
        String cmd = String("fsck.vfat -a '") + targetPath + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    } else if (fs == "ntfs") {
        String cmd = String("ntfsfix '") + targetPath + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    } else if (fs == "f2fs") {
        String cmd = String("fsck.f2fs -a '") + targetPath + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    }
    return true;
}

static bool resizeFilesystem(const String &targetPath, const String &fstype, const String &mountPoint = "") {
    String fs = fstype.toLowerCase().trim();
    if (fs == "ext4" || fs == "ext3" || fs == "ext2" || fs.isEmpty() || fs == "gpt" || fs == "raw") {
        String cmd = String("e2fsck -fy '") + targetPath + "' >/dev/null 2>&1; resize2fs '" + targetPath + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    } else if (fs == "xfs" && !mountPoint.isEmpty()) {
        String cmd = String("xfs_growfs '") + mountPoint + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    } else if (fs == "btrfs" && !mountPoint.isEmpty()) {
        String cmd = String("btrfs filesystem resize max '") + mountPoint + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    } else if (fs == "ntfs") {
        String cmd = String("ntfsresize -f -b '") + targetPath + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    } else if (fs == "f2fs") {
        String cmd = String("resize.f2fs '") + targetPath + "' >/dev/null 2>&1";
        return (::system(cmd.c_str()) == 0);
    }
    return true;
}

static String attachLoopDevice(const String &imagePath, bool partScan = true) {
    char buf[256] = {};
    String cmd = String("losetup -f ") + (partScan ? "-P " : "") + "--show '" + imagePath + "' 2>/dev/null";
    FILE *fp = ::popen(cmd.c_str(), "r");
    if (fp) {
        if (::fgets(buf, sizeof(buf) - 1, fp)) {
            ::pclose(fp);
            return String(buf).trim();
        }
        ::pclose(fp);
    }
    return "";
}

static void detachLoopDevice(const String &loopDev) {
    if (!loopDev.isEmpty()) {
        String cmd = String("losetup -d '") + loopDev + "' >/dev/null 2>&1";
        (void)::system(cmd.c_str());
    }
}

static bool applyPartitionTable(const String &imagePath, const String &tableType, const Array<ImagePartition> &partitions, bool isNewImage, bool resizeRequested) {
    String tbl = (tableType == "mbr" || tableType == "dos") ? "msdos" : "gpt";

    if (isNewImage) {
        String mklabelCmd = String("parted -s '") + imagePath + "' mklabel " + tbl + " >/dev/null 2>&1";
        (void)::system(mklabelCmd.c_str());
    }

    if (partitions.length() > 0) {
        size_t currentOffsetMiB = 1; // start at 1MiB
        for (size_t i = 0; i < partitions.length(); ++i) {
            const auto &p = partitions[i];
            int partNum = p.number > 0 ? p.number : (int)(i + 1);

            size_t startMiB = currentOffsetMiB;
            if (!p.offset.isEmpty()) {
                size_t offBytes = Manifest::parseSize(p.offset);
                if (offBytes > 0) startMiB = (offBytes + 1048575) / 1048576;
            }
            String startStr = intStr((int)startMiB) + "MiB";

            String endStr;
            if (p.size.isEmpty() || p.size == "max" || p.size == "100%" || (i == partitions.length() - 1 && p.size.isEmpty())) {
                endStr = "100%";
            } else {
                size_t pSizeBytes = Manifest::parseSize(p.size);
                size_t pSizeMiB = (pSizeBytes + 1048575) / 1048576;
                if (pSizeMiB == 0) pSizeMiB = 1;
                currentOffsetMiB = startMiB + pSizeMiB;
                endStr = intStr((int)currentOffsetMiB) + "MiB";
            }

            String partFs = p.type.isEmpty() ? "ext4" : p.type.toLowerCase().trim();
            if (partFs == "vfat" || partFs == "fat") partFs = "fat32";
            String partName = p.name.isEmpty() ? (String("part") + intStr(partNum)) : p.name;

            String chkCmd = String("parted -s '") + imagePath + "' print 2>/dev/null | grep -E '^[[:space:]]*" + intStr(partNum) + "[[:space:]]' >/dev/null 2>&1";
            bool partExists = (::system(chkCmd.c_str()) == 0);

            if (!partExists) {
                String mkpartCmd;
                if (tbl == "gpt") {
                    mkpartCmd = String("parted -s '") + imagePath + "' mkpart " + partName + " " + partFs + " " + startStr + " " + endStr + " >/dev/null 2>&1";
                } else {
                    mkpartCmd = String("parted -s '") + imagePath + "' mkpart primary " + partFs + " " + startStr + " " + endStr + " >/dev/null 2>&1";
                }
                (void)::system(mkpartCmd.c_str());

                if (!p.flags.isEmpty()) {
                    Array<String> flagList = p.flags.split(",");
                    for (size_t f = 0; f < flagList.length(); ++f) {
                        String fl = flagList[f].trim();
                        if (!fl.isEmpty()) {
                            String setFlagCmd = String("parted -s '") + imagePath + "' set " + intStr(partNum) + " " + fl + " on >/dev/null 2>&1";
                            (void)::system(setFlagCmd.c_str());
                        }
                    }
                }
            } else if (resizeRequested && (endStr == "100%" || i == partitions.length() - 1)) {
                if (tbl == "gpt") {
                    String fixGpt = String("printf 'Fix\\n' | parted ---pretend-input-tty '") + imagePath + "' print >/dev/null 2>&1 || true";
                    (void)::system(fixGpt.c_str());
                }
                String resizePartCmd = String("parted -s '") + imagePath + "' resizepart " + intStr(partNum) + " " + endStr + " >/dev/null 2>&1";
                (void)::system(resizePartCmd.c_str());
            }
        }

        // Format or resize partition filesystems using loopback partition mapping
        String loopDev = attachLoopDevice(imagePath, true);
        if (!loopDev.isEmpty()) {
            for (size_t i = 0; i < partitions.length(); ++i) {
                const auto &p = partitions[i];
                int partNum = p.number > 0 ? p.number : (int)(i + 1);
                String partDev = loopDev + "p" + intStr(partNum);
                if (!pathExists(partDev)) {
                    partDev = loopDev + intStr(partNum);
                }
                if (pathExists(partDev)) {
                    String partFs = p.type.isEmpty() ? "ext4" : p.type;
                    if (isNewImage) {
                        formatFilesystem(partDev, partFs, p.label);
                    } else if (resizeRequested) {
                        checkFilesystem(partDev, partFs);
                        resizeFilesystem(partDev, partFs);
                    }
                }
            }
            detachLoopDevice(loopDev);
        }
    }
    return true;
}


bool Runner::execImage(const DImage &d) {
    if (!Namespace::enterUser(_isoCtx)) {
        logError("tau: execImage: cannot enter user namespace");
        return false;
    }

    String rawSource = !d.source.isEmpty() ? d.source : d.path;
    String imagePath = resolvePath(interp(rawSource));

    bool exists = pathExists(imagePath);
    bool isNewImage = !exists;
    if (exists) {
        struct stat st;
        if (::stat(imagePath.c_str(), &st) == 0 && st.st_size == 0) {
            isNewImage = true;
        }
    }

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

    bool resizePerformed = false;
    if (!d.size.isEmpty() || !d.sizeMax.isEmpty()) {
        size_t sizeBytes = !d.size.isEmpty() ? Manifest::parseSize(interp(d.size)) : 0;
        size_t sizeMaxBytes = !d.sizeMax.isEmpty() ? Manifest::parseSize(interp(d.sizeMax)) : 0;
        size_t targetSize = sizeBytes > 0 ? sizeBytes : sizeMaxBytes;

        if (targetSize > 0) {
            struct stat st;
            bool needResize = true;
            if (exists && ::stat(imagePath.c_str(), &st) == 0) {
                if ((size_t)st.st_size == targetSize) {
                    needResize = false;
                } else if (sizeMaxBytes > 0 && (size_t)st.st_size >= targetSize && (size_t)st.st_size <= sizeMaxBytes) {
                    needResize = false; // already within [size, size_max]
                }
            }
            if (needResize) {
                ::truncate(imagePath.c_str(), (off_t)targetSize);
                resizePerformed = true;
            }
        }
    }

    bool isPartitioned = (d.table == "gpt" || d.table == "mbr" || d.table == "dos" ||
                          d.type == "gpt" || d.type == "mbr" || d.type == "dos" ||
                          d.partitions.length() > 0);

    if (isPartitioned) {
        String tbl = !d.table.isEmpty() ? d.table : d.type;
        applyPartitionTable(imagePath, tbl, d.partitions, isNewImage, resizePerformed && d.resize);
    } else {
        if (isNewImage) {
            String fsLabel = interp(d.label);
            formatFilesystem(imagePath, d.type, fsLabel);
        } else if (exists && resizePerformed && d.resize) {
            if (d.fsck) checkFilesystem(imagePath, d.type);
            resizeFilesystem(imagePath, d.type);
        }
    }

    // If target is empty and no partitions have a target, apply-only completed!
    bool hasAnyTarget = !d.target.isEmpty();
    for (size_t p = 0; p < d.partitions.length(); ++p) {
        if (!d.partitions[p].target.isEmpty()) { hasAnyTarget = true; break; }
    }
    if (!hasAnyTarget) {
        _activeImgDir = "";
        return true;
    }

    String baseDir = "/tmp/tau_img_" + hexEncode(Sec::hash(imagePath, 8));
    mkdirP(baseDir);
    _tempDirs.push(baseDir);
    _imagePaths.push(imagePath);
    _activeImgDir = baseDir;

    // Handle Partition Mounts
    if (isPartitioned && d.partitions.length() > 0) {
        String loopDev = attachLoopDevice(imagePath, true);
        if (!loopDev.isEmpty()) {
            _createdLoopDevs.push(loopDev);
            for (size_t i = 0; i < d.partitions.length(); ++i) {
                const auto &p = d.partitions[i];
                if (p.target.isEmpty()) continue;
                int partNum = p.number > 0 ? p.number : (int)(i + 1);
                String partDev = loopDev + "p" + intStr(partNum);
                if (!pathExists(partDev)) {
                    partDev = loopDev + intStr(partNum);
                }
                String partMergedDir = resolvePath(interp(p.target));
                mkdirP(partMergedDir);
                _mounts.push(partMergedDir);

                String partMountDir = baseDir + "/part_" + intStr(partNum);
                mkdirP(partMountDir);
                _mounts.push(partMountDir);

                String mntCmd = String("mount '") + partDev + "' '" + partMountDir + "' >/dev/null 2>&1";
                (void)::system(mntCmd.c_str());

                if (!p.lower.isEmpty()) {
                    String partUpper = !p.upper.isEmpty() ? resolvePath(interp(p.upper)) : (baseDir + "/upper_" + intStr(partNum));
                    String partWork  = !p.work.isEmpty()  ? resolvePath(interp(p.work))  : (baseDir + "/work_" + intStr(partNum));
                    mkdirP(partUpper);
                    mkdirP(partWork);
                    Namespace::doMount(resolvePath(interp(p.lower)), baseDir, partMergedDir, p.read, p.write, partUpper);
                } else {
                    Namespace::doMount(partMountDir, "", partMergedDir, p.read, p.write);
                }
            }
        }
    }

    // Handle Whole Image Mount
    if (!d.target.isEmpty()) {
        String imgDir = baseDir + "/img";
        String mergedDir = resolvePath(interp(d.target));

        for (size_t i = 0; i < _mounts.length(); ++i) {
            if (_mounts[i] == mergedDir || _mounts[i] == imgDir) {
                return true;
            }
        }

        String workDir = d.work.isEmpty() ? baseDir + "/work" : resolvePath(interp(d.work));
        mkdirP(imgDir);
        mkdirP(workDir);

        bool targetExisted = pathExists(mergedDir);
        mkdirP(mergedDir);
        if (!targetExisted || mergedDir.startsWith("/tmp/tau_") || mergedDir.startsWith("/tmp/tau-")) {
            _tempDirs.push(mergedDir);
        }

        bool mounted = false;
        String fsType = d.type.toLowerCase().trim();
        if (d.fsck) checkFilesystem(imagePath, fsType);

        // 1. Check if already attached to loop device
        String loopDev;
        {
            char lbuf[256] = {};
            String jcmd = String("losetup -j '") + imagePath + "' 2>/dev/null";
            FILE *jfp = ::popen(jcmd.c_str(), "r");
            if (jfp) {
                if (::fgets(lbuf, sizeof(lbuf) - 1, jfp)) {
                    String line(lbuf);
                    long long colon = line.find(":");
                    if (colon > 0) {
                        loopDev = line.substring(0, (size_t)colon).trim();
                    }
                }
                ::pclose(jfp);
            }
        }
        if (loopDev.isEmpty()) {
            loopDev = attachLoopDevice(imagePath, false);
            if (!loopDev.isEmpty()) _createdLoopDevs.push(loopDev);
        }

        if (!loopDev.isEmpty()) {
            String mountCmd = String("mount '") + loopDev + "' '" + imgDir + "' >/dev/null 2>&1";
            mounted = (::system(mountCmd.c_str()) == 0);
            if (!mounted && !fsType.isEmpty()) {
                mountCmd = String("mount -t ") + fsType + " '" + loopDev + "' '" + imgDir + "' >/dev/null 2>&1";
                mounted = (::system(mountCmd.c_str()) == 0);
            }
        }
        if (!mounted) {
            String mountCmd = String("mount -o loop '") + imagePath + "' '" + imgDir + "' >/dev/null 2>&1";
            mounted = (::system(mountCmd.c_str()) == 0);
            if (!mounted && !fsType.isEmpty()) {
                mountCmd = String("mount -t ") + fsType + " -o loop '" + imagePath + "' '" + imgDir + "' >/dev/null 2>&1";
                mounted = (::system(mountCmd.c_str()) == 0);
            }
        }

        // 2. If loop mount failed (e.g. unprivileged user namespace), fall back to fuse2fs
        if (!mounted && (fsType == "ext4" || fsType == "ext3" || fsType == "ext2" || fsType.isEmpty())) {
            if (::system("which fuse2fs >/dev/null 2>&1") == 0) {
                String fuseCmd = String("fuse2fs -o fakeroot,rw '") + imagePath + "' '" + imgDir + "' >/dev/null 2>&1";
                mounted = (::system(fuseCmd.c_str()) == 0);
            }
        }

        logInfo("tau: execImage: imagePath=", imagePath, " loopDev=", loopDev, " mounted=", mounted ? "1" : "0");
        if (mounted) {
            // Ensure FUSE / loop mount is fully ready
            for (int retry = 0; retry < 40; ++retry) {
                DIR *dtest = ::opendir(imgDir.c_str());
                if (dtest) {
                    struct dirent *de;
                    int count = 0;
                    while ((de = ::readdir(dtest)) != nullptr) count++;
                    ::closedir(dtest);
                    if (count > 2) { ::usleep(20000); break; }
                }
                ::usleep(25000);
            }
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
            String upperSource = !d.upper.isEmpty() ? interp(d.upper) : "";

            // If no external upper directory specified and image is mounted, try direct overlay on image disk
            String diskUpper = imgDir + "/upper";
            String diskWork  = imgDir + "/work";

            logInfo("tau: execImage: resolvedLower=", resolvedLower, " diskUpper=", diskUpper, " mergedDir=", mergedDir);

            if (mounted && upperSource.isEmpty()) {
                String cleanWork = String("rm -rf '") + diskWork + "' 2>/dev/null";
                (void)::system(cleanWork.c_str());
                mkdirP(diskUpper);
                mkdirP(diskWork);

                // If existing disk has files at root instead of /upper (from previous runs), migrate them to /upper
                DIR *dcheck = ::opendir(imgDir.c_str());
                if (dcheck) {
                    struct dirent *de;
                    while ((de = ::readdir(dcheck)) != nullptr) {
                        if (de->d_name[0] == '.') continue;
                        String fname(de->d_name);
                        if (fname == "lost+found" || fname == "upper" || fname == "work") continue;
                        String srcPath = imgDir + "/" + fname;
                        String dstPath = diskUpper + "/" + fname;
                        if (!pathExists(dstPath)) {
                            ::rename(srcPath.c_str(), dstPath.c_str());
                        }
                    }
                    ::closedir(dcheck);
                }

                String ovlOpts = String("lowerdir=") + resolvedLower + ",upperdir=" + diskUpper + ",workdir=" + diskWork + ",index=off,metacopy=off";
                int rc = ::mount("overlay", mergedDir.c_str(), "overlay", 0, ovlOpts.c_str());
                if (rc != 0) {
                    ovlOpts = String("lowerdir=") + resolvedLower + ",upperdir=" + diskUpper + ",workdir=" + diskWork + ",index=off";
                    rc = ::mount("overlay", mergedDir.c_str(), "overlay", 0, ovlOpts.c_str());
                }
                if (rc != 0) {
                    ovlOpts = String("lowerdir=") + resolvedLower + ",upperdir=" + diskUpper + ",workdir=" + diskWork + ",index=off,userxattr";
                    rc = ::mount("overlay", mergedDir.c_str(), "overlay", 0, ovlOpts.c_str());
                }
                if (rc != 0) {
                    ovlOpts = String("lowerdir=") + resolvedLower + ",upperdir=" + diskUpper + ",workdir=" + diskWork;
                    rc = ::mount("overlay", mergedDir.c_str(), "overlay", 0, ovlOpts.c_str());
                }
                if (rc != 0) {
                    ovlOpts = String("lowerdir=") + resolvedLower + ",upperdir=" + diskUpper + ",workdir=" + diskWork + ",userxattr";
                    rc = ::mount("overlay", mergedDir.c_str(), "overlay", 0, ovlOpts.c_str());
                }
                if (rc != 0) {
                    String mntCmd = String("mount -t overlay overlay '") + mergedDir + "' -o 'lowerdir=" + resolvedLower + ",upperdir=" + diskUpper + ",workdir=" + diskWork + ",index=off' >/dev/null 2>&1";
                    rc = ::system(mntCmd.c_str());
                }
                if (rc != 0) {
                    String mntCmd = String("mount -t overlay overlay '") + mergedDir + "' -o 'lowerdir=" + resolvedLower + ",upperdir=" + diskUpper + ",workdir=" + diskWork + ",index=off,userxattr' >/dev/null 2>&1";
                    rc = ::system(mntCmd.c_str());
                }
                if (rc != 0) {
                    String mntCmd = String("mount -t overlay overlay '") + mergedDir + "' -o 'lowerdir=" + resolvedLower + ",upperdir=" + diskUpper + ",workdir=" + diskWork + "' >/dev/null 2>&1";
                    rc = ::system(mntCmd.c_str());
                }
                logInfo("tau: execImage direct overlay rc=", intStr(rc), " err=", ::strerror(errno));
                if (rc == 0) {
                    // Native direct disk overlay active! Changes are written directly to disk in real time.
                    _mounts.push(mergedDir);
                    _activeImgDir = "";
                    return true;
                }
            }

            // Fallback for FUSE or external upper directory
            String upperDir = baseDir + "/upper";
            String hostWorkDir = baseDir + "/work";
            mkdirP(upperDir);
            mkdirP(hostWorkDir);

            if (!upperSource.isEmpty() && pathExists(resolvePath(upperSource))) {
                String copyCmd = String("cp -a --no-preserve=ownership '") + resolvePath(upperSource) + "/.' '" + upperDir + "/'";
                (void)::system(copyCmd.c_str());
            } else if (mounted) {
                if (pathExists(diskUpper)) {
                    String syncUpper = String("cp -a --no-preserve=ownership '") + diskUpper + "/.' '" + upperDir + "/'";
                    int surc = ::system(syncUpper.c_str());
                    logInfo("tau: execImage syncUpper diskUpper -> upperDir rc=", intStr(surc), " cmd=", syncUpper);
                }
            }
            String rmLost = String("rm -rf '") + upperDir + "/lost+found' '" + upperDir + "/work' 2>/dev/null";
            (void)::system(rmLost.c_str());

            if (mounted) {
                mkdirP(diskUpper);
                ImageSync isync;
                isync.upperDir  = upperDir;
                isync.mergedDir = "";
                isync.imgDir    = diskUpper;
                _imageSyncs.push(isync);
            }

            logInfo("tau: execImage fallback Namespace::doMount upperDir=", upperDir);
            Namespace::doMount(resolvedLower, baseDir, mergedDir, d.read, d.write, upperDir);
            _mounts.push(mergedDir);
        } else if (mounted) {
            _mounts.push(mergedDir);
            Namespace::doMount(imgDir, "", mergedDir, d.read, d.write);
            if (mergedDir != imgDir) {
                ImageSync isync;
                isync.upperDir  = "";
                isync.mergedDir = mergedDir;
                isync.imgDir    = imgDir;
                _imageSyncs.push(isync);
            }
        }
    }

    _activeImgDir = "";
    return true;
}

bool Runner::execDocker(const DDocker &d) {
    if (!Namespace::enterUser(_isoCtx)) {
        logError("tau: execDocker: cannot enter user namespace");
        return false;
    }

    DDocker resolvedD = d;
    resolvedD.target = resolvePath(d.target);
    if (!d.source.isEmpty()) resolvedD.source = resolvePath(d.source);
    if (!d.work.isEmpty())   resolvedD.work   = resolvePath(d.work);

    // If already mounted on target, preserve existing mount
    for (size_t i = 0; i < _mounts.length(); ++i) {
        if (_mounts[i] == resolvedD.target) {
            return true;
        }
    }

    Array<String> outLayers;
    String globalStore = Config::storePath();
    bool ok = Docker::pullAndMount(resolvedD, globalStore, _opts.storeMode, outLayers);
    if (!ok) {
        logError("tau: [docker] failed to pull and mount image: ", !d.image.isEmpty() ? d.image : d.source);
        return false;
    }

    _mounts.push(resolvedD.target);
    return true;
}

// ─── Dependency directives ────────────────────────────────────────────────────

bool Runner::execLocal(const DLocal &d) {
    String src = resolvePath(d.source);
    String dst = resolvePath(d.target.isEmpty() ? d.source : d.target);

    bool ok = false;
    Resource::LinuxFS fs;

    bool useStore = _opts.storeMode && d.store;

    if (useStore) {
        // Copy source into store directory and symlink/copy to target
        String storeDir = Config::storePath() + "/local_" +
                          hexEncode(Sec::hash(src, 8));
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
                _manifestDepth++;
                run(parsed.directives);
                _manifestDepth--;
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
    bool useStore = _opts.storeMode && d.store;

    if (useStore) {
        String storeDir = Config::storePath() + "/git_" + hexEncode(Sec::hash(gitUrl + ":" + commit, 8));
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
                _manifestDepth++;
                run(parsed.directives);
                _manifestDepth--;
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
    Data::Regex nameRe(d.name);
    for (size_t i = 0; i < release.assets.length(); ++i) {
        const GHAsset &asset = release.assets[i];
        bool match = false;
        if (d.name.startsWith("reg ")) {
            Data::Regex re(d.name.substring(4));
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
        Data::Regex re(d.key);
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
        String curVal   = current ? *current : interp(key);

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
                Data::Regex re(interp(d.value));
                result = re.matchAll(curVal, 1).length() > 0;
                break;
            }

            case VarOp::CheckGT:
                result = Xi::parseDouble(curVal) >
                         Xi::parseDouble(interp(d.value));
                break;
            case VarOp::CheckGTE:
                result = Xi::parseDouble(curVal) >=
                         Xi::parseDouble(interp(d.value));
                break;
            case VarOp::CheckLT:
                result = Xi::parseDouble(curVal) <
                         Xi::parseDouble(interp(d.value));
                break;
            case VarOp::CheckLTE:
                result = Xi::parseDouble(curVal) <=
                         Xi::parseDouble(interp(d.value));
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
            Data::Regex re(d.patterns[pi]);
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
            Data::Regex re(d.patterns[pi]);
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

// ─── ESlot & Entry directives ──────────────────────────────────────────────────

static pid_t getParentPidFromProc(pid_t pid) {
    if (pid <= 1) return 0;
    char statPath[64];
    ::snprintf(statPath, sizeof(statPath), "/proc/%d/stat", (int)pid);
    int fd = ::open(statPath, O_RDONLY);
    if (fd < 0) return 0;
    char buf[1024] = {};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return 0;
    // Format: pid (comm) state ppid ...
    char *closeParen = ::strrchr(buf, ')');
    if (!closeParen) return 0;
    pid_t ppid = 0;
    if (::sscanf(closeParen + 2, "%*c %d", &ppid) == 1) {
        return ppid;
    }
    return 0;
}

DESlot *Runner::findESlot(const String &name) {
    // 1. Check local _activeESlots first (inside slots are always allowed)
    for (size_t i = 0; i < _activeESlots.length(); ++i) {
        auto *es = _activeESlots[i];
        if (es && es->name == name) {
            return es;
        }
    }

    // If noeslot is active, filter outside eslot requests (from parents / instances / globals)
    if (_noESlotEnabled) {
        bool allowed = false;
        for (size_t i = 0; i < _noESlotExceptions.length(); ++i) {
            if (_noESlotExceptions[i] == name) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            logInfo("tau/findESlot: outside eslot '", name, "' masked by noeslot directive");
            return nullptr; // outside slot masked
        }
    }

    // 2. Check TAU_INSTANCE_MANIFEST if set in environment by parent tau-instance
    const char *envManifest = ::getenv("TAU_INSTANCE_MANIFEST");
    if (envManifest && *envManifest && pathExists(envManifest)) {
        Array<String> visited;
        ParseError perr;
        auto *pm = new ParsedManifest(Manifest::load(envManifest, visited, perr));
        if (perr.ok) {
            Manifest::resolveVariables(pm->directives, _vars);
            for (auto *d : pm->directives) {
                if (d && d->kind == DirectiveKind::ESlot) {
                    auto *es = static_cast<DESlot*>(d);
                    if (es->name == name) {
                        _activeESlots.push(es);
                        return es;
                    }
                }
            }
        }
    }

    // 3. Walk parent processes up to PID 1 to find any parent tau-instance
    pid_t cur = ::getpid();
    Array<pid_t> ancestors;
    while (cur > 1) {
        ancestors.push(cur);
        pid_t parent = getParentPidFromProc(cur);
        if (parent <= 1 || parent == cur) break;
        cur = parent;

        // Directly check cmdline of ancestor process for any manifest paths
        char cmdlinePath[64];
        ::snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%d/cmdline", (int)cur);
        int cfd = ::open(cmdlinePath, O_RDONLY);
        if (cfd >= 0) {
            char cbuf[4096] = {};
            ssize_t cn = ::read(cfd, cbuf, sizeof(cbuf) - 1);
            ::close(cfd);
            if (cn > 0) {
                // cmdline has null-separated args
                ssize_t idx = 0;
                while (idx < cn) {
                    String arg(cbuf + idx);
                    if (arg.isEmpty()) {
                        idx++;
                        continue;
                    }
                    idx += arg.length() + 1;
                    if (arg.endsWith(".yml") || arg.endsWith(".yaml")) {
                        if (pathExists(arg)) {
                            Array<String> visited;
                            ParseError perr;
                            auto *pm = new ParsedManifest(Manifest::load(arg, visited, perr));
                            if (perr.ok) {
                                Manifest::resolveVariables(pm->directives, _vars);
                                for (auto *d : pm->directives) {
                                    if (d && d->kind == DirectiveKind::ESlot) {
                                        auto *es = static_cast<DESlot*>(d);
                                        if (es->name == name) {
                                            _activeESlots.push(es);
                                            return es;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Also check /proc/<pid>/environ for TAU_INSTANCE_MANIFEST
        char envPath[64];
        ::snprintf(envPath, sizeof(envPath), "/proc/%d/environ", (int)cur);
        int efd = ::open(envPath, O_RDONLY);
        if (efd >= 0) {
            char ebuf[8192] = {};
            ssize_t en = ::read(efd, ebuf, sizeof(ebuf) - 1);
            ::close(efd);
            if (en > 0) {
                ssize_t eidx = 0;
                while (eidx < en) {
                    String ev(ebuf + eidx);
                    if (ev.isEmpty()) {
                        eidx++;
                        continue;
                    }
                    eidx += ev.length() + 1;
                    if (ev.startsWith("TAU_INSTANCE_MANIFEST=")) {
                        String eMan = ev.substring(22);
                        if (pathExists(eMan)) {
                            Array<String> visited;
                            ParseError perr;
                            auto *pm = new ParsedManifest(Manifest::load(eMan, visited, perr));
                            if (perr.ok) {
                                Manifest::resolveVariables(pm->directives, _vars);
                                for (auto *d : pm->directives) {
                                    if (d && d->kind == DirectiveKind::ESlot) {
                                        auto *es = static_cast<DESlot*>(d);
                                        if (es->name == name) {
                                            _activeESlots.push(es);
                                            return es;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    String instRoot = Config::instancesPath();
    DIR *id = ::opendir(instRoot.c_str());
    if (id) {
        struct dirent *ent;
        while ((ent = ::readdir(id)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            String iname(ent->d_name);
            String idir = instRoot + "/" + iname;
            InstanceState st;
            if (Instance::load(idir, st)) {
                bool isAncestorInstance = false;
                for (size_t ai = 0; ai < ancestors.length(); ++ai) {
                    pid_t apid = ancestors[ai];
                    if (st.spawnPID == apid) {
                        isAncestorInstance = true;
                        break;
                    }
                    for (size_t si = 0; si < st.spawns.length(); ++si) {
                        if (st.spawns[si].pid == apid) {
                            isAncestorInstance = true;
                            break;
                        }
                    }
                    if (isAncestorInstance) break;
                }

                if (isAncestorInstance) {
                    String mPath = idir + "/instance.yml";
                    if (!pathExists(mPath)) mPath = idir + "/manifest.yml";
                    if (!pathExists(mPath) && !st.manifestPath.isEmpty()) mPath = st.manifestPath;
                    if (pathExists(mPath)) {
                        Array<String> visited;
                        ParseError perr;
                        auto *pm = new ParsedManifest(Manifest::load(mPath, visited, perr));
                        if (perr.ok) {
                            Manifest::resolveVariables(pm->directives, _vars);
                            for (auto *d : pm->directives) {
                                if (d && d->kind == DirectiveKind::ESlot) {
                                    auto *es = static_cast<DESlot*>(d);
                                    if (es->name == name) {
                                        ::closedir(id);
                                        _activeESlots.push(es);
                                        return es;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        ::closedir(id);
    }

    // 4. Check global slots directories (TAU_PATH/slots and /var/tau/slots)
    Config::init();
    Array<String> slotDirs;
    slotDirs.push(Config::slotsPath());
    if (Config::slotsGlobalPath() != Config::slotsPath()) {
        slotDirs.push(Config::slotsGlobalPath());
    }

    for (size_t di = 0; di < slotDirs.length(); ++di) {
        String sdir = slotDirs[di];
        String slotFile = sdir + "/" + name + ".yml";
        if (pathExists(slotFile)) {
            Array<String> visited;
            ParseError perr;
            ParsedManifest pm = Manifest::load(slotFile, visited, perr);
            if (perr.ok) {
                Manifest::resolveVariables(pm.directives, _vars);
                for (size_t dii = 0; dii < pm.directives.length(); ++dii) {
                    auto *d = pm.directives[dii];
                    if (d && d->kind == DirectiveKind::ESlot) {
                        auto *origEs = static_cast<DESlot*>(d);
                        if (origEs->name == name) {
                            auto *es = new DESlot();
                            es->name = origEs->name;
                            es->slot = origEs->slot;
                            es->entry = Manifest::cloneDirectives(origEs->entry);
                            es->escape = Manifest::cloneDirectives(origEs->escape);
                            es->inject = Manifest::cloneDirectives(origEs->inject);
                            _activeESlots.push(es);
                            return es;
                        }
                    }
                }
            }
        }
    }

    return nullptr;
}

bool Runner::execESlot(const DESlot &d) {
    // Register eslot locally and globally across parents/children
    _activeESlots.push(const_cast<DESlot *>(&d));
    return true;
}

bool Runner::execNoESlot(const DNoESlot &d) {
    _noESlotEnabled = d.enabled;
    _noESlotExceptions = d.exceptions;
    return true;
}

bool Runner::execEntry(const DEntry &d) {
    String targetName = interp(d.name).trim();
    if (targetName.isEmpty()) return true;

    // Search for matching eslot in local, parents up process tree, or global
    // (findESlot allows inside/local eslots and exceptions, while masking outside ones when noeslot is active)
    DESlot *es = findESlot(targetName);
    if (es) {
        // Ensure all caller runner variables are synced to environment for child spawns
        for (auto &entry : _vars) {
            if (!entry.key.isEmpty() && !entry.value.isEmpty()) {
                ::setenv(entry.key.c_str(), entry.value.c_str(), 1);
            }
        }
        bool entryPassed = true;
        if (es->entry.length() > 0) {
            entryPassed = run(es->entry);
        }
        if (entryPassed && es->slot) {
            if (d.inject.length() > 0) {
                run(d.inject);
            } else if (es->inject.length() > 0) {
                run(es->inject);
            }
        } else {
            if (es->escape.length() > 0) {
                run(es->escape);
            }
        }
        // Stops at first successful slot regardless of result
        return true;
    }
    return true;
}

// ─── Packaging ────────────────────────────────────────────────────────────────

bool Runner::execBin(const DBin &d) {
    if (d.name.isEmpty()) return true;

    String targetManifest = _opts.sourceManifestPath.isEmpty() ? _opts.manifestPath : _opts.sourceManifestPath;
    char realTarget[4096] = {};
    if (::realpath(targetManifest.c_str(), realTarget)) {
        targetManifest = String(realTarget);
    }

    bool found = false;
    for (size_t i = 0; i < _bins.length(); ++i) {
        if (_bins[i].name == d.name) {
            found = true;
            // Overwrite only if this definition is closer (smaller depth)
            if (_manifestDepth < _bins[i].depth) {
                _bins[i].description  = d.description;
                _bins[i].manifestPath = targetManifest;
                _bins[i].depth        = _manifestDepth;
            }
            break;
        }
    }
    if (!found) {
        RegisteredBin rb;
        rb.name         = d.name;
        rb.description  = d.description;
        rb.manifestPath = targetManifest;
        rb.depth        = _manifestDepth;
        _bins.push(rb);
    }

    // If active bin directories exist, generate the wrapper right away as well
    for (size_t di = 0; di < _activeBinDirs.length(); ++di) {
        const String &dir = _activeBinDirs[di];
        String scriptPath = dir + "/" + d.name;
        Resource::LinuxFS fs;
        String content = "#!/bin/sh\n";
        content += "# Wrapper script generated by tau for bin: " + d.name + "\n";
        if (!d.description.isEmpty()) content += "# Description: " + d.description + "\n";
        if (!targetManifest.isEmpty()) {
            content += "exec tau run --headless --remove \"" + targetManifest + "\" -- \"$@\"\n";
        } else {
            content += "exec tau run --headless --remove -- \"$@\"\n";
        }
        fs.write(scriptPath, content);
        ::chmod(scriptPath.c_str(), 0755);
    }

    return true;
}

bool Runner::execBinDir(const DBinDir &d) {
    String dir = resolvePath(d.path);
    Resource::LinuxFS fs;
    fs.mkdir(dir);

    char realDir[4096] = {};
    if (::realpath(dir.c_str(), realDir)) {
        dir = String(realDir);
    }

    // Add to active bin directories if not already present
    bool present = false;
    for (size_t i = 0; i < _activeBinDirs.length(); ++i) {
        if (_activeBinDirs[i] == dir) { present = true; break; }
    }
    if (!present) {
        _activeBinDirs.push(dir);
    }

    // Update PATH variable in _vars for current runner scope
    String currentPath = _vars["PATH"];
    if (currentPath.isEmpty()) {
        const char *ep = ::getenv("PATH");
        currentPath = (ep && *ep) ? String(ep) : "/usr/local/bin:/usr/bin:/bin";
    }
    if (currentPath.find(dir) < 0) {
        _vars["PATH"] = dir + ":" + currentPath;
    }

    // Generate wrappers for all registered bins (current + children + grandchildren)
    for (size_t i = 0; i < _bins.length(); ++i) {
        const RegisteredBin &b = _bins[i];
        if (!b.name.isEmpty()) {
            String scriptPath = dir + "/" + b.name;
            String content = "#!/bin/sh\n";
            content += "# Wrapper script generated by tau for bin: " + b.name + "\n";
            if (!b.description.isEmpty()) content += "# Description: " + b.description + "\n";
            if (!b.manifestPath.isEmpty()) {
                content += "exec tau run --headless --remove \"" + b.manifestPath + "\" -- \"$@\"\n";
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

    // Resolve relative paths against cwd or the manifest directory
    if (current[0] != '/') {
        if (pathExists(current)) {
            char rbuf[4096];
            if (::realpath(current.c_str(), rbuf)) current = String(rbuf);
        } else if (!_opts.sourceManifestPath.isEmpty() || !_opts.manifestPath.isEmpty()) {
            String mPath = !_opts.sourceManifestPath.isEmpty() ? _opts.sourceManifestPath : _opts.manifestPath;
            char rbuf[4096];
            if (::realpath(mPath.c_str(), rbuf)) mPath = String(rbuf);

            if (mPath.find("/instances/") < 0 && mPath.find("/manifests/") < 0) {
                long long sl = rfind(mPath, '/');

                if (sl >= 0) {
                    String dir = mPath.substring(0, (size_t)sl);
                    String candidate = dir + "/" + current;
                    if (pathExists(candidate)) {
                        current = candidate;
                    }
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
        // Never prefix internal tau temp/store/system paths!
        if (!current.startsWith("/tmp/tau") &&
            !current.startsWith("/var/tau") &&
            !current.startsWith("/.var/tau") &&
            !current.startsWith("/run/") &&
            !current.startsWith(_isoCtx.chrootPath + "/") &&
            current != _isoCtx.chrootPath) {
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


// ─── VM directive ─────────────────────────────────────────────────────────────

static String detectDriveFormat(const String &path) {
    if (path.endsWith(".qcow2") || path.endsWith(".qcow")) return "qcow2";
    if (path.endsWith(".raw") || path.endsWith(".img") || path.endsWith(".iso")) return "raw";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        char magic[4] = {};
        if (::read(fd, magic, 4) == 4) {
            if (magic[0] == 'Q' && magic[1] == 'F' && magic[2] == 'I' && (unsigned char)magic[3] == 0xfb) {
                ::close(fd);
                return "qcow2";
            }
        }
        ::close(fd);
    }
    return "raw";
}

bool Runner::execVM(const DVM &d) {
    // 1. Find QEMU binary
    String qemuBin = "qemu-system-x86_64";
    if (pathExists("/usr/bin/qemu-system-x86_64")) {
        qemuBin = "/usr/bin/qemu-system-x86_64";
    } else if (pathExists("/usr/libexec/qemu-kvm")) {
        qemuBin = "/usr/libexec/qemu-kvm";
    } else if (pathExists("/usr/bin/qemu-kvm")) {
        qemuBin = "/usr/bin/qemu-kvm";
    }

    String cmd = qemuBin;

    // 2. Acceleration
    bool hasKvm = (::access("/dev/kvm", R_OK | W_OK) == 0);
    if (d.accel == "kvm" || d.accel.isEmpty()) {
        if (hasKvm) {
            cmd += " -enable-kvm -cpu host";
        } else {
            cmd += " -accel tcg";
        }
    } else if (d.accel == "tcg") {
        cmd += " -accel tcg";
    } else {
        cmd += " -accel " + d.accel;
    }

    // 3. Memory limits
    size_t memBytes = _isoCtx.memoryBytes > 0 ? _isoCtx.memoryBytes : 512 * 1024 * 1024;
    size_t memMb = memBytes / (1024 * 1024);
    if (memMb == 0) memMb = 128;

    if (_isoCtx.memoryBytesMax > memBytes) {
        size_t maxMb = _isoCtx.memoryBytesMax / (1024 * 1024);
        cmd += " -m " + intStr((int)memMb) + "M,maxmem=" + intStr((int)maxMb) + "M,slots=1";
    } else {
        cmd += " -m " + intStr((int)memMb) + "M";
    }

    // 4. CPU limits
    int cores = 1;
    if (_isoCtx.cpuQuota > 0) {
        cores = (int)(_isoCtx.cpuQuota >= 100 ? _isoCtx.cpuQuota / 100 : 1);
    }
    cmd += " -smp " + intStr(cores);

    // 5. Drives
    for (size_t i = 0; i < d.drives.length(); ++i) {
        const auto &drv = d.drives[i];
        String resolvedDrive = resolvePath(interp(drv.source));
        String fmt = drv.format;
        if (fmt.isEmpty() || fmt == "auto") {
            fmt = detectDriveFormat(resolvedDrive);
        } else if (fmt == "qcow") {
            fmt = "qcow2";
        }
        String ro = drv.write ? "off" : "on";
        cmd += " -drive file='" + resolvedDrive + "',if=virtio,format=" + fmt + ",readonly=" + ro;
    }

    // 6. Direct kernel boot (optional)
    if (!d.kernel.isEmpty()) {
        cmd += " -kernel '" + resolvePath(interp(d.kernel)) + "'";
    }
    if (!d.initrd.isEmpty()) {
        cmd += " -initrd '" + resolvePath(interp(d.initrd)) + "'";
    }
    if (!d.cmdline.isEmpty()) {
        cmd += " -append \"" + interp(d.cmdline) + "\"";
    }

    // 7. Networking
    // Check if network namespace has TAP / veth interfaces
    bool attachedTap = false;
    DIR *netDir = ::opendir("/sys/class/net");
    if (netDir) {
        struct dirent *ent;
        while ((ent = ::readdir(netDir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            String ifName(ent->d_name);
            if (ifName == "lo") continue;
            if (ifName.startsWith("tap")) {
                cmd += " -netdev tap,id=net0,ifname=" + ifName + ",script=no,downscript=no -device virtio-net-pci,netdev=net0";
                attachedTap = true;
                break;
            }
        }
        ::closedir(netDir);
    }
    if (!attachedTap) {
        cmd += " -netdev user,id=net0 -device virtio-net-pci,netdev=net0";
    }

    // 8. Console / PTY
    cmd += " -nographic -serial mon:stdio -monitor none";

    // 9. Execute via standard spawn pipeline
    DSpawn sp;
    sp.command     = cmd;
    sp.waitForExit = d.waitForExit;
    sp.name        = !d.name.isEmpty() ? d.name : "vm";

    bool ok = execSpawn(sp);
    const_cast<DVM&>(d).pid      = sp.pid;
    const_cast<DVM&>(d).exitCode = sp.exitCode;
    return ok;
}


bool Runner::runShell(const String &cmd) {
    return ::system(cmd.c_str()) == 0;
}

Map<String, String> Runner::currentEnv() const {
    return _vars;
}

} // namespace Tau
