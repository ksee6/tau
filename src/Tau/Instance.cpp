/**
 * @file Instance.cpp
 * @brief Instance state persistence and name:spawn resolution.
 */

#include <Tau/Instance.hpp>
#include <Tau/Config.hpp>
#include <Tau/Util.hpp>

#include <Encoding/Yaml.hpp>
#include <Resource/File.hpp>
#include <Collection/Tree.hpp>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

namespace Tau {

using namespace Encoding;
using namespace Collection;

// ─── SpawnState ───────────────────────────────────────────────────────────────

String SpawnState::statusString() const {
    switch (status) {
        case SpawnStatus::Running: return "running";
        case SpawnStatus::Exited:  return "exited";
        case SpawnStatus::Failed:  return "failed";
        case SpawnStatus::Stopped: return "stopped";
    }
    return "unknown";
}

// ─── InstanceState ────────────────────────────────────────────────────────────

String InstanceState::statusString() const {
    switch (status) {
        case InstanceStatus::Running: return "running";
        case InstanceStatus::Stopped: return "stopped";
        case InstanceStatus::Failed:  return "failed";
        case InstanceStatus::Frozen:  return "frozen";
    }
    return "unknown";
}

SpawnState *InstanceState::findSpawn(const String &spawnName) {
    for (size_t i = 0; i < spawns.length(); ++i)
        if (spawns[i].name == spawnName) return &spawns[i];
    return nullptr;
}

const SpawnState *InstanceState::findSpawn(const String &spawnName) const {
    for (size_t i = 0; i < spawns.length(); ++i)
        if (spawns[i].name == spawnName) return &spawns[i];
    return nullptr;
}

SpawnState *InstanceState::firstSpawn() {
    return spawns.length() > 0 ? &spawns[0] : nullptr;
}

// ─── toYAML ───────────────────────────────────────────────────────────────────

String Instance::toYAML(const InstanceState &state) {
    String out;
    out += "name: " + state.name + "\n";
    out += "pid: " + intStr(state.spawnPID) + "\n";
    out += "status: " + state.statusString() + "\n";
    out += "manifest: " + state.manifestPath + "\n";

    if (state.spawns.length() > 0) {
        out += "spawns:\n";
        for (size_t i = 0; i < state.spawns.length(); ++i) {
            const SpawnState &sp = state.spawns[i];
            out += "  - name: " + sp.name + "\n";
            if (sp.status == SpawnStatus::Exited || sp.status == SpawnStatus::Failed || sp.exitCode != 0) {
                out += "    exitcode: " + intStr(sp.exitCode) + "\n";
                out += "    exit_code: " + intStr(sp.exitCode) + "\n";
            }
            if (sp.pid > 0) {
                out += "    pid: " + intStr(sp.pid) + "\n";
            }
            out += "    status: " + sp.statusString() + "\n";
            String cmdEscaped = sp.command.replace("\"", "\\\"");
            out += "    command: \"" + cmdEscaped + "\"\n";
            out += String("    pty: ") + (sp.hasPTY ? "true" : "false") + "\n";
        }
    } else {
        out += "spawns: []\n";
    }
    return out;
}

// ─── fromYAML ─────────────────────────────────────────────────────────────────

bool Instance::fromYAML(const String &yaml, InstanceState &out) {
    NodeBase root;
    if (!YAML::parse(yaml, root)) return false;

    out.name         = yamlChildString(&root, "name");
    out.spawnPID     = (pid_t)parseLong(yamlChildString(&root, "pid", "0"));
    out.manifestPath = yamlChildString(&root, "manifest");

    String statusStr = yamlChildString(&root, "status");
    if (statusStr == "running")      out.status = InstanceStatus::Running;
    else if (statusStr == "failed")  out.status = InstanceStatus::Failed;
    else if (statusStr == "frozen")  out.status = InstanceStatus::Frozen;
    else                              out.status = InstanceStatus::Stopped;

    NodeBase *spawnsNode = root.get("spawns");
    if (!spawnsNode) {
        for (size_t i = 0; i < root.size(); ++i) {
            if (root[i] && root[i]->getName().isEmpty() && root[i]->size() > 0) {
                spawnsNode = root[i];
                break;
            }
        }
    }

    if (spawnsNode) {
        for (size_t i = 0; i < spawnsNode->size(); ++i) {
            NodeBase *sn = (*spawnsNode)[i];
            if (!sn) continue;

            SpawnState sp;
            sp.name     = yamlChildString(sn, "name");
            sp.pid      = (pid_t)parseLong(yamlChildString(sn, "pid", "0"));
            sp.command  = yamlChildString(sn, "command");
            String ecStr = yamlChildString(sn, "exitcode", "");
            if (ecStr.isEmpty()) ecStr = yamlChildString(sn, "exit_code", "0");
            sp.exitCode = (int)parseLong(ecStr);
            sp.hasPTY   = (yamlChildString(sn, "pty") == "true");

            String ss = yamlChildString(sn, "status");
            if (ss == "running")      sp.status = SpawnStatus::Running;
            else if (ss == "exited")  sp.status = SpawnStatus::Exited;
            else if (ss == "failed")  sp.status = SpawnStatus::Failed;
            else if (!ecStr.isEmpty() && ecStr != "0") sp.status = SpawnStatus::Failed;
            else                      sp.status = SpawnStatus::Stopped;

            if (!sp.name.isEmpty()) {
                out.spawns.push(sp);
            }
        }
    }
    return true;
}

// ─── load / save ──────────────────────────────────────────────────────────────

bool Instance::load(const String &instanceDir, InstanceState &out) {
    Resource::LinuxFS fs;
    String content = fs.read(instanceDir + "/state.yml");
    if (content.isEmpty()) return false;
    return fromYAML(content, out);
}


bool Instance::save(const String &instanceDir, const InstanceState &state) {
    String yaml     = toYAML(state);
    String tmpPath  = instanceDir + "/state.yml.tmp";
    String final_   = instanceDir + "/state.yml";

    Resource::LinuxFS fs;
    fs.write(tmpPath, yaml);

    if (::rename(tmpPath.c_str(), final_.c_str()) != 0) {
        logError("tau: rename state.yml.tmp: ", ::strerror(errno));
        return false;
    }
    return true;
}

// ─── listAll ──────────────────────────────────────────────────────────────────

Array<String> Instance::listAll(const String &instancesDir) {
    Array<String> names;
    DIR *d = ::opendir(instancesDir.c_str());
    if (!d) return names;

    struct dirent *entry;
    while ((entry = ::readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        String name(entry->d_name);
        String statePath = instancesDir + "/" + name + "/state.yml";
        String instPath  = instancesDir + "/" + name + "/instance.yml";
        if (pathExists(statePath) || pathExists(instPath)) names.push(name);
    }
    ::closedir(d);
    return names;
}


// ─── resolve ──────────────────────────────────────────────────────────────────

bool Instance::resolve(const String &spec, const String &instancesDir,
                        String &outInstance, String &outSpawn) {
    if (spec.isEmpty()) return false;

    // 1. Check direct match (e.g. spec is exact instance name)
    InstanceState state;
    if (load(instancesDir + "/" + spec, state)) {
        outInstance = spec;
        auto *first = state.firstSpawn();
        outSpawn = first ? first->name : String("0");
        return true;
    }

    // 2. Check separators (':', '.', '/')
    String instName, spawnName;
    long long sep = -1;
    for (size_t i = 0; i < spec.length(); ++i) {
        char c = spec[i];
        if (c == ':' || c == '.' || c == '/') {
            sep = (long long)i;
            break;
        }
    }

    if (sep >= 0) {
        instName  = spec.substring(0, (size_t)sep);
        spawnName = spec.substring((size_t)sep + 1);
    } else {
        instName = spec;
    }

    if (instName.isEmpty()) return false;

    if (!load(instancesDir + "/" + instName, state)) return false;

    outInstance = instName;
    if (spawnName.isEmpty()) {
        auto *first = state.firstSpawn();
        outSpawn = first ? first->name : String("0");
    } else {
        outSpawn = spawnName;
    }
    return true;
}

} // namespace Tau
