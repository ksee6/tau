/**
 * @file Snapshot.hpp
 * @brief Full and delta process memory & socket snapshotting for Tau instances.
 */

#pragma once

#include "Instance.hpp"
#include <Collection/String.hpp>
#include <Collection/Array.hpp>
#include <Collection/Map.hpp>

#include <cstdint>
#include <sys/types.h>

namespace Tau {

using namespace Collection;
using namespace Xi;

struct MemorySegment {
    uint64_t start = 0;
    uint64_t end = 0;
    String   perms;
    String   pathname;
    bool     isDelta = false;
    uint64_t dataOffset = 0;
    uint64_t dataSize = 0;
};

struct ProcessMemoryDump {
    pid_t                 pid = -1;
    String                spawnName;
    Array<MemorySegment>  segments;
};

struct SocketInfo {
    int      fd = -1;
    pid_t    pid = -1;
    String   spawnName;
    String   family;      ///< "inet", "inet6", "unix"
    String   type;        ///< "tcp", "udp", "stream", "dgram"
    String   state;       ///< "ESTABLISHED", "LISTEN", etc.
    String   localAddr;
    int      localPort = 0;
    String   peerAddr;
    int      peerPort = 0;
    String   unixPath;
    uint32_t sndSeq = 0;
    uint32_t rcvSeq = 0;
    String   sndQueueData;
    String   rcvQueueData;
    bool     isRepair = false;
};

struct SnapshotInfo {
    String                  timestamp;
    bool                    isDelta = false;
    String                  baseTimestamp;
    String                  instanceName;
    String                  targetSpawn;
    bool                    isSoft = false;
    bool                    wol = false;
    String                  manifestPath;
    Array<ProcessMemoryDump> processes;
    Array<SocketInfo>       sockets;
};

class Snapshot {
public:
    /**
     * @brief Take a full or delta snapshot of an instance or specific spawn.
     *
     * Saves to <instanceDir>/snapshots/<timestamp>/
     *
     * @param instanceDir  Path to instance directory.
     * @param state        Current InstanceState.
     * @param isFreeze     Whether this snapshot is part of a freeze operation.
     * @param outTimestamp Generated timestamp on success.
     * @param targetSpawn  Optional specific spawn/VM to snapshot (empty = all).
     * @param isSoft       Whether this is a soft freeze (keep supervisor alive).
     * @param wol          Whether Wake on LAN is enabled for this freeze.
     * @return true on success.
     */
    static bool take(const String &instanceDir,
                     const InstanceState &state,
                     bool isFreeze,
                     String &outTimestamp,
                     const String &targetSpawn = "",
                     bool isSoft = false,
                     bool wol = false);

    /**
     * @brief List available snapshot timestamps in an instance directory.
     */
    static Array<String> list(const String &instanceDir);

    /**
     * @brief Find snapshot matching closest timestamp <= targetTimestamp.
     *        If targetTimestamp is empty, returns the latest snapshot.
     */
    static String findMatching(const String &instanceDir, const String &targetTimestamp = "");

    /**
     * @brief Load and reconstruct a snapshot (resolving deltas if needed).
     */
    static bool load(const String &instanceDir,
                     const String &timestamp,
                     SnapshotInfo &outInfo);

    /**
     * @brief Extract all running PIDs in an instance.
     */
    static Array<pid_t> collectInstancePids(const InstanceState &state);
};

} // namespace Tau
