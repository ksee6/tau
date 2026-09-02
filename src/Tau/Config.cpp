/**
 * @file Config.cpp
 * @brief TAU_PATH, TAU_PATH_TEMP, and TAU_GLOBAL resolution and directory layout management.
 */

#include <Tau/Config.hpp>
#include <Tau/Util.hpp>

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>

namespace Tau {

// ─── Static members ───────────────────────────────────────────────────────────
String Config::_tauPath;
String Config::_tauPathTemp;
String Config::_tauGlobal;
bool   Config::_initialised = false;

// ─── expandHome ───────────────────────────────────────────────────────────────

String Config::expandHome(const String &path) {
    if (path.length() == 0 || path[0] != '~') return path;

    const char *home = ::getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd *pw = ::getpwuid(::getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return path;

    String expanded(home);
    expanded += path.substring(1);
    return expanded;
}

// ─── init ─────────────────────────────────────────────────────────────────────

void Config::init() {
    if (_initialised) return;

    uid_t uid = ::getuid();
    String userName;
    struct passwd *pw = ::getpwuid(uid);
    if (pw && pw->pw_name) userName = String(pw->pw_name);
    const char *uEnv = ::getenv("USER");
    if (userName.isEmpty() && uEnv) userName = String(uEnv);

    // 1. TAU_GLOBAL (defaults to /.var/tau)
    const char *envGlobal = ::getenv(ENV_TAU_GLOBAL);
    _tauGlobal = expandHome(envGlobal ? String(envGlobal) : String(DEFAULT_TAU_GLOBAL));

    // 2. TAU_PATH_TEMP
    // defaults to /tmp/tau for root (uid == 0), and /tmp/tau-<uid> for non-root
    const char *envTemp = ::getenv(ENV_TAU_PATH_TEMP);
    if (envTemp && envTemp[0] != '\0') {
        _tauPathTemp = expandHome(String(envTemp));
    } else {
        if (uid == 0) {
            _tauPathTemp = "/tmp/tau";
        } else {
            _tauPathTemp = "/tmp/tau-" + intStr((long long)uid);
        }
    }

    // 3. TAU_PATH
    // defaults to ~/.var/tau for users, and TAU_GLOBAL for root
    const char *envPath = ::getenv(ENV_TAU_PATH);
    if (envPath && envPath[0] != '\0') {
        _tauPath = expandHome(String(envPath));
    } else {
        if (uid == 0) {
            _tauPath = _tauGlobal;
        } else {
            _tauPath = expandHome(String(DEFAULT_TAU_PATH));
        }
    }

    _initialised = true;
}

// ─── Accessors ────────────────────────────────────────────────────────────────

const String &Config::tauPath() {
    if (!_initialised) init();
    return _tauPath;
}

const String &Config::tauPathTemp() {
    if (!_initialised) init();
    return _tauPathTemp;
}

const String &Config::tauGlobal() {
    if (!_initialised) init();
    return _tauGlobal;
}

String Config::storePath()       { return tauPath() + "/store"; }
String Config::manifestsPath()   { return tauPath() + "/manifests"; }
String Config::listsPath(const String &listName) {
    if (listName.isEmpty()) return tauPath() + "/lists";
    return tauPath() + "/lists/" + listName;
}
String Config::listTempPath(const String &listName) {
    if (listName.isEmpty()) return tauPathTemp() + "/lists";
    return tauPathTemp() + "/lists/" + listName;
}
String Config::slotsPath() { return tauPath() + "/slots"; }
String Config::slotsGlobalPath() { return tauGlobal() + "/slots"; }
String Config::githubCachePath() { return tauPath() + "/githubCache"; }
String Config::globalManifestPath() { return tauPath() + "/tau.yml"; }

String Config::instancesPath()   { return tauPathTemp() + "/instances"; }
String Config::monitorsPath()    { return tauPathTemp() + "/monitors"; }
String Config::instanceDir(const String &name) {
    if (name.startsWith("/") || (name.find("/") >= 0 && pathExists(name))) {
        return name;
    }
    return instancesPath() + "/" + name;
}

String Config::targetManifestsDir() {
    if (!_initialised) init();
    String globalManifests     = tauGlobal() + "/manifests";
    String sourceBuildTauBin   = tauGlobal() + "/source/build/tau";
    String globalTauBin        = tauGlobal() + "/tau";
    String sourceBuildStoreBin = tauGlobal() + "/source/build/tau-store";
    String globalStoreBin      = tauGlobal() + "/tau-store";

    bool storeExecutable = (pathExists(sourceBuildTauBin) && ::access(sourceBuildTauBin.c_str(), X_OK) == 0) ||
                           (pathExists(globalTauBin) && ::access(globalTauBin.c_str(), X_OK) == 0) ||
                           (pathExists(sourceBuildStoreBin) && ::access(sourceBuildStoreBin.c_str(), X_OK) == 0) ||
                           (pathExists(globalStoreBin) && ::access(globalStoreBin.c_str(), X_OK) == 0);

    if (isDir(globalManifests) && ::access(globalManifests.c_str(), W_OK) == 0 && storeExecutable) {
        return globalManifests;
    }
    return manifestsPath();
}

// ─── ensureLayout ─────────────────────────────────────────────────────────────

void Config::ensureLayout() {
    if (!_initialised) init();

    mkdirP(tauPath());
    mkdirP(storePath());
    mkdirP(githubCachePath());

    String mPath = manifestsPath();
    if (!isDir(mPath)) ::mkdir(mPath.c_str(), 0777);
    ::chmod(mPath.c_str(), 0777);

    mkdirP(tauPathTemp());
    mkdirP(instancesPath());

    String monPath = monitorsPath();
    if (!isDir(monPath)) ::mkdir(monPath.c_str(), 0777);
    ::chmod(monPath.c_str(), 0777);
}

} // namespace Tau


