/**
 * @file Cgroup.cpp
 * @brief cgroup v2 resource-limit helpers.
 */

#include <Tau/Cgroup.hpp>
#include <Tau/Util.hpp>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

namespace Tau {

static constexpr const char *CGROUP_ROOT = "/sys/fs/cgroup/tau";

bool Cgroup::writeFile(const String &path, const String &value) {
    int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
    if (fd < 0) {
        logWarn("tau/cgroup: cannot open ", path, ": ", ::strerror(errno));
        return false;
    }
    ssize_t n = ::write(fd, value.c_str(), value.length());
    ::close(fd);
    return n == (ssize_t)value.length();
}

String Cgroup::cgroupPath(const String &instanceName, const String &spawnName) {
    if (spawnName.isEmpty()) {
        return String(CGROUP_ROOT) + "/" + instanceName;
    }
    return String(CGROUP_ROOT) + "/" + instanceName + "/" + spawnName;
}

bool Cgroup::available() {
    return pathExists("/sys/fs/cgroup");
}

bool Cgroup::create(const String &instanceName, const String &spawnName) {
    if (!available()) {
        logError("tau/cgroup: /sys/fs/cgroup is not available");
        return false;
    }
    if (!mkdirP(String(CGROUP_ROOT))) {
        logError("tau/cgroup: cannot create cgroup root ", CGROUP_ROOT, ": ", ::strerror(errno));
        return false;
    }
    String path = cgroupPath(instanceName, spawnName);
    if (!mkdirP(path)) {
        logError("tau/cgroup: cannot create ", path, ": ", ::strerror(errno));
        return false;
    }
    return true;
}

bool Cgroup::setMemory(const String &instanceName, const String &spawnName,
                        size_t bytes) {
    if (!available()) return true;
    String path = cgroupPath(instanceName, spawnName) + "/memory.max";
    String value = (bytes == 0) ? String("max") : intStr(bytes);
    return writeFile(path, value + "\n");
}

bool Cgroup::setCPU(const String &instanceName, const String &spawnName,
                     long long quota) {
    if (!available()) return true;
    String path = cgroupPath(instanceName, spawnName) + "/cpu.max";
    String value;
    if (quota < 0) {
        value = "max 100000";
    } else {
        value = intStr(quota * 1000LL) + " 100000";
    }
    return writeFile(path, value + "\n");
}

bool Cgroup::setCPUSet(const String &instanceName, const String &spawnName,
                        const String &cpus) {
    if (!available()) return true;
    String path = cgroupPath(instanceName, spawnName) + "/cpuset.cpus";
    return writeFile(path, cpus + "\n");
}

bool Cgroup::addPID(const String &instanceName, const String &spawnName,
                     pid_t pid) {
    if (!available()) return true;
    String path = cgroupPath(instanceName, spawnName) + "/cgroup.procs";
    return writeFile(path, intStr(pid) + "\n");
}

void Cgroup::remove(const String &instanceName, const String &spawnName) {
    if (!available()) return;
    String path = cgroupPath(instanceName, spawnName);
    if (!pathExists(path)) return;
    writeFile(path + "/cgroup.kill", "1\n");
    ::rmdir(path.c_str());
}

void Cgroup::removeInstance(const String &instanceName) {
    if (!available()) return;
    String path = String(CGROUP_ROOT) + "/" + instanceName;
    if (!pathExists(path)) return;
    writeFile(path + "/cgroup.kill", "1\n");
    ::rmdir(path.c_str());
}


} // namespace Tau
