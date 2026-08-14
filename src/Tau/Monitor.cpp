/**
 * @file Monitor.cpp
 * @brief Instance event monitor implementation via Unix datagram sockets.
 */

#include <Tau/Monitor.hpp>
#include <Tau/Util.hpp>
#include <Terminal/Format.hpp>
#include <Encoding/Regex.hpp>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>

#include <regex.h>

namespace Tau {

static inline uint64_t currentMicros() {
    struct timeval tv;
    ::gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static bool matchInstanceRegex(const String &pattern, const String &name) {
    if (pattern.isEmpty() || pattern == ".*" || pattern == "*") {
        return true;
    }
    Encoding::Regex re(pattern);
    if (re.parsed && re.matchAll(name, 1).length() > 0) {
        return true;
    }
    regex_t preg;
    if (::regcomp(&preg, pattern.c_str(), REG_EXTENDED | REG_NOSUB) == 0) {
        bool match = (::regexec(&preg, name.c_str(), 0, nullptr, 0) == 0);
        ::regfree(&preg);
        return match;
    }
    return false;
}

void Monitor::broadcast(const String &eventType,
                        const String &instanceName,
                        uint64_t      startupTime,
                        const String &data) {
    String payload = eventType + " " + instanceName + " " + intStr(startupTime);
    if (!data.isEmpty()) {
        payload += " " + data;
    }

    String monDir = Config::monitorsPath();
    if (!isDir(monDir)) return;

    DIR *d = ::opendir(monDir.c_str());
    if (!d) return;

    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        ::closedir(d);
        return;
    }

    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr) {
        String fname(ent->d_name);
        if (fname.endsWith(".sock")) {
            String sockPath = monDir + "/" + fname;
            struct sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            ::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
            socklen_t addrLen = sizeof(addr.sun_family) + sockPath.length() + 1;

            ssize_t sent = ::sendto(fd, payload.c_str(), payload.length(), 0,
                                    (struct sockaddr *)&addr, addrLen);
            if (sent < 0) {
                if (errno == ECONNREFUSED || errno == ENOENT || errno == ENOTCONN) {
                    ::unlink(sockPath.c_str());
                }
            }
        }
    }

    ::close(fd);
    ::closedir(d);
}

static String s_monitorCleanupPath;
static void handleMonitorExit(int) {
    if (!s_monitorCleanupPath.isEmpty()) {
        ::unlink(s_monitorCleanupPath.c_str());
    }
    ::fflush(stdout);
    ::_exit(0);
}

int Monitor::runListener(const String &regexPattern,
                         bool          powerEvents,
                         bool          ipEvents,
                         bool          allowNewer) {
    ::setvbuf(stdout, nullptr, _IONBF, 0);
    Config::ensureLayout();
    String monDir = Config::monitorsPath();
    mkdirP(monDir);

    uint64_t monitorStartTime = currentMicros();
    pid_t pid = ::getpid();
    ::srand((unsigned int)::time(nullptr) ^ (unsigned int)pid);
    String sockPath = monDir + "/mon_" + intStr(pid) + "_" + intStr((uint64_t)::rand()) + ".sock";

    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        Terminal::Error("tau monitor: cannot create socket: " + String(::strerror(errno)));
        return 1;
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    socklen_t addrLen = sizeof(addr.sun_family) + sockPath.length() + 1;
    ::unlink(sockPath.c_str());

    if (::bind(fd, (struct sockaddr *)&addr, addrLen) != 0) {
        Terminal::Error("tau monitor: cannot bind socket: " + String(::strerror(errno)));
        ::close(fd);
        return 1;
    }

    s_monitorCleanupPath = sockPath;
    ::signal(SIGINT, handleMonitorExit);
    ::signal(SIGTERM, handleMonitorExit);

    bool filterAll = (!powerEvents && !ipEvents);

    char buf[4096];
    while (true) {
        ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        String line(buf);
        Array<String> parts = line.trim().split(" ");
        if (parts.length() < 3) continue;

        String eventType     = parts[0];
        String instanceName  = parts[1];
        uint64_t startupTime = (uint64_t)Collection::parseLong(parts[2]);
        String data;
        for (size_t p = 3; p < parts.length(); ++p) {
            if (!data.isEmpty()) data += " ";
            data += parts[p];
        }

        // 1. Instance regex matching
        if (!matchInstanceRegex(regexPattern, instanceName)) {
            continue;
        }

        // 2. Startup time check: if instance is newer than monitor and allowNewer is false, drop
        if (startupTime > monitorStartTime && !allowNewer) {
            continue;
        }

        // 3. Event filter check
        bool isPower = (eventType == "START" || eventType == "STOP");
        bool isNet   = (eventType == "IP");

        if (!filterAll) {
            if (powerEvents && !isPower && !ipEvents) continue;
            if (ipEvents && !isNet && !powerEvents) continue;
            if (!powerEvents && isPower) continue;
            if (!ipEvents && isNet) continue;
        }

        // Output event
        if (eventType == "START") {
            printf("[START] %s (startup_time=%llu)\n", instanceName.c_str(), (unsigned long long)startupTime);
        } else if (eventType == "STOP") {
            printf("[STOP] %s %s\n", instanceName.c_str(), data.c_str());
        } else if (eventType == "IP") {
            printf("[IP] %s %s\n", instanceName.c_str(), data.c_str());
        } else {
            printf("[%s] %s %s\n", eventType.c_str(), instanceName.c_str(), data.c_str());
        }
        ::fflush(stdout);
    }

    ::unlink(sockPath.c_str());
    ::close(fd);
    return 0;
}


} // namespace Tau
