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
#include <pwd.h>
#include <grp.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <mntent.h>

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

static bool getSubIDRange(const char *subFile, uid_t uid, const char *username, unsigned int &subId, unsigned int &count) {
    FILE *f = ::fopen(subFile, "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (::fgets(line, sizeof(line), f)) {
        char name[64] = {};
        unsigned int start = 0, cnt = 0;
        if (::sscanf(line, "%63[^:]:%u:%u", name, &start, &cnt) == 3) {
            if ((username && ::strcmp(name, username) == 0) ||
                (::atoi(name) == (int)uid && ::strcmp(name, "0") != 0)) {
                subId = start;
                count = cnt;
                found = true;
                break;
            }
        }
    }
    ::fclose(f);
    return found;
}

// ─── enterUser ────────────────────────────────────────────────────────────────

bool Namespace::enterUser(IsolationContext &ctx) {
    if (ctx.hasUserNS) return true;

    uid_t realUID = ::getuid();
    gid_t realGID = ::getgid();

    if (realUID == 0) {
        // Already root: no user namespace mapping needed, just private mount/uts/ipc
        if (::unshare(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC) != 0) {
            (void)::unshare(CLONE_NEWNS);
        }
        // Crucial: make mount propagation strictly private to isolate from host
        (void)::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
        ctx.hasUserNS  = true;
        ctx.hasMountNS = true;
        return true;
    }

    bool hasNewIDMap = (pathExists("/usr/bin/newuidmap") || ::system("which newuidmap >/dev/null 2>&1") == 0) &&
                       (pathExists("/usr/bin/newgidmap") || ::system("which newgidmap >/dev/null 2>&1") == 0);

    if (hasNewIDMap) {
        const char *username = nullptr;
        struct passwd *pw = ::getpwuid(realUID);
        if (pw) username = pw->pw_name;

        unsigned int subUID = 0, subUIDCount = 0;
        unsigned int subGID = 0, subGIDCount = 0;
        bool hasSubUID = getSubIDRange("/etc/subuid", realUID, username, subUID, subUIDCount);
        bool hasSubGID = getSubIDRange("/etc/subgid", realGID, username, subGID, subGIDCount);

        int toHelper[2];
        int fromHelper[2];
        if (::pipe(toHelper) == 0 && ::pipe(fromHelper) == 0) {
            pid_t targetPid = ::getpid();
            pid_t helper = ::fork();
            if (helper == 0) {
                ::close(toHelper[1]);
                ::close(fromHelper[0]);

                char dummy = 0;
                (void)::read(toHelper[0], &dummy, 1);
                ::close(toHelper[0]);

                char ucmd[512], gcmd[512];
                if (hasSubUID && subUIDCount > 0) {
                    ::snprintf(ucmd, sizeof(ucmd), "newuidmap %d 0 %u 1 1 %u %u >/dev/null 2>&1",
                               (int)targetPid, (unsigned)realUID, subUID, subUIDCount);
                } else {
                    ::snprintf(ucmd, sizeof(ucmd), "newuidmap %d 0 %u 1 >/dev/null 2>&1",
                               (int)targetPid, (unsigned)realUID);
                }

                if (hasSubGID && subGIDCount > 0) {
                    ::snprintf(gcmd, sizeof(gcmd), "newgidmap %d 0 %u 1 1 %u %u >/dev/null 2>&1",
                               (int)targetPid, (unsigned)realGID, subGID, subGIDCount);
                } else {
                    ::snprintf(gcmd, sizeof(gcmd), "newgidmap %d 0 %u 1 >/dev/null 2>&1",
                               (int)targetPid, (unsigned)realGID);
                }

                int urc = ::system(ucmd);
                int grc = ::system(gcmd);
                if (urc == 0 && grc == 0) {
                    (void)::write(fromHelper[1], "O", 1);
                } else {
                    (void)::write(fromHelper[1], "F", 1);
                }
                ::close(fromHelper[1]);
                ::_exit(0);
            }

            ::close(toHelper[0]);
            ::close(fromHelper[1]);

            if (::unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC) != 0) {
                if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
                    logError("tau: unshare(CLONE_NEWUSER|CLONE_NEWNS): ", ::strerror(errno));
                    ::close(toHelper[1]);
                    ::close(fromHelper[0]);
                    int st; ::waitpid(helper, &st, 0);
                    return false;
                }
            }

            (void)::write(toHelper[1], "U", 1);
            ::close(toHelper[1]);

            char status = 0;
            (void)::read(fromHelper[0], &status, 1);
            ::close(fromHelper[0]);
            int st; ::waitpid(helper, &st, 0);

            if (status == 'O') {
                (void)::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
                ctx.hasUserNS  = true;
                ctx.hasMountNS = true;
                return true;
            }
        }
    }

    if (!ctx.hasUserNS) {
        if (::unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC) != 0) {
            if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
                logError("tau: unshare(CLONE_NEWUSER|CLONE_NEWNS): ", ::strerror(errno));
                return false;
            }
        }
    }

    if (!writeIDMaps(realUID, realGID)) return false;

    (void)::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
    ctx.hasUserNS  = true;
    ctx.hasMountNS = true;
    return true;
}


// ─── writeIDMaps ──────────────────────────────────────────────────────────────

bool Namespace::writeIDMaps(uid_t realUID, gid_t realGID) {
    pid_t self = ::getpid();

    // Direct proc fallback (requires setgroups deny)
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


bool Namespace::enterPID(IsolationContext &ctx) {
    if (ctx.hasPidNS) return true;
    if (::unshare(CLONE_NEWPID) != 0) {
        logWarn("tau: unshare(CLONE_NEWPID): ", ::strerror(errno));
        return false;
    }
    ctx.hasPidNS = true;
    return true;
}

// ─── Host Namespace Helper ───────────────────────────────────────────────────

static int s_hostToHelper[2] = { -1, -1 };
static int s_hostFromHelper[2] = { -1, -1 };
static pid_t s_hostHelperPid = -1;

void Namespace::initHostHelper() {
    if (s_hostHelperPid > 0) return;
    if (::pipe(s_hostToHelper) != 0 || ::pipe(s_hostFromHelper) != 0) return;
    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid == 0) {
        ::close(s_hostToHelper[1]);
        ::close(s_hostFromHelper[0]);
        char buf[1024];
        while (true) {
            ssize_t n = ::read(s_hostToHelper[0], buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            String line = String(buf).trim();
            if (line == "EXIT" || line.isEmpty()) break;

            if (line.startsWith("VETH ")) {
                Array<String> parts = line.split(" ");
                if (parts.length() >= 3) {
                    String cmd = String("ip link add ") + parts[1] + " type veth peer name " + parts[2] + " 2>/dev/null";
                    int rc = ::system(cmd.c_str());
                    const char *resp = (rc == 0) ? "OK\n" : "ERR\n";
                    (void)::write(s_hostFromHelper[1], resp, ::strlen(resp));
                }
            } else if (line.startsWith("MOVE ")) {
                Array<String> parts = line.split(" ");
                if (parts.length() >= 3) {
                    String cmd = String("ip link set ") + parts[1] + " netns " + parts[2] + " 2>/dev/null";
                    int rc = ::system(cmd.c_str());
                    const char *resp = (rc == 0) ? "OK\n" : "ERR\n";
                    (void)::write(s_hostFromHelper[1], resp, ::strlen(resp));
                }
            } else if (line.startsWith("IP ")) {
                String sub = line.substring(3);
                String cmd = String("ip ") + sub + " 2>/dev/null";
                int rc = ::system(cmd.c_str());
                const char *resp = (rc == 0) ? "OK\n" : "ERR\n";
                (void)::write(s_hostFromHelper[1], resp, ::strlen(resp));
            } else if (line.startsWith("DEL ")) {
                String iface = line.substring(4).trim();
                String cmd = String("ip link del ") + iface + " 2>/dev/null";
                (void)::system(cmd.c_str());
                (void)::write(s_hostFromHelper[1], "OK\n", 3);
            } else if (line.startsWith("LINK_EXISTS ")) {
                String iface = line.substring(12).trim();
                String path = "/sys/class/net/" + iface;
                bool ex = (::access(path.c_str(), F_OK) == 0);
                if (!ex) {
                    String cmd = "ip link show dev " + iface + " >/dev/null 2>&1";
                    ex = (::system(cmd.c_str()) == 0);
                }
                const char *resp = ex ? "YES\n" : "NO\n";
                (void)::write(s_hostFromHelper[1], resp, ::strlen(resp));
            } else if (line.startsWith("BRIDGE_CREATE ")) {
                Array<String> parts = line.split(" ");
                if (parts.length() >= 2) {
                    String brName = parts[1];
                    String cmd = "ip link add name " + brName + " type bridge forward_delay 0 stp_state 0 2>/dev/null";
                    (void)::system(cmd.c_str());
                    if (parts.length() >= 3 && !parts[2].isEmpty()) {
                        String addrCmd = "ip addr add " + parts[2] + " dev " + brName + " 2>/dev/null";
                        (void)::system(addrCmd.c_str());
                    }
                    String upCmd = "ip link set " + brName + " up 2>/dev/null";
                    int rc = ::system(upCmd.c_str());
                    const char *resp = (rc == 0) ? "OK\n" : "ERR\n";
                    (void)::write(s_hostFromHelper[1], resp, ::strlen(resp));
                }
            } else if (line.startsWith("BRIDGE_JOIN ")) {
                Array<String> parts = line.split(" ");
                if (parts.length() >= 3) {
                    String brName = parts[1];
                    String target = parts[2];
                    String upTarget = "ip link set dev " + target + " up 2>/dev/null";
                    (void)::system(upTarget.c_str());
                    String joinCmd = "ip link set dev " + target + " master " + brName + " 2>/dev/null";
                    int rc = ::system(joinCmd.c_str());
                    const char *resp = (rc == 0) ? "OK\n" : "ERR\n";
                    (void)::write(s_hostFromHelper[1], resp, ::strlen(resp));
                }
            } else if (line.startsWith("BRIDGE_UNJOIN ")) {
                String target = line.substring(14).trim();
                String unjoinCmd = "ip link set dev " + target + " nomaster 2>/dev/null";
                (void)::system(unjoinCmd.c_str());
                (void)::write(s_hostFromHelper[1], "OK\n", 3);
            } else if (line.startsWith("BRIDGE_CLEANUP ")) {
                Array<String> parts = line.split(" ");
                if (parts.length() >= 2) {
                    String brName = parts[1];
                    String brifPath = "/sys/class/net/" + brName + "/brif";
                    DIR *d = ::opendir(brifPath.c_str());
                    bool hasOtherIfaces = false;
                    if (d) {
                        struct dirent *ent;
                        while ((ent = ::readdir(d)) != nullptr) {
                            if (ent->d_name[0] == '.') continue;
                            String attachedIface(ent->d_name);
                            bool isKnown = false;
                            for (size_t k = 2; k < parts.length(); ++k) {
                                if (attachedIface == parts[k]) { isKnown = true; break; }
                            }
                            if (!isKnown) {
                                hasOtherIfaces = true;
                                break;
                            }
                        }
                        ::closedir(d);
                    }
                    if (!hasOtherIfaces) {
                        String delCmd = "ip link del " + brName + " 2>/dev/null";
                        (void)::system(delCmd.c_str());
                    }
                    (void)::write(s_hostFromHelper[1], "OK\n", 3);
                }
            }
        }
        ::close(s_hostToHelper[0]);
        ::close(s_hostFromHelper[1]);
        ::_exit(0);
    }
    ::close(s_hostToHelper[0]);
    ::close(s_hostFromHelper[1]);
    s_hostHelperPid = pid;
}

bool Namespace::linkExists(const String &iface) {
    if (s_hostHelperPid > 0) {
        String msg = "LINK_EXISTS " + iface + "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        ssize_t n = ::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
        if (n > 0 && ::strncmp(resp, "YES", 3) == 0) return true;
        if (n > 0 && ::strncmp(resp, "NO", 2) == 0) return false;
    }
    String path = "/sys/class/net/" + iface;
    if (::access(path.c_str(), F_OK) == 0) return true;
    String cmd = "ip link show dev " + iface + " >/dev/null 2>&1";
    return (::system(cmd.c_str()) == 0);
}

bool Namespace::hostCreateBridge(const String &bridgeName, const String &address) {
    if (s_hostHelperPid > 0) {
        String msg = "BRIDGE_CREATE " + bridgeName + (address.isEmpty() ? "" : (" " + address)) + "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        ssize_t n = ::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
        if (n > 0 && ::strncmp(resp, "OK", 2) == 0) return true;
    }
    String cmd = "ip link add name " + bridgeName + " type bridge forward_delay 0 stp_state 0 2>/dev/null";
    (void)::system(cmd.c_str());
    if (!address.isEmpty()) {
        String addrCmd = "ip addr add " + address + " dev " + bridgeName + " 2>/dev/null";
        (void)::system(addrCmd.c_str());
    }
    String upCmd = "ip link set " + bridgeName + " up 2>/dev/null";
    return (::system(upCmd.c_str()) == 0);
}

bool Namespace::hostJoinBridge(const String &bridgeName, const String &target) {
    if (s_hostHelperPid > 0) {
        String msg = "BRIDGE_JOIN " + bridgeName + " " + target + "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        ssize_t n = ::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
        if (n > 0 && ::strncmp(resp, "OK", 2) == 0) return true;
    }
    String upTarget = "ip link set dev " + target + " up 2>/dev/null";
    (void)::system(upTarget.c_str());
    String joinCmd = "ip link set dev " + target + " master " + bridgeName + " 2>/dev/null";
    return (::system(joinCmd.c_str()) == 0);
}

bool Namespace::hostUnjoinBridge(const String &target) {
    if (s_hostHelperPid > 0) {
        String msg = "BRIDGE_UNJOIN " + target + "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        (void)::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
        return true;
    }
    String unjoinCmd = "ip link set dev " + target + " nomaster 2>/dev/null";
    return (::system(unjoinCmd.c_str()) == 0);
}

bool Namespace::hostDeleteBridgeIfEmpty(const String &bridgeName, const Array<String> &instanceLinks) {
    if (s_hostHelperPid > 0) {
        String msg = "BRIDGE_CLEANUP " + bridgeName;
        for (size_t i = 0; i < instanceLinks.length(); ++i) {
            msg += " " + instanceLinks[i];
        }
        msg += "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        (void)::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
        return true;
    }
    String brifPath = "/sys/class/net/" + bridgeName + "/brif";
    DIR *d = ::opendir(brifPath.c_str());
    bool hasOtherIfaces = false;
    if (d) {
        struct dirent *ent;
        while ((ent = ::readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            String attachedIface(ent->d_name);
            bool isKnown = false;
            for (size_t k = 0; k < instanceLinks.length(); ++k) {
                if (attachedIface == instanceLinks[k]) { isKnown = true; break; }
            }
            if (!isKnown) { hasOtherIfaces = true; break; }
        }
        ::closedir(d);
    }
    if (!hasOtherIfaces) {
        String delCmd = "ip link del " + bridgeName + " 2>/dev/null";
        (void)::system(delCmd.c_str());
    }
    return true;
}

bool Namespace::hostCreateVeth(const String &hostName, const String &peerName) {
    if (s_hostHelperPid > 0) {
        String msg = "VETH " + hostName + " " + peerName + "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        ssize_t n = ::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
        if (n > 0 && ::strncmp(resp, "OK", 2) == 0) return true;
    }
    String cmd = String("ip link add ") + hostName + " type veth peer name " + peerName + " 2>/dev/null";
    return (::system(cmd.c_str()) == 0);
}

bool Namespace::hostMoveNetns(const String &iface, pid_t targetPid) {
    if (s_hostHelperPid > 0) {
        String msg = "MOVE " + iface + " " + intStr(targetPid) + "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        ssize_t n = ::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
        if (n > 0 && ::strncmp(resp, "OK", 2) == 0) return true;
    }
    String cmd = String("ip link set ") + iface + " netns " + intStr(targetPid) + " 2>/dev/null";
    return (::system(cmd.c_str()) == 0);
}

bool Namespace::hostRunIP(const String &rawCmd) {
    if (s_hostHelperPid > 0) {
        String msg = "IP " + rawCmd + "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        ssize_t n = ::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
        if (n > 0 && ::strncmp(resp, "OK", 2) == 0) return true;
    }
    String cmd = String("ip ") + rawCmd + " 2>/dev/null";
    return (::system(cmd.c_str()) == 0);
}

void Namespace::hostCleanupLink(const String &iface) {
    if (s_hostHelperPid > 0) {
        String msg = "DEL " + iface + "\n";
        (void)::write(s_hostToHelper[1], msg.c_str(), msg.length());
        char resp[16] = {};
        (void)::read(s_hostFromHelper[0], resp, sizeof(resp) - 1);
    }
    String cmd = String("ip link del ") + iface + " 2>/dev/null";
    (void)::system(cmd.c_str());
}

void Namespace::shutdownHostHelper() {
    if (s_hostHelperPid > 0) {
        (void)::write(s_hostToHelper[1], "EXIT\n", 5);
        ::close(s_hostToHelper[1]);
        ::close(s_hostFromHelper[0]);
        int st;
        ::waitpid(s_hostHelperPid, &st, 0);
        s_hostHelperPid = -1;
    }
}

// ─── enterNet ─────────────────────────────────────────────────────────────────

bool Namespace::enterNet(const Array<String> &allowList, IsolationContext &ctx) {
    if (ctx.hasNetNS) return true;

    pid_t self = ::getpid();

    if (::unshare(CLONE_NEWNET) != 0) {
        logError("tau: unshare(CLONE_NEWNET): ", ::strerror(errno));
        return false;
    }
    ctx.hasNetNS = true;

    // Move allowed interfaces into this new netns using the host helper
    for (size_t i = 0; i < allowList.length(); ++i) {
        hostMoveNetns(allowList[i], self);
    }

    (void)::system("ip link set lo up 2>/dev/null");

    for (size_t i = 0; i < allowList.length(); ++i) {
        String upCmd = String("ip link set ") + allowList[i] + " up 2>/dev/null";
        (void)::system(upCmd.c_str());
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
    return hostCreateVeth(hostName, peerName);
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
        long long sStart = Xi::parseLong(srcPort.substring(0, (size_t)srcDash));
        long long sEnd   = Xi::parseLong(srcPort.substring((size_t)srcDash + 1));
        long long span   = sEnd - sStart;
        long long dStart = Xi::parseLong(dstPort);
        if (span > 0 && dStart > 0) {
            dstPort = intStr(dStart) + "-" + intStr(dStart + span);
        }
    } else if (dstDash >= 0 && srcDash < 0) {
        // e.g. dstPort="2000-2010", srcPort="1000" -> srcPort="1000-1010"
        long long dStart = Xi::parseLong(dstPort.substring(0, (size_t)dstDash));
        long long dEnd   = Xi::parseLong(dstPort.substring((size_t)dstDash + 1));
        long long span   = dEnd - dStart;
        long long sStart = Xi::parseLong(srcPort);
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
    (void)read;
    mkdirP(target);

    if (workDir.isEmpty()) {
        unsigned long flags = MS_BIND | MS_REC;
        if (!write) flags |= MS_RDONLY;
        if (::mount(source.c_str(), target.c_str(), nullptr, flags, nullptr) != 0) {
            String cmd = String("cp -a --no-preserve=ownership '") + source + "/.' '" + target + "/' 2>/dev/null";
            ::system(cmd.c_str());
        }
    } else {
        String upper = upperDir.isEmpty() ? (workDir + "/upper") : upperDir;
        String work  = workDir + "/work";
        String cleanWork = String("rm -rf '") + work + "' 2>/dev/null";
        (void)::system(cleanWork.c_str());
        mkdirP(upper);
        mkdirP(work);

        String opts = String("lowerdir=") + source +
                      ",upperdir=" + upper +
                      ",workdir=" + work +
                      ",index=off,metacopy=off";
        int rc = ::mount("overlay", target.c_str(), "overlay", 0, opts.c_str());
        if (rc != 0) {
            opts = String("lowerdir=") + source +
                   ",upperdir=" + upper +
                   ",workdir=" + work +
                   ",index=off";
            rc = ::mount("overlay", target.c_str(), "overlay", 0, opts.c_str());
        }
        if (rc != 0) {
            opts = String("lowerdir=") + source +
                   ",upperdir=" + upper +
                   ",workdir=" + work +
                   ",index=off,userxattr";
            rc = ::mount("overlay", target.c_str(), "overlay", 0, opts.c_str());
        }
        if (rc != 0) {
            opts = String("lowerdir=") + source +
                   ",upperdir=" + upper +
                   ",workdir=" + work;
            rc = ::mount("overlay", target.c_str(), "overlay", 0, opts.c_str());
        }
        if (rc != 0) {
            opts = String("lowerdir=") + source +
                   ",upperdir=" + upper +
                   ",workdir=" + work +
                   ",userxattr";
            rc = ::mount("overlay", target.c_str(), "overlay", 0, opts.c_str());
        }
        if (rc != 0) {
            String mntCmd = String("mount -t overlay overlay '") + target + "' -o 'lowerdir=" + source + ",upperdir=" + upper + ",workdir=" + work + ",index=off' >/dev/null 2>&1";
            rc = ::system(mntCmd.c_str());
        }
        if (rc != 0) {
            String mntCmd = String("mount -t overlay overlay '") + target + "' -o 'lowerdir=" + source + ",upperdir=" + upper + ",workdir=" + work + ",index=off,userxattr' >/dev/null 2>&1";
            rc = ::system(mntCmd.c_str());
        }
        logInfo("tau/doMount overlay: target=", target, " opts=", opts, " rc=", rc, " err=", ::strerror(errno));
        if (rc != 0) {
            // Fall back for unprivileged user namespaces on kernels without user-overlayfs
            Array<String> lowers = source.split(":");
            for (long long i = (long long)lowers.length() - 1; i >= 0; --i) {
                if (!lowers[(size_t)i].isEmpty()) {
                    String cmd = String("cp -a --no-preserve=ownership '") + lowers[(size_t)i] + "/.' '" + target + "/' 2>/dev/null";
                    int cprc = ::system(cmd.c_str());
                    logInfo("tau/doMount fallback copy: cmd=", cmd, " rc=", cprc);
                }
            }
            if (!upper.isEmpty() && upper != target) {
                String cmd = String("cp -a --no-preserve=ownership '") + upper + "/.' '" + target + "/' 2>/dev/null";
                (void)::system(cmd.c_str());
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
    mkdirP(p + "/sys");
    mkdirP(p + "/dev");

    // Check if /dev or any device nodes are already mounted in rootfs
    bool devMounted = false;
    FILE *mf = ::setmntent("/proc/mounts", "r");
    if (!mf) mf = ::setmntent("/etc/mtab", "r");
    if (mf) {
        struct mntent *me;
        String devPath = p + "/dev";
        while ((me = ::getmntent(mf)) != nullptr) {
            if (me->mnt_dir) {
                String md(me->mnt_dir);
                if (md == devPath || md == (devPath + "/null") || (p == "/" && md == "/dev")) {
                    devMounted = true;
                    break;
                }
            }
        }
        ::endmntent(mf);
    }

    if (!devMounted) {
        // Mount tmpfs on container /dev so OpenRC/init detects /dev is already mounted
        // and avoids attempting devtmpfs (which fails with EPERM in rootless user namespaces).
        (void)::mount("tmpfs", (p + "/dev").c_str(), "tmpfs", MS_NOSUID, "mode=0755");

        mkdirP(p + "/dev/pts");
        mkdirP(p + "/dev/shm");

        // Bind mount essential host device nodes into container /dev
        const char *devNodes[] = { "null", "zero", "full", "random", "urandom" };
        for (const char *node : devNodes) {
            String targetNode = p + "/dev/" + node;
            String hostNode   = String("/dev/") + node;
            if (!pathExists(targetNode)) {
                int fd = ::open(targetNode.c_str(), O_CREAT | O_WRONLY, 0666);
                if (fd >= 0) ::close(fd);
            }
            (void)::mount(hostNode.c_str(), targetNode.c_str(), nullptr, MS_BIND, nullptr);
        }

        // Mount devpts with rootless container options
        String ptsDir = p + "/dev/pts";
        mkdirP(ptsDir);
        (void)::mount("devpts", ptsDir.c_str(), "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=620");
        ::unlink((p + "/dev/ptmx").c_str());
        (void)::symlink("/dev/pts/ptmx", (p + "/dev/ptmx").c_str());
    }

    mkdirP(p + "/dev/pts");
    mkdirP(p + "/dev/shm");
    mkdirP(p + "/run");
    mkdirP(p + "/etc");

    if (!pathExists(p + "/etc/resolv.conf")) {
        writeProc((p + "/etc/resolv.conf").c_str(), "nameserver 1.1.1.1\nnameserver 8.8.8.8\n");
    }
    if (!pathExists(p + "/etc/hosts")) {
        writeProc((p + "/etc/hosts").c_str(), "127.0.0.1 localhost\n::1 localhost\n");
    }

    String hnameFile = p + "/etc/hostname";
    if (pathExists(hnameFile)) {
        int fd = ::open(hnameFile.c_str(), O_RDONLY);
        if (fd >= 0) {
            char hbuf[256] = {};
            ssize_t n = ::read(fd, hbuf, sizeof(hbuf) - 1);
            ::close(fd);
            if (n > 0) {
                while (n > 0 && (hbuf[n - 1] == '\n' || hbuf[n - 1] == '\r' || hbuf[n - 1] == ' ')) {
                    hbuf[--n] = '\0';
                }
                if (n > 0) {
                    (void)::sethostname(hbuf, (size_t)n);
                }
            }
        }
    } else {
        writeProc(hnameFile.c_str(), "saas-container-1\n");
        (void)::sethostname("saas-container-1", 16);
    }

    String ifaceFile = p + "/etc/network/interfaces";
    if (pathExists(p + "/etc/network") && !pathExists(ifaceFile)) {
        writeProc(ifaceFile.c_str(), "auto lo\niface lo inet loopback\n");
    }

    // Link standard standard I/O handles
    (void)::symlink("/proc/self/fd", (p + "/dev/fd").c_str());
    (void)::symlink("/proc/self/fd/0", (p + "/dev/stdin").c_str());
    (void)::symlink("/proc/self/fd/1", (p + "/dev/stdout").c_str());
    (void)::symlink("/proc/self/fd/2", (p + "/dev/stderr").c_str());

    ::unlink((p + "/dev/tty").c_str());
    (void)::symlink("/proc/self/fd/0", (p + "/dev/tty").c_str());

    for (int t = 1; t <= 6; ++t) {
        String ttyNode = p + "/dev/tty" + intStr(t);
        ::unlink(ttyNode.c_str());
        (void)::symlink("/dev/null", ttyNode.c_str());
    }
    ::unlink((p + "/dev/console").c_str());
    (void)::symlink("/proc/self/fd/0", (p + "/dev/console").c_str());

    // Remove custom preloads or wrappers if previously created
    ::unlink((p + "/etc/ld.so.preload").c_str());
    ::unlink((p + "/lib/libnosetgroups.so").c_str());
    ::unlink((p + "/sbin/container-login").c_str());

    // Adapt inittab for container console with authentic vanilla /bin/login
    String inittabPath = p + "/etc/inittab";
    if (pathExists(inittabPath)) {
        String sedCmd = String("sed -i '/::respawn:/d' '") + inittabPath + "' 2>/dev/null";
        (void)::system(sedCmd.c_str());
        String addCmd = String("echo '::respawn:/bin/login' >> '") + inittabPath + "' 2>/dev/null";
        (void)::system(addCmd.c_str());
    }

    // Allow root login on container PTYs
    ::unlink((p + "/etc/securetty").c_str());

    return true;
}


// ─── cleanupRootfsStubs ───────────────────────────────────────────────────────

bool Namespace::cleanupRootfsStubs(const String &p) {
    if (p.isEmpty() || p == "/" || p == "/root" || p == "/home" || p == "/dev" || p == "/proc" || p == "/sys" || p == "/run") {
        return true;
    }

    // Unmount all dev nodes and pseudo-filesystems in reverse order
    const char *subMounts[] = {
        "/dev/pts",
        "/dev/null",
        "/dev/zero",
        "/dev/full",
        "/dev/random",
        "/dev/urandom",
        "/dev",
        "/proc",
        "/sys"
    };

    for (const char *sub : subMounts) {
        String target = p + sub;
        if (target == "/proc" || target == "/dev" || target == "/sys") continue;
        (void)::umount2(target.c_str(), MNT_DETACH);
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
