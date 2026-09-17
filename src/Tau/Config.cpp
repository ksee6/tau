/**
 * @file Config.cpp
 * @brief TAU_PATH, TAU_PATH_TEMP, and TAU_GLOBAL resolution and directory layout management.
 */

#include <Tau/Config.hpp>
#include <Tau/Util.hpp>
#include <Resource/File.hpp>

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>

namespace Tau {

// ─── Static members ───────────────────────────────────────────────────────────
static String s_tauGlobal;
static String s_tau;
static String s_tauRuntime;
static String s_tauStore;
static bool s_hasGlobal = false;
static bool s_hasTau = false;
static bool s_hasRuntime = false;
static bool s_hasStore = false;

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

// ─── Lazy Path Getters ────────────────────────────────────────────────────────

const String &getTauGlobalDir() {
    if (s_hasGlobal) return s_tauGlobal;
    const char *envGlobal = ::getenv(ENV_TAU_GLOBAL);
    s_tauGlobal = Config::expandHome(envGlobal ? String(envGlobal) : String(DEFAULT_TAU_GLOBAL));
    s_hasGlobal = true;
    return s_tauGlobal;
}

const String &getTauDir() {
    if (s_hasTau) return s_tau;
    const char *env = ::getenv(ENV_TAU);
    if (!env || !*env) env = ::getenv(ENV_TAU_PATH);
    String path = env ? Config::expandHome(String(env)) : Config::expandHome(String(DEFAULT_TAU));
    // If it evaluates starts with /root, its automatically removed
    if (path.startsWith("/root/")) {
        path = path.substring(5); // e.g. /root/.var/tau -> /.var/tau
    } else if (path == "/root") {
        path = "/";
    }
    s_tau = path;
    s_hasTau = true;
    return s_tau;
}

const String &getTauRuntimeDir() {
    if (s_hasRuntime) return s_tauRuntime;
    const char *env = ::getenv(ENV_TAU_RUNTIME);
    if (!env || !*env) env = ::getenv(ENV_TAU_PATH_TEMP);
    if (env && *env) {
        s_tauRuntime = Config::expandHome(String(env));
    } else {
        String runTau = Config::expandHome(String(DEFAULT_TAU_RUNTIME));
        String runDir = Config::expandHome("~/.run");
        if (isDir(runTau) || isDir(runDir) || mkdirP(runTau)) {
            s_tauRuntime = runTau;
        } else {
            uid_t uid = ::getuid();
            if (uid == 0) {
                s_tauRuntime = "/run/tau";
            } else {
                String userName;
                struct passwd *pw = ::getpwuid(uid);
                if (pw && pw->pw_name) userName = String(pw->pw_name);
                const char *uEnv = ::getenv("USER");
                if (userName.isEmpty() && uEnv) userName = String(uEnv);
                if (userName.isEmpty()) userName = intStr((long long)uid);
                s_tauRuntime = "/run/" + userName + "/tau";
            }
        }
    }
    s_hasRuntime = true;
    return s_tauRuntime;
}

const String &getTauStoreDir() {
    if (s_hasStore) return s_tauStore;
    const char *env = ::getenv(ENV_TAU_STORE);
    if (env && *env) {
        s_tauStore = Config::expandHome(String(env));
        s_hasStore = true;
        return s_tauStore;
    }

    String globalDir = getTauGlobalDir();
    String globalTau = globalDir + "/tau";
    struct stat st;
    bool isSuid = (::stat(globalTau.c_str(), &st) == 0 && (st.st_mode & S_ISUID) && (st.st_uid == 0));
    bool inSudoers = false;
    if (!isSuid && pathExists(globalTau)) {
        if (pathExists("/etc/sudoers.d/tau")) {
            Resource::LinuxFS fs;
            String content = fs.read("/etc/sudoers.d/tau");
            if (content.find("tau") >= 0) inSudoers = true;
        }
        if (!inSudoers) {
            int ret = ::system("sudo -n -l 2>/dev/null | grep -E 'tau|ALL' >/dev/null 2>&1");
            if (ret == 0) inSudoers = true;
        }
    }

    if (isSuid || inSudoers) {
        s_tauStore = globalDir;
    } else {
        s_tauStore = getTauDir();
    }
    s_hasStore = true;
    return s_tauStore;
}

// ─── init ─────────────────────────────────────────────────────────────────────

void Config::init() {
    getTauGlobalDir();
    getTauDir();
    getTauRuntimeDir();
    getTauStoreDir();
}

// ─── Accessors ────────────────────────────────────────────────────────────────

const String &Config::tauPath()        { return getTauDir(); }
const String &Config::tauPathTemp()    { return getTauRuntimeDir(); }
const String &Config::tauGlobal()      { return getTauGlobalDir(); }
const String &Config::getTauGlobalDir(){ return Tau::getTauGlobalDir(); }
const String &Config::getTauDir()      { return Tau::getTauDir(); }
const String &Config::getTauRuntimeDir(){ return Tau::getTauRuntimeDir(); }
const String &Config::getTauStoreDir() { return Tau::getTauStoreDir(); }

String Config::storePath()       { return getTauStoreDir() + "/store"; }
String Config::manifestsPath()   { return getTauStoreDir() + "/manifests"; }
String Config::listsPath(const String &listName) {
    if (listName.isEmpty()) return getTauDir() + "/lists";
    return getTauDir() + "/lists/" + listName;
}
String Config::listTempPath(const String &listName) {
    if (listName.isEmpty()) return getTauRuntimeDir() + "/lists";
    return getTauRuntimeDir() + "/lists/" + listName;
}
String Config::slotsPath() { return getTauDir() + "/slots"; }
String Config::slotsGlobalPath() { return getTauGlobalDir() + "/slots"; }
String Config::githubCachePath() { return getTauStoreDir() + "/githubCache"; }
String Config::globalManifestPath() { return getTauStoreDir() + "/tau.yml"; }

String Config::instancesPath()   { return getTauRuntimeDir() + "/instances"; }
String Config::monitorsPath()    { return getTauRuntimeDir() + "/monitors"; }
String Config::instanceDir(const String &name) {
    if (name.startsWith("/") || (name.find("/") >= 0 && pathExists(name))) {
        return name;
    }
    return instancesPath() + "/" + name;
}

String Config::targetManifestsDir() {
    String globalManifests     = getTauGlobalDir() + "/manifests";
    String sourceBuildTauBin   = getTauGlobalDir() + "/source/build/tau";
    String globalTauBin        = getTauGlobalDir() + "/tau";
    String sourceBuildStoreBin = getTauGlobalDir() + "/source/build/tau-store";
    String globalStoreBin      = getTauGlobalDir() + "/tau-store";

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
    mkdirP(getTauDir());
    mkdirP(storePath());
    mkdirP(githubCachePath());

    String mPath = manifestsPath();
    if (!isDir(mPath)) ::mkdir(mPath.c_str(), 0777);
    ::chmod(mPath.c_str(), 0777);

    mkdirP(getTauRuntimeDir());
    mkdirP(instancesPath());

    String monPath = monitorsPath();
    if (!isDir(monPath)) ::mkdir(monPath.c_str(), 0777);
    ::chmod(monPath.c_str(), 0777);
}

} // namespace Tau


