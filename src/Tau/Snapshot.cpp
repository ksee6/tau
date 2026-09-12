/**
 * @file Snapshot.cpp
 * @brief Implementation of process memory & socket snapshotting for Tau.
 */

#include <Tau/Snapshot.hpp>
#include <Tau/Cgroup.hpp>
#include <Tau/Util.hpp>
#include <Data/Yaml.hpp>
#include <Resource/File.hpp>

#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/un.h>

#ifndef TCP_REPAIR
#define TCP_REPAIR 19
#endif
#ifndef TCP_REPAIR_QUEUE
#define TCP_REPAIR_QUEUE 20
#endif
#ifndef TCP_QUEUE_SEQ
#define TCP_QUEUE_SEQ 21
#endif
#ifndef TCP_REPAIR_OPTIONS
#define TCP_REPAIR_OPTIONS 22
#endif
#ifndef TCP_NO_QUEUE
#define TCP_NO_QUEUE 0
#endif
#ifndef TCP_RECV_QUEUE
#define TCP_RECV_QUEUE 1
#endif
#ifndef TCP_SEND_QUEUE
#define TCP_SEND_QUEUE 2
#endif
#ifndef TCP_QUEUES_NR
#define TCP_QUEUES_NR 3
#endif

namespace Tau {

using namespace Data;

static String generateTimestamp() {
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    unsigned long long ms = (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
    return intStr(ms);
}

static void collectDescendantPids(pid_t rootPid, Array<pid_t> &outPids) {
    if (rootPid <= 0) return;
    outPids.push(rootPid);

    DIR *procDir = ::opendir("/proc");
    if (!procDir) return;

    struct ProcEntry { pid_t pid; pid_t ppid; };
    Array<ProcEntry> allProcs;

    struct dirent *entry;
    while ((entry = ::readdir(procDir)) != nullptr) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        pid_t p = (pid_t)::atoi(entry->d_name);
        if (p <= 0) continue;

        char statPath[64];
        ::snprintf(statPath, sizeof(statPath), "/proc/%d/stat", p);
        int fd = ::open(statPath, O_RDONLY);
        if (fd < 0) continue;
        char buf[512];
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';

        const char *rparen = ::strrchr(buf, ')');
        if (!rparen) continue;
        char state;
        int ppid = 0;
        if (::sscanf(rparen + 1, " %c %d", &state, &ppid) == 2) {
            allProcs.push({ p, (pid_t)ppid });
        }
    }
    ::closedir(procDir);

    bool added = true;
    while (added) {
        added = false;
        for (size_t i = 0; i < allProcs.length(); ++i) {
            pid_t candPid = allProcs[i].pid;
            pid_t candPpid = allProcs[i].ppid;
            bool parentFound = false;
            for (size_t j = 0; j < outPids.length(); ++j) {
                if (outPids[j] == candPpid) { parentFound = true; break; }
            }
            if (parentFound) {
                bool alreadyIn = false;
                for (size_t j = 0; j < outPids.length(); ++j) {
                    if (outPids[j] == candPid) { alreadyIn = true; break; }
                }
                if (!alreadyIn) {
                    outPids.push(candPid);
                    added = true;
                }
            }
        }
    }
}

Array<pid_t> Snapshot::collectInstancePids(const InstanceState &state) {
    Array<pid_t> pids;
    for (size_t i = 0; i < state.spawns.length(); ++i) {
        if (state.spawns[i].pid > 0 && state.spawns[i].pid != state.spawnPID) {
            bool found = false;
            for (size_t j = 0; j < pids.length(); ++j) {
                if (pids[j] == state.spawns[i].pid) { found = true; break; }
            }
            if (!found) {
                collectDescendantPids(state.spawns[i].pid, pids);
            }
        }
    }
    return pids;
}

static void dumpProcessMemory(pid_t pid,
                              const String &spawnName,
                              int memDataFd,
                              uint64_t &currentOffset,
                              ProcessMemoryDump &outDump,
                              const ProcessMemoryDump *prevDump,
                              int prevMemFd) {
    outDump.pid = pid;
    outDump.spawnName = spawnName;

    char mapsPath[64];
    ::snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);
    FILE *fmaps = ::fopen(mapsPath, "r");
    if (!fmaps) return;

    char memPath[64];
    ::snprintf(memPath, sizeof(memPath), "/proc/%d/mem", pid);
    int memFd = ::open(memPath, O_RDONLY);
    if (memFd < 0) {
        ::fclose(fmaps);
        return;
    }

    char line[1024];
    while (::fgets(line, sizeof(line), fmaps)) {
        uint64_t start = 0, end = 0;
        char perms[16] = {};
        unsigned long long offset = 0;
        int devMajor = 0, devMinor = 0;
        unsigned long long inode = 0;
        char pathname[512] = {};

        int scanned = ::sscanf(line, "%llx-%llx %15s %llx %x:%x %llu %511[^\n]",
                               (unsigned long long *)&start, (unsigned long long *)&end, perms, &offset, &devMajor, &devMinor, &inode, pathname);
        if (scanned < 3) continue;

        String pathStr(pathname);
        pathStr = pathStr.trim();

        // Skip non-readable/non-writable special device or read-only file mappings if not anon/stack/heap
        if (perms[0] != 'r') continue;
        if (pathStr == "[vsyscall]" || pathStr == "[vdso]" || pathStr == "[vvar]") continue;

        uint64_t segLen = (end > start) ? (end - start) : 0;
        if (segLen == 0 || segLen > (1024ULL * 1024ULL * 1024ULL)) continue; // skip abnormal segments

        MemorySegment seg;
        seg.start = start;
        seg.end = end;
        seg.perms = String(perms);
        seg.pathname = pathStr;

        // Check if matching segment exists in previous dump
        bool canDelta = false;
        if (prevDump && prevMemFd >= 0) {
            for (size_t k = 0; k < prevDump->segments.length(); ++k) {
                const auto &ps = prevDump->segments[k];
                if (ps.start == start && ps.end == end && ps.dataSize == segLen) {
                    // Compare memory contents
                    std::vector<char> curBuf((size_t)segLen);
                    std::vector<char> prevBuf((size_t)segLen);
                    ssize_t r1 = ::pread(memFd, curBuf.data(), (size_t)segLen, (off_t)start);
                    ssize_t r2 = ::pread(prevMemFd, prevBuf.data(), (size_t)segLen, (off_t)ps.dataOffset);
                    if (r1 == (ssize_t)segLen && r2 == (ssize_t)segLen &&
                        ::memcmp(curBuf.data(), prevBuf.data(), (size_t)segLen) == 0) {
                        seg.isDelta = true;
                        seg.dataOffset = ps.dataOffset;
                        seg.dataSize = segLen;
                        canDelta = true;
                    }
                    break;
                }
            }
        }

        if (!canDelta) {
            std::vector<char> buf((size_t)segLen);
            ssize_t rn = ::pread(memFd, buf.data(), (size_t)segLen, (off_t)start);
            if (rn > 0) {
                seg.isDelta = false;
                seg.dataOffset = currentOffset;
                seg.dataSize = (uint64_t)rn;
                ::write(memDataFd, buf.data(), (size_t)rn);
                currentOffset += (uint64_t)rn;
            }
        }

        outDump.segments.push(seg);
    }

    ::close(memFd);
    ::fclose(fmaps);
}

static void dumpProcessSockets(pid_t pid, const String &spawnName, Array<SocketInfo> &outSockets) {
    char fdDirPath[64];
    ::snprintf(fdDirPath, sizeof(fdDirPath), "/proc/%d/fd", pid);
    DIR *d = ::opendir(fdDirPath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        int fdNum = ::atoi(ent->d_name);
        char linkTarget[512] = {};
        char fdPath[512];
        ::snprintf(fdPath, sizeof(fdPath), "%s/%s", fdDirPath, ent->d_name);
        ssize_t len = ::readlink(fdPath, linkTarget, sizeof(linkTarget) - 1);
        if (len <= 0) continue;
        linkTarget[len] = '\0';

        String targetStr(linkTarget);
        if (targetStr.startsWith("socket:[")) {
            SocketInfo s;
            s.fd = fdNum;
            s.pid = pid;
            s.spawnName = spawnName;

            int sockFd = ::open(fdPath, O_RDONLY);
            if (sockFd >= 0) {
                struct sockaddr_storage addr = {};
                socklen_t addrLen = sizeof(addr);
                if (::getsockname(sockFd, (struct sockaddr *)&addr, &addrLen) == 0) {
                    if (addr.ss_family == AF_INET) {
                        s.family = "inet";
                        struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
                        char ipBuf[INET_ADDRSTRLEN] = {};
                        ::inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
                        s.localAddr = String(ipBuf);
                        s.localPort = (int)ntohs(sin->sin_port);
                    } else if (addr.ss_family == AF_INET6) {
                        s.family = "inet6";
                        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr;
                        char ipBuf[INET6_ADDRSTRLEN] = {};
                        ::inet_ntop(AF_INET6, &sin6->sin6_addr, ipBuf, sizeof(ipBuf));
                        s.localAddr = String(ipBuf);
                        s.localPort = (int)ntohs(sin6->sin6_port);
                    } else if (addr.ss_family == AF_UNIX) {
                        s.family = "unix";
                        struct sockaddr_un *sun = (struct sockaddr_un *)&addr;
                        s.unixPath = String(sun->sun_path);
                    }
                }

                struct sockaddr_storage peerAddr = {};
                socklen_t peerLen = sizeof(peerAddr);
                if (::getpeername(sockFd, (struct sockaddr *)&peerAddr, &peerLen) == 0) {
                    if (peerAddr.ss_family == AF_INET) {
                        struct sockaddr_in *sin = (struct sockaddr_in *)&peerAddr;
                        char ipBuf[INET_ADDRSTRLEN] = {};
                        ::inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
                        s.peerAddr = String(ipBuf);
                        s.peerPort = (int)ntohs(sin->sin_port);
                    } else if (peerAddr.ss_family == AF_INET6) {
                        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&peerAddr;
                        char ipBuf[INET6_ADDRSTRLEN] = {};
                        ::inet_ntop(AF_INET6, &sin6->sin6_addr, ipBuf, sizeof(ipBuf));
                        s.peerAddr = String(ipBuf);
                        s.peerPort = (int)ntohs(sin6->sin6_port);
                    }
                }

                int sockType = 0;
                socklen_t optLen = sizeof(sockType);
                if (::getsockopt(sockFd, SOL_SOCKET, SO_TYPE, &sockType, &optLen) == 0) {
                    if (sockType == SOCK_STREAM) s.type = (s.family == "unix") ? "stream" : "tcp";
                    else if (sockType == SOCK_DGRAM) s.type = (s.family == "unix") ? "dgram" : "udp";
                }

                // TCP repair mode inspection
                if (s.type == "tcp") {
                    int repairVal = 1;
                    if (::setsockopt(sockFd, IPPROTO_TCP, TCP_REPAIR, &repairVal, sizeof(repairVal)) == 0) {
                        s.isRepair = true;
                        uint32_t seq = 0;
                        socklen_t seqLen = sizeof(seq);
                        int q = TCP_SEND_QUEUE;
                        if (::setsockopt(sockFd, IPPROTO_TCP, TCP_REPAIR_QUEUE, &q, sizeof(q)) == 0) {
                            if (::getsockopt(sockFd, IPPROTO_TCP, TCP_QUEUE_SEQ, &seq, &seqLen) == 0) {
                                s.sndSeq = seq;
                            }
                        }
                        q = TCP_RECV_QUEUE;
                        if (::setsockopt(sockFd, IPPROTO_TCP, TCP_REPAIR_QUEUE, &q, sizeof(q)) == 0) {
                            if (::getsockopt(sockFd, IPPROTO_TCP, TCP_QUEUE_SEQ, &seq, &seqLen) == 0) {
                                s.rcvSeq = seq;
                            }
                        }
                        repairVal = 0;
                        ::setsockopt(sockFd, IPPROTO_TCP, TCP_REPAIR, &repairVal, sizeof(repairVal));
                    }
                }
                ::close(sockFd);
            }

            outSockets.push(s);
        }
    }
    ::closedir(d);
}

Array<String> Snapshot::list(const String &instanceDir) {
    Array<String> out;
    String snapDir = instanceDir + "/snapshots";
    DIR *d = ::opendir(snapDir.c_str());
    if (!d) return out;

    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        String name(ent->d_name);
        String ymlPath = snapDir + "/" + name + "/snapshot.yml";
        if (pathExists(ymlPath)) {
            out.push(name);
        }
    }
    ::closedir(d);

    // Sort numerically by timestamp
    for (size_t i = 0; i < out.length(); ++i) {
        for (size_t j = i + 1; j < out.length(); ++j) {
            unsigned long long t1 = (unsigned long long)parseLong(out[i]);
            unsigned long long t2 = (unsigned long long)parseLong(out[j]);
            if (t1 > t2) {
                String tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    return out;
}

String Snapshot::findMatching(const String &instanceDir, const String &targetTimestamp) {
    Array<String> snaps = list(instanceDir);
    if (snaps.length() == 0) return "";
    if (targetTimestamp.isEmpty()) {
        return snaps[snaps.length() - 1]; // latest
    }

    unsigned long long target = (unsigned long long)parseLong(targetTimestamp);
    String best = "";
    unsigned long long bestTs = 0;

    for (size_t i = 0; i < snaps.length(); ++i) {
        unsigned long long ts = (unsigned long long)parseLong(snaps[i]);
        if (ts <= target) {
            if (best.isEmpty() || ts > bestTs) {
                best = snaps[i];
                bestTs = ts;
            }
        }
    }

    if (best.isEmpty()) {
        best = snaps[0]; // fallback to oldest available
    }
    return best;
}

bool Snapshot::take(const String &instanceDir,
                    const InstanceState &state,
                    bool isFreeze,
                    String &outTimestamp,
                    const String &targetSpawn,
                    bool isSoft,
                    bool wol) {
    String timestamp = generateTimestamp();
    outTimestamp = timestamp;

    String snapshotsRoot = instanceDir + "/snapshots";
    mkdirP(snapshotsRoot);
    String targetDir = snapshotsRoot + "/" + timestamp;
    if (!mkdirP(targetDir)) return false;

    // Check for previous snapshot for delta calculation
    Array<String> existing = list(instanceDir);
    String prevTimestamp;
    if (existing.length() > 0) {
        if (!targetSpawn.isEmpty()) {
            for (long long idx = (long long)existing.length() - 1; idx >= 0; --idx) {
                SnapshotInfo checkInfo;
                if (load(instanceDir, existing[(size_t)idx], checkInfo)) {
                    if (checkInfo.targetSpawn == targetSpawn || checkInfo.targetSpawn.isEmpty()) {
                        prevTimestamp = existing[(size_t)idx];
                        break;
                    }
                }
            }
        } else {
            prevTimestamp = existing[existing.length() - 1];
        }
    }

    SnapshotInfo prevInfo;
    int prevMemFd = -1;
    if (!prevTimestamp.isEmpty()) {
        if (load(instanceDir, prevTimestamp, prevInfo)) {
            String prevMemPath = snapshotsRoot + "/" + (prevInfo.isDelta ? prevInfo.baseTimestamp : prevTimestamp) + "/memory.bin";
            prevMemFd = ::open(prevMemPath.c_str(), O_RDONLY);
        }
    }

    // Freeze instance or target spawn processes during snapshot
    bool cgroupFrozen = false;
    Array<pid_t> pids;
    if (!targetSpawn.isEmpty()) {
        for (size_t j = 0; j < state.spawns.length(); ++j) {
            if (state.spawns[j].name == targetSpawn && state.spawns[j].pid > 0) {
                collectDescendantPids(state.spawns[j].pid, pids);
            }
        }
    } else {
        cgroupFrozen = Cgroup::freeze(state.name, "", true);
        pids = collectInstancePids(state);
    }

    for (size_t i = 0; i < pids.length(); ++i) {
        if (pids[i] > 0) ::kill(pids[i], SIGSTOP);
    }

    String memBinPath = targetDir + "/memory.bin";
    int memDataFd = ::open(memBinPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    SnapshotInfo info;
    info.timestamp = timestamp;
    info.instanceName = state.name;
    info.manifestPath = state.manifestPath;
    info.targetSpawn = targetSpawn;
    info.isSoft = isSoft;
    info.wol = wol;
    info.isDelta = !prevTimestamp.isEmpty();
    info.baseTimestamp = prevTimestamp.isEmpty() ? timestamp : prevTimestamp;

    uint64_t currentOffset = 0;

    for (size_t i = 0; i < pids.length(); ++i) {
        pid_t pid = pids[i];
        if (pid <= 0) continue;

        String spawnName = targetSpawn;
        for (size_t j = 0; j < state.spawns.length(); ++j) {
            if (state.spawns[j].pid == pid) {
                spawnName = state.spawns[j].name;
                break;
            }
        }

        const ProcessMemoryDump *prevProcDump = nullptr;
        if (!prevTimestamp.isEmpty()) {
            for (size_t k = 0; k < prevInfo.processes.length(); ++k) {
                if (prevInfo.processes[k].pid == pid) {
                    prevProcDump = &prevInfo.processes[k];
                    break;
                }
            }
        }

        ProcessMemoryDump pDump;
        dumpProcessMemory(pid, spawnName, memDataFd, currentOffset, pDump, prevProcDump, prevMemFd);
        info.processes.push(pDump);

        dumpProcessSockets(pid, spawnName, info.sockets);
    }

    if (memDataFd >= 0) ::close(memDataFd);
    if (prevMemFd >= 0) ::close(prevMemFd);

    // Save snapshot.yml
    String yml;
    yml += "timestamp: " + info.timestamp + "\n";
    yml += String("is_delta: ") + (info.isDelta ? "true" : "false") + "\n";
    yml += "base_timestamp: " + info.baseTimestamp + "\n";
    yml += "instance: " + info.instanceName + "\n";
    if (!info.targetSpawn.isEmpty()) yml += "target_spawn: " + info.targetSpawn + "\n";
    yml += String("is_soft: ") + (info.isSoft ? "true" : "false") + "\n";
    yml += String("wol: ") + (info.wol ? "true" : "false") + "\n";
    yml += "manifest: " + info.manifestPath + "\n";
    yml += "processes:\n";
    for (size_t i = 0; i < info.processes.length(); ++i) {
        const auto &p = info.processes[i];
        yml += "  - pid: " + intStr(p.pid) + "\n";
        yml += "    spawn: " + p.spawnName + "\n";
        yml += "    segments:\n";
        for (size_t j = 0; j < p.segments.length(); ++j) {
            const auto &seg = p.segments[j];
            yml += "      - start: " + intStr(seg.start) + "\n";
            yml += "        end: " + intStr(seg.end) + "\n";
            yml += "        perms: " + seg.perms + "\n";
            yml += "        pathname: \"" + seg.pathname + "\"\n";
            yml += String("        is_delta: ") + (seg.isDelta ? "true" : "false") + "\n";
            yml += "        data_offset: " + intStr(seg.dataOffset) + "\n";
            yml += "        data_size: " + intStr(seg.dataSize) + "\n";
        }
    }

    yml += "sockets:\n";
    for (size_t i = 0; i < info.sockets.length(); ++i) {
        const auto &s = info.sockets[i];
        yml += "  - fd: " + intStr(s.fd) + "\n";
        yml += "    pid: " + intStr(s.pid) + "\n";
        yml += "    spawn: " + s.spawnName + "\n";
        yml += "    family: " + s.family + "\n";
        yml += "    type: " + s.type + "\n";
        if (!s.localAddr.isEmpty()) yml += "    local_addr: " + s.localAddr + "\n";
        if (s.localPort > 0) yml += "    local_port: " + intStr(s.localPort) + "\n";
        if (!s.peerAddr.isEmpty()) yml += "    peer_addr: " + s.peerAddr + "\n";
        if (s.peerPort > 0) yml += "    peer_port: " + intStr(s.peerPort) + "\n";
        if (!s.unixPath.isEmpty()) yml += "    unix_path: " + s.unixPath + "\n";
        if (s.isRepair) {
            yml += "    snd_seq: " + intStr(s.sndSeq) + "\n";
            yml += "    rcv_seq: " + intStr(s.rcvSeq) + "\n";
            yml += "    repair: true\n";
        }
    }

    Resource::LinuxFS fs;
    fs.write(targetDir + "/snapshot.yml", yml);

    // If not a freeze operation, unfreeze processes
    if (!isFreeze) {
        for (size_t i = 0; i < pids.length(); ++i) {
            if (pids[i] > 0) ::kill(pids[i], SIGCONT);
        }
        if (cgroupFrozen) {
            Cgroup::freeze(state.name, "", false);
        }
    }

    return true;
}

bool Snapshot::load(const String &instanceDir,
                    const String &timestamp,
                    SnapshotInfo &outInfo) {
    String ymlPath = instanceDir + "/snapshots/" + timestamp + "/snapshot.yml";
    if (!pathExists(ymlPath)) return false;

    Resource::LinuxFS fs;
    String content = fs.read(ymlPath);
    NodeBase root;
    if (!YAML::parse(content, root)) return false;

    outInfo.timestamp = yamlChildString(&root, "timestamp");
    outInfo.isDelta = (yamlChildString(&root, "is_delta") == "true");
    outInfo.baseTimestamp = yamlChildString(&root, "base_timestamp");
    outInfo.instanceName = yamlChildString(&root, "instance");
    outInfo.targetSpawn = yamlChildString(&root, "target_spawn");
    outInfo.isSoft = (yamlChildString(&root, "is_soft") == "true");
    outInfo.wol = (yamlChildString(&root, "wol") == "true");
    outInfo.manifestPath = yamlChildString(&root, "manifest");

    NodeBase *procsNode = root.get("processes");
    if (procsNode) {
        for (size_t i = 0; i < procsNode->size(); ++i) {
            NodeBase *pn = (*procsNode)[i];
            if (!pn) continue;
            ProcessMemoryDump pd;
            pd.pid = (pid_t)parseLong(yamlChildString(pn, "pid", "-1"));
            pd.spawnName = yamlChildString(pn, "spawn");

            NodeBase *segsNode = pn->get("segments");
            if (segsNode) {
                for (size_t j = 0; j < segsNode->size(); ++j) {
                    NodeBase *sn = (*segsNode)[j];
                    if (!sn) continue;
                    MemorySegment ms;
                    ms.start = (uint64_t)parseLong(yamlChildString(sn, "start", "0"));
                    ms.end = (uint64_t)parseLong(yamlChildString(sn, "end", "0"));
                    ms.perms = yamlChildString(sn, "perms");
                    ms.pathname = yamlChildString(sn, "pathname");
                    ms.isDelta = (yamlChildString(sn, "is_delta") == "true");
                    ms.dataOffset = (uint64_t)parseLong(yamlChildString(sn, "data_offset", "0"));
                    ms.dataSize = (uint64_t)parseLong(yamlChildString(sn, "data_size", "0"));
                    pd.segments.push(ms);
                }
            }
            outInfo.processes.push(pd);
        }
    }

    NodeBase *socksNode = root.get("sockets");
    if (socksNode) {
        for (size_t i = 0; i < socksNode->size(); ++i) {
            NodeBase *sn = (*socksNode)[i];
            if (!sn) continue;
            SocketInfo s;
            s.fd = (int)parseLong(yamlChildString(sn, "fd", "-1"));
            s.pid = (pid_t)parseLong(yamlChildString(sn, "pid", "-1"));
            s.spawnName = yamlChildString(sn, "spawn");
            s.family = yamlChildString(sn, "family");
            s.type = yamlChildString(sn, "type");
            s.localAddr = yamlChildString(sn, "local_addr");
            s.localPort = (int)parseLong(yamlChildString(sn, "local_port", "0"));
            s.peerAddr = yamlChildString(sn, "peer_addr");
            s.peerPort = (int)parseLong(yamlChildString(sn, "peer_port", "0"));
            s.unixPath = yamlChildString(sn, "unix_path");
            s.isRepair = (yamlChildString(sn, "repair") == "true");
            s.sndSeq = (uint32_t)parseLong(yamlChildString(sn, "snd_seq", "0"));
            s.rcvSeq = (uint32_t)parseLong(yamlChildString(sn, "rcv_seq", "0"));
            outInfo.sockets.push(s);
        }
    }

    return true;
}

} // namespace Tau
