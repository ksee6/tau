/**
 * @file Namespace.cpp
 * @brief Linux namespace, chroot, mount, and capability management.
 */

#include <Tau/Namespace.hpp>
#include <Tau/Directives.hpp>
#include <Tau/Util.hpp>

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdio>

#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

// libcap is not universally available — use raw syscall for capset
#ifndef _LINUX_CAPABILITY_VERSION_3
#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#endif

struct cap_header { uint32_t version; int pid; };
struct cap_data   { uint32_t effective; uint32_t permitted; uint32_t inheritable; };

namespace Tau {

// ─── Internal helpers ─────────────────────────────────────────────────────────

static bool writeProc(const char *path, const char *content) {
    int fd = ::open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return false;
    size_t len = ::strlen(content);
    ssize_t n  = ::write(fd, content, len);
    ::close(fd);
    return n == (ssize_t)len;
}

// ─── isolate ──────────────────────────────────────────────────────────────────

bool Namespace::isolate(IsolationContext &ctx) {
    if (ctx.isIsolated) return true;

    if (!enterUser(ctx)) return false;

    ctx.hasPidNS   = true;
    ctx.isIsolated = true;
    return true;
}

bool Namespace::setupContainerCaps() {
    ::prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
#ifdef PR_CAP_AMBIENT
#ifdef PR_CAP_AMBIENT_RAISE
    for (int cap = 0; cap <= 40; ++cap) {
        ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0);
    }
#endif
#endif
    return true;
}

// ─── enterUser ────────────────────────────────────────────────────────────────

bool Namespace::enterUser(IsolationContext &ctx) {
    if (ctx.hasUserNS) return true;

    uid_t realUID = ::getuid();
    gid_t realGID = ::getgid();

    if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
        logError("tau: unshare(CLONE_NEWUSER|CLONE_NEWNS): ", ::strerror(errno));
        return false;
    }

    if (!writeIDMaps(realUID, realGID)) return false;

    ctx.hasUserNS  = true;
    ctx.hasMountNS = true;
    return true;
}


// ─── writeIDMaps ──────────────────────────────────────────────────────────────

bool Namespace::writeIDMaps(uid_t realUID, gid_t realGID) {
    pid_t self = ::getpid();
    char path[64], content[64];

    ::snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)self);
    ::snprintf(content, sizeof(content), "0 %d 1\n", (int)realUID);
    if (!writeProc(path, content)) {
        logError("tau: write uid_map: ", ::strerror(errno));
        return false;
    }

    ::snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)self);
    writeProc(path, "deny");

    ::snprintf(path, sizeof(path), "/proc/%d/gid_map", (int)self);
    ::snprintf(content, sizeof(content), "0 %d 1\n", (int)realGID);
    if (!writeProc(path, content)) {
        logError("tau: write gid_map: ", ::strerror(errno));
        return false;
    }

    return true;
}


// ─── enterPID ─────────────────────────────────────────────────────────────────

bool Namespace::enterPID(IsolationContext &ctx) {
    ctx.hasPidNS = true;
    return true;
}

// ─── enterNet ─────────────────────────────────────────────────────────────────


bool Namespace::enterNet(const Array<String> &allowList, IsolationContext &ctx) {
    if (ctx.hasNetNS) return true;

    if (allowList.length() > 0) {
        pid_t parentPid = ::getpid();
        int pfd[2];
        if (::pipe(pfd) == 0) {
            pid_t helper = ::fork();
            if (helper == 0) {
                ::close(pfd[1]);
                char dummy;
                (void)::read(pfd[0], &dummy, 1);
                ::close(pfd[0]);

                for (size_t i = 0; i < allowList.length(); ++i) {
                    String cmd = String("ip link set ") + allowList[i] +
                                 " netns " + intStr(parentPid) + " 2>/dev/null";
                    (void)::system(cmd.c_str());
                }
                ::_exit(0);
            }
            ::close(pfd[0]);

            if (::unshare(CLONE_NEWNET) != 0) {
                logError("tau: unshare(CLONE_NEWNET): ", ::strerror(errno));
                ::write(pfd[1], "x", 1);
                ::close(pfd[1]);
                int st; ::waitpid(helper, &st, 0);
                return false;
            }

            ctx.hasNetNS = true;
            (void)::system("ip link set lo up 2>/dev/null");

            ::write(pfd[1], "k", 1);
            ::close(pfd[1]);
            int st; ::waitpid(helper, &st, 0);
            return true;
        }
    }

    if (::unshare(CLONE_NEWNET) != 0) {
        logError("tau: unshare(CLONE_NEWNET): ", ::strerror(errno));
        return false;
    }
    ctx.hasNetNS = true;
    (void)::system("ip link set lo up 2>/dev/null");

    // Automatically initialize rootless user-mode networking if pasta / slirp4netns is installed
    pid_t self = ::getpid();
    if (::system("which pasta >/dev/null 2>&1") == 0) {
        String pastaCmd = String("pasta -q ") + intStr(self) + " >/dev/null 2>&1 &";
        (void)::system(pastaCmd.c_str());
    } else if (::system("which slirp4netns >/dev/null 2>&1") == 0) {
        String slirpCmd = String("slirp4netns --configure --mtu=65520 --disable-host-loopback ") +
                          intStr(self) + " tap0 >/dev/null 2>&1 &";
        (void)::system(slirpCmd.c_str());
    }

    return true;
}

// ─── doChroot ─────────────────────────────────────────────────────────────────

bool Namespace::doChroot(const String &path, IsolationContext &ctx) {
    ensureRootfsStubs(path);
    mountProc(path);

    if (::chroot(path.c_str()) != 0) {
        logError("tau: chroot(", path, "): ", ::strerror(errno));
        return false;
    }
    if (::chdir("/") != 0) {
        logError("tau: chdir(/) after chroot: ", ::strerror(errno));
        return false;
    }
    ctx.hasChrootd = true;
    ctx.chrootPath = path;
    return true;
}

// ─── createVeth ───────────────────────────────────────────────────────────────

bool Namespace::createVeth(const String &hostName, const String &peerName) {
    String cmd = String("ip link add ") + hostName +
                 " type veth peer name " + peerName + " 2>/dev/null";
    return ::system(cmd.c_str()) == 0;
}

bool Namespace::createMacvlan(const String &parent, const String &name, const String &mode, const String &mac) {
    String m = mode.isEmpty() ? "bridge" : mode;
    String cmd = String("ip link add link ") + parent + " name " + name + " type macvlan mode " + m;
    if (!mac.isEmpty()) {
        cmd += " address " + mac;
    }
    cmd += " 2>/dev/null";
    return ::system(cmd.c_str()) == 0;
}

bool Namespace::createIPVlan(const String &parent, const String &name, const String &mode) {
    String m = mode.isEmpty() ? "l2" : mode;
    String cmd = String("ip link add link ") + parent + " name " + name + " type ipvlan mode " + m + " 2>/dev/null";
    return ::system(cmd.c_str()) == 0;
}

bool Namespace::createBridge(const String &name, const String &attach, const String &address) {
    String cmd = String("ip link add name ") + name + " type bridge 2>/dev/null";
    (void)::system(cmd.c_str());

    if (!attach.isEmpty()) {
        String attachCmd = String("ip link set dev ") + attach + " master " + name + " 2>/dev/null";
        (void)::system(attachCmd.c_str());
    }

    if (!address.isEmpty()) {
        String addrCmd = String("ip addr add ") + address + " dev " + name + " 2>/dev/null";
        (void)::system(addrCmd.c_str());
    }

    String upCmd = String("ip link set ") + name + " up 2>/dev/null";
    return ::system(upCmd.c_str()) == 0;
}

static String parseWildcardCIDR(const String &s) {
    if (s.isEmpty() || s == "*") return "0.0.0.0/0";
    Array<String> octets = s.split(".");
    if (octets.length() != 4) return s;
    int prefix = 32;
    String out;
    for (size_t i = 0; i < 4; ++i) {
        if (i > 0) out += ".";
        if (octets[i] == "*") {
            out += "0";
            if (prefix == 32) prefix = (int)i * 8;
        } else {
            out += octets[i];
        }
    }
    if (prefix < 32) out += "/" + intStr(prefix);
    return out;
}

bool Namespace::setupForwarding(const DForward &d) {
    String srcPort = d.sourcePort.isEmpty() ? d.port : d.sourcePort;
    String dstPort = d.port.isEmpty() ? d.sourcePort : d.port;
    if (srcPort.isEmpty() && dstPort.isEmpty()) return false;

    // Resolve port range span preservation
    long long srcDash = srcPort.find("-");
    long long dstDash = dstPort.find("-");

    if (srcDash >= 0 && dstDash < 0) {
        // e.g. srcPort="1000-2000", dstPort="3000" -> dstPort="3000-4000"
        long long sStart = Collection::parseLong(srcPort.substring(0, (size_t)srcDash));
        long long sEnd   = Collection::parseLong(srcPort.substring((size_t)srcDash + 1));
        long long span   = sEnd - sStart;
        long long dStart = Collection::parseLong(dstPort);
        if (span > 0 && dStart > 0) {
            dstPort = intStr(dStart) + "-" + intStr(dStart + span);
        }
    } else if (dstDash >= 0 && srcDash < 0) {
        // e.g. dstPort="2000-2010", srcPort="1000" -> srcPort="1000-1010"
        long long dStart = Collection::parseLong(dstPort.substring(0, (size_t)dstDash));
        long long dEnd   = Collection::parseLong(dstPort.substring((size_t)dstDash + 1));
        long long span   = dEnd - dStart;
        long long sStart = Collection::parseLong(srcPort);
        if (span > 0 && sStart > 0) {
            srcPort = intStr(sStart) + "-" + intStr(sStart + span);
        }
    }

    String cidr = parseWildcardCIDR(d.sourceIP);
    String proto = d.protocol.isEmpty() ? "both" : d.protocol.toLowerCase();

    Array<String> protos;
    if (proto == "tcp" || proto == "both") protos.push("tcp");
    if (proto == "udp" || proto == "both") protos.push("udp");

    // Replace iptables colons/dashes for port formats
    String iptSrcPort = srcPort;
    iptSrcPort = iptSrcPort.replace("-", ":");
    String iptDstPort = dstPort;
    iptDstPort = iptDstPort.replace("-", ":");

    bool anyOk = false;
    for (size_t i = 0; i < protos.length(); ++i) {
        String p = protos[i];
        String cmd;
        if (!d.ip.isEmpty()) {
            cmd = String("iptables -t nat -A PREROUTING ") +
                  (!d.source.isEmpty() ? ("-i " + d.source + " ") : "") +
                  (!cidr.isEmpty() && cidr != "0.0.0.0/0" ? ("-s " + cidr + " ") : "") +
                  "-p " + p + " --dport " + iptSrcPort +
                  " -j DNAT --to-destination " + d.ip + ":" + iptDstPort + " 2>/dev/null";
        } else {
            cmd = String("iptables -t nat -A PREROUTING ") +
                  (!d.source.isEmpty() ? ("-i " + d.source + " ") : "") +
                  (!cidr.isEmpty() && cidr != "0.0.0.0/0" ? ("-s " + cidr + " ") : "") +
                  "-p " + p + " --dport " + iptSrcPort +
                  " -j REDIRECT --to-ports " + iptDstPort + " 2>/dev/null";
        }
        if (::system(cmd.c_str()) == 0) anyOk = true;
    }

    return anyOk || true;
}



// ─── doMount ──────────────────────────────────────────────────────────────────

bool Namespace::doMount(const String &source, const String &workDir,
                         const String &target, bool read, bool write,
                         const String &upperDir) {
    mkdirP(target);

    if (workDir.isEmpty()) {
        unsigned long flags = MS_BIND | MS_REC;
        if (!write) flags |= MS_RDONLY;
        if (::mount(source.c_str(), target.c_str(), nullptr, flags, nullptr) != 0) {
            String cmd = String("cp -a '") + source + "/.' '" + target + "/' 2>/dev/null";
            ::system(cmd.c_str());
        }
    } else {
        String upper = upperDir.isEmpty() ? (workDir + "/upper") : upperDir;
        String work  = workDir + "/work";
        mkdirP(upper);
        mkdirP(work);

        String opts = String("lowerdir=") + source +
                      ",upperdir=" + upper +
                      ",workdir=" + work +
                      ",userxattr";
        int rc = ::mount("overlay", target.c_str(), "overlay", 0, opts.c_str());
        logInfo("tau/doMount overlay (userxattr): target=", target, " opts=", opts, " rc=", rc, " err=", ::strerror(errno));
        if (rc != 0) {
            opts = String("lowerdir=") + source +
                   ",upperdir=" + upper +
                   ",workdir=" + work;
            rc = ::mount("overlay", target.c_str(), "overlay", 0, opts.c_str());
            logInfo("tau/doMount overlay (plain): target=", target, " opts=", opts, " rc=", rc, " err=", ::strerror(errno));
            if (rc != 0) {
                // Fall back for unprivileged user namespaces on kernels without user-overlayfs
                Array<String> lowers = source.split(":");
                for (long long i = (long long)lowers.length() - 1; i >= 0; --i) {
                    if (!lowers[(size_t)i].isEmpty()) {
                        String cmd = String("cp -a '") + lowers[(size_t)i] + "/.' '" + target + "/' 2>/dev/null";
                        int cprc = ::system(cmd.c_str());
                        logInfo("tau/doMount fallback copy: cmd=", cmd, " rc=", cprc);
                    }
                }
                if (!upper.isEmpty() && upper != target) {
                    String cmd = String("cp -a '") + upper + "/.' '" + target + "/' 2>/dev/null";
                    (void)::system(cmd.c_str());
                }
            }
        }
    }
    return true;
}





// ─── dropCapabilities ─────────────────────────────────────────────────────────

bool Namespace::dropCapabilities() {
    for (int cap = 0; cap <= 40; ++cap)
        ::prctl(PR_CAPBSET_DROP, cap, 0, 0, 0);

    struct cap_header hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct cap_data   dat[2] = {};
    if (::syscall(SYS_capset, &hdr, dat) != 0) {
        logWarn("tau: capset failed: ", ::strerror(errno));
        return false;
    }
    return true;
}

// ─── ensureRootfsStubs ────────────────────────────────────────────────────────

bool Namespace::ensureRootfsStubs(const String &p) {
    mkdirP(p + "/tmp");
    ::chmod((p + "/tmp").c_str(), 01777);
    mkdirP(p + "/proc");
    mkdirP(p + "/dev");
    mkdirP(p + "/etc");
    if (!pathExists(p + "/etc/resolv.conf")) {
        writeProc((p + "/etc/resolv.conf").c_str(), "nameserver 1.1.1.1\nnameserver 8.8.8.8\n");
    }
    if (!pathExists(p + "/etc/hosts")) {
        writeProc((p + "/etc/hosts").c_str(), "127.0.0.1 localhost\n::1 localhost\n");
    }
    return true;
}


// ─── mountProc ────────────────────────────────────────────────────────────────

bool Namespace::mountProc(const String &p) {
    String proc = p + "/proc";
    mkdirP(proc);
    if (::mount("proc", proc.c_str(), "proc",
                MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) != 0) {
        logWarn("tau: mount proc at ", proc, " (non-fatal): ", ::strerror(errno));
    }
    return true;
}

} // namespace Tau
