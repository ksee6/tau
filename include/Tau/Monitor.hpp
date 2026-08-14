/**
 * @file Monitor.hpp
 * @brief Instance event monitoring and broadcast system.
 */

#pragma once

#include "Config.hpp"
#include <Collection/String.hpp>
#include <Collection/Array.hpp>
#include <cstdint>

namespace Tau {

using namespace Collection;
using namespace Xi;

class Monitor {
public:
    /**
     * @brief Broadcast an event from an instance to all active monitor sockets in TAU_PATH/monitors.
     * @param eventType    Event kind (e.g. "START", "STOP", "IP").
     * @param instanceName Name of the instance generating the event.
     * @param startupTime  Instance startup timestamp (seconds or microseconds).
     * @param data         Extra payload (e.g. "exit_code=0" or "iface=eth0 ip=192.168.1.1").
     */
    static void broadcast(const String &eventType,
                          const String &instanceName,
                          uint64_t      startupTime,
                          const String &data = "");

    /**
     * @brief Run the monitor event listener.
     * @param regexPattern Regex matching instance names (default empty = all).
     * @param powerEvents  Filter for power events (START, STOP).
     * @param ipEvents     Filter for network IP events (IP).
     * @param allowNewer   Whether to allow events from instances started after monitor start time.
     * @return Exit code.
     */
    static int runListener(const String &regexPattern,
                           bool          powerEvents,
                           bool          ipEvents,
                           bool          allowNewer);
};

} // namespace Tau
