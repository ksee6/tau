/**
 * @file main.cpp
 * @brief tau — main CLI entry point.
 */

#include <Tau/Config.hpp>
#include <Tau/Manifest.hpp>
#include <Tau/Store.hpp>
#include <Tau/IPC.hpp>
#include <Tau/Instance.hpp>
#include <Tau/Monitor.hpp>
#include <Tau/GitHub.hpp>
#include <Tau/Snapshot.hpp>
#include <Tau/Util.hpp>


#include <Terminal/Command.hpp>
#include <Terminal/Format.hpp>
#include <Resource/File.hpp>
#include <Encoding/Regex.hpp>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <dirent.h>
#include <regex.h>

using namespace Terminal;
using namespace Tau;
using namespace Collection;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void resolveTerminalSize(const String &colOpt, const String &rowOpt, int &outCols, int &outRows) {
    outCols = 80;
    outRows = 24;

    struct winsize ws = {};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        outCols = ws.ws_col;
        outRows = ws.ws_row;
    } else if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        outCols = ws.ws_col;
        outRows = ws.ws_row;
    } else {
        const char *c = ::getenv("COLUMNS");
        const char *r = ::getenv("LINES");
        if (c && *c) { int v = ::atoi(c); if (v > 0) outCols = v; }
        if (r && *r) { int v = ::atoi(r); if (v > 0) outRows = v; }
    }

    if (!colOpt.isEmpty() && colOpt != "auto") {
        long long c = Collection::parseLong(colOpt);
        if (c > 0) outCols = (int)c;
    }
    if (!rowOpt.isEmpty() && rowOpt != "auto") {
        long long r = Collection::parseLong(rowOpt);
        if (r > 0) outRows = (int)r;
    }
}


static String resolveExecutable(const String &name) {
    // 1. <tau current path from the running binary>/<name>
    char selfBuf[4096] = {};
    ssize_t n = ::readlink("/proc/self/exe", selfBuf, sizeof(selfBuf) - 1);
    if (n > 0) {
        String selfPath(selfBuf);
        long long sl = rfind(selfPath, '/');
        if (sl >= 0) {
            String siblingBin = selfPath.substring(0, (size_t)sl) + "/" + name;
            if (pathExists(siblingBin) && ::access(siblingBin.c_str(), X_OK) == 0) {
                return siblingBin;
            }
        }
    }

    // 2. Prioritize TAU_PATH/source/build/<name> (check global then user path)
    String sourceBuildGlobal = Config::tauGlobal() + "/source/build/" + name;
    if (pathExists(sourceBuildGlobal) && ::access(sourceBuildGlobal.c_str(), X_OK) == 0) {
        return sourceBuildGlobal;
    }

    String sourceBuildUser = Config::tauPath() + "/source/build/" + name;
    if (pathExists(sourceBuildUser) && ::access(sourceBuildUser.c_str(), X_OK) == 0) {
        return sourceBuildUser;
    }

    // 3. Fallback to TAU_GLOBAL/<name>
    String globalBin = Config::tauGlobal() + "/" + name;
    if (pathExists(globalBin) && ::access(globalBin.c_str(), X_OK) == 0) {
        return globalBin;
    }

    // 4. Default to binary name for PATH lookup
    return name;
}

static String findNearestManifest(const String &startDir) {
    String dir = startDir;
    for (int depth = 0; depth < 20; ++depth) {
        String candidate = dir + "/tau.yml";
        if (pathExists(candidate)) return candidate;

        long long sl = rfind(dir, '/');
        if (sl <= 0) break;
        dir = dir.substring(0, (size_t)sl);
    }
    return "";
}

static String resolveCwd() {
    char buf[4096] = {};
    if (::getcwd(buf, sizeof(buf) - 1)) return String(buf);
    return ".";
}

static String uniqueInstanceName(const String &baseName) {
    Config::ensureLayout();
    String candidate = baseName;
    if (candidate.isEmpty()) candidate = "0";

    String dir = Config::instanceDir(candidate);
    if (!pathExists(dir)) return candidate;

    // Check if stale
    InstanceState st;
    if (Instance::load(dir, st)) {
        if (st.spawnPID <= 0 || ::kill(st.spawnPID, 0) != 0) {
            // Dead instance; clean up directory and reclaim name
            ::system(("rm -rf '" + dir + "'").c_str());
            return candidate;
        }
    } else {
        ::system(("rm -rf '" + dir + "'").c_str());
        return candidate;
    }

    // Append incrementing integer
    for (int i = 1; i < 10000; ++i) {
        String numbered = candidate + "-" + intStr(i);
        String ndir = Config::instanceDir(numbered);
        if (!pathExists(ndir)) return numbered;
        InstanceState nst;
        if (Instance::load(ndir, nst)) {
            if (nst.spawnPID <= 0 || ::kill(nst.spawnPID, 0) != 0) {
                ::system(("rm -rf '" + ndir + "'").c_str());
                return numbered;
            }
        } else {
            ::system(("rm -rf '" + ndir + "'").c_str());
            return numbered;
        }
    }
    return candidate + "-new";
}


static int attachToInstance(const String &instanceName,
                            const String &spawnTargetName,
                            bool          attachStdin,
                            const String &detachKey,
                            int           historyLines,
                            int           cols,
                            int           rows,
                            bool          streamOutput = true) {
    String instDir = Config::instanceDir(instanceName);
    String sockPath = instDir + "/tau.sock";

    IPCClient client;
    if (!client.connect(instDir, /*quiet=*/true)) {
        InstanceState state;
        if (Instance::load(instDir, state)) {
            if (state.spawnPID <= 0 || ::kill(state.spawnPID, 0) != 0) {
                ::system(("rm -rf '" + instDir + "'").c_str());
            }
        }
        Terminal::Error("Cannot connect to " + instanceName);
        return 1;
    }

    String spawnName = spawnTargetName.isEmpty() ? "0" : spawnTargetName;
    if (spawnName == "0") {
        for (int retry = 0; retry < 500; ++retry) {
            InstanceState state;
            if (Instance::load(instDir, state)) {
                auto *first = state.firstSpawn();
                if (first && !first->name.isEmpty()) {
                    spawnName = first->name;
                    break;
                }
                if (state.status == InstanceStatus::Failed || state.status == InstanceStatus::Stopped) break;
            }
            if (!pathExists(sockPath)) break;
            ::usleep(10000); // 10ms
        }
    }

    if (cols > 0 && rows > 0) {
        client.resize(spawnName, cols, rows);
    }

    if (historyLines > 0) {
        client.cat(spawnName, historyLines);
    }

    if (!streamOutput) {
        return 0;
    }

    client.attach(spawnName, attachStdin, detachKey, cols, rows);
    return 0;
}


static int launchSpawnProcess(const String        &instanceName,
                               const Array<String> &manifestEntries,
                               const String        &workDir,
                               bool                 detach,
                               bool                 attachStdin,
                               const String        &detachKey,
                               int                  historyLines = 0,
                               bool                 debug = false,
                               int                  cols = 80,
                               int                  rows = 24,
                               bool                 headless = false,
                               bool                 remove_ = false,
                               bool                 global_ = false,
                               const String        &sourceManifestPath = "",
                               const String        &headDir = "",
                               bool                 copyYaml = true,
                               bool                 watch = true,
                               const Array<String> &extraArgs = {}) {

    String spawnBin = resolveExecutable("tau-instance");

    // Double-fork + setsid to fully detach tau-instance from the terminal session.
    // First fork: parent continues, intermediate child detaches.
    pid_t pid1 = ::fork();
    if (pid1 < 0) { Terminal::Error("tau: fork failed"); return 1; }
    if (pid1 > 0) {
        // Parent: wait for intermediate child to fork the daemon and exit
        int st; ::waitpid(pid1, &st, 0);

        String instDir = !headDir.isEmpty() ? headDir : Config::instanceDir(instanceName);
        String sockPath = instDir + "/tau.sock";

        // Poll for socket readiness (up to 3 seconds)
        bool started = false;
        bool ready = false;
        for (int i = 0; i < 300; ++i) {
            if (pathExists(sockPath)) { ready = true; started = true; break; }
            InstanceState ist;
            if (Instance::load(instDir, ist)) {
                started = true;
                if (ist.status == InstanceStatus::Running) {
                    ready = true;
                    break;
                }
                if (ist.status == InstanceStatus::Failed || ist.status == InstanceStatus::Stopped) {
                    return (ist.status == InstanceStatus::Failed) ? 1 : 0;
                }
            }
            if (started && !pathExists(instDir)) {
                return 0; // Instance ran and finished cleanly
            }
            ::usleep(10000); // 10ms
        }

        if (started) {
            InstanceState ist;
            for (int r = 0; r < 50; ++r) {
                if (Instance::load(instDir, ist) && ist.spawnPID > 0) break;
                ::usleep(20000);
            }

            for (size_t mi = 0; mi < manifestEntries.length(); ++mi) {
                Array<String> visited;
                ParseError perr;
                ParsedManifest pm = Manifest::load(manifestEntries[mi], visited, perr);
                if (perr.ok) {
                    for (auto *d : pm.directives) {
                        if (d->kind == DirectiveKind::Forward) {
                            auto *fd = static_cast<DForward*>(d);
                            String srcPort = fd->sourcePort.isEmpty() ? fd->port : fd->sourcePort;
                            String dstPort = fd->port.isEmpty() ? fd->sourcePort : fd->port;
                            if (!srcPort.isEmpty() && !dstPort.isEmpty() && ist.spawnPID > 0) {
                                String killStale = String("pkill -9 -f 'socat TCP-LISTEN:") + srcPort + "' 2>/dev/null";
                                (void)::system(killStale.c_str());
                                ::usleep(20000);

                                pid_t fwdFork = ::fork();
                                if (fwdFork == 0) {
                                    ::setsid();
                                    pid_t fwdChild = ::fork();
                                    if (fwdChild == 0) {
                                        int devNull = ::open("/dev/null", O_RDWR);
                                        if (devNull >= 0) {
                                            ::dup2(devNull, STDIN_FILENO);
                                            ::dup2(devNull, STDOUT_FILENO);
                                            ::dup2(devNull, STDERR_FILENO);
                                            if (devNull > 2) ::close(devNull);
                                        }
                                        String listenArg = String("TCP-LISTEN:") + srcPort + ",fork,reuseaddr";
                                        String execArg = String("EXEC:nsenter -t ") + intStr(ist.spawnPID) + " -U -n --preserve-credentials nc 127.0.0.1 " + dstPort;
                                        const char *sargv[] = {
                                            "socat",
                                            listenArg.c_str(),
                                            execArg.c_str(),
                                            nullptr
                                        };
                                        ::execvp("socat", (char *const *)sargv);
                                        ::_exit(127);
                                    }
                                    ::_exit(0);
                                }
                                if (fwdFork > 0) {
                                    int st; ::waitpid(fwdFork, &st, 0);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (detach) {
            Terminal::Info("Started instance " + Terminal::Bold(instanceName));
            return 0;
        }

        if (!ready) {
            InstanceState ist;
            if (Instance::load(instDir, ist)) {
                return (ist.status == InstanceStatus::Failed) ? 1 : 0;
            }
            if (!pathExists(instDir)) return 0;
            Terminal::Error("tau: instance failed to start socket");
            return 1;
        }

        return attachToInstance(instanceName, "0", attachStdin, detachKey, historyLines, cols, rows);
    }

    // Intermediate: become session leader, detach from tty
    ::setsid();

    // Second fork: ensures we are not the session leader so we can't acquire a tty
    pid_t pid2 = ::fork();
    if (pid2 < 0) ::_exit(1);
    if (pid2 > 0) ::_exit(0); // Intermediate exits

    // Grandchild: fully detached daemon — redirect standard streams to /dev/null
    ::close(STDIN_FILENO);
    ::close(STDOUT_FILENO);
    ::close(STDERR_FILENO);
    int devNull = ::open("/dev/null", O_RDWR);
    if (devNull >= 0) {
        ::dup2(devNull, STDIN_FILENO);
        ::dup2(devNull, STDOUT_FILENO);
        ::dup2(devNull, STDERR_FILENO);
        if (devNull > 2) ::close(devNull);
    }

#ifdef __NR_close_range
    ::syscall(__NR_close_range, 3, ~0U, 0);
#else
    DIR *d = ::opendir("/proc/self/fd");
    if (d) {
        int dfd = ::dirfd(d);
        struct dirent *e;
        while ((e = ::readdir(d))) {
            int f = ::atoi(e->d_name);
            if (f > 2 && f != dfd) ::close(f);
        }
        ::closedir(d);
    }
#endif

    Array<const char *> argv;
    argv.push(spawnBin.c_str());
    argv.push("--name"); argv.push(instanceName.c_str());
    if (!headDir.isEmpty()) {
        argv.push("--head");
        argv.push(headDir.c_str());
    }
    if (headless) argv.push("--headless");
    if (!copyYaml) argv.push("--no-copy");
    if (!watch) argv.push("--no-watch");
    if (remove_)  argv.push("--remove");
    if (global_)  argv.push("--global");
    if (!sourceManifestPath.isEmpty()) {
        argv.push("--source-manifest");
        argv.push(sourceManifestPath.c_str());
    }
    if (!workDir.isEmpty()) {
        argv.push("--workdir");
        argv.push(workDir.c_str());
    }
    argv.push("--detach");
    if (debug) argv.push("--debug");
    if (!detachKey.isEmpty()) {
        argv.push("--detach-key");
        argv.push(detachKey.c_str());
    }
    String colStr = intStr(cols);
    String rowStr = intStr(rows);
    argv.push("--col"); argv.push(colStr.c_str());
    argv.push("--row"); argv.push(rowStr.c_str());

    for (size_t i = 0; i < manifestEntries.length(); ++i) {
        argv.push(manifestEntries[i].c_str());
    }
    if (extraArgs.length() > 0) {
        argv.push("--");
        for (size_t i = 0; i < extraArgs.length(); ++i) {
            argv.push(extraArgs[i].c_str());
        }
    }
    argv.push(nullptr);

    ::execvp(spawnBin.c_str(), (char *const *)argv.data());
    ::_exit(127);
}






// Forward declarations
static int cmdAdd(const String &manifestArg);
static int cmdRemove(const String &manifestArg);
static int launchStoreInBackground(const String &manifestPath);

// ─── tau init ─────────────────────────────────────────────────────────────────

static int cmdInit() {
    String cwd       = resolveCwd();
    String tauYml    = cwd + "/tau.yml";

    if (pathExists(tauYml)) {
        Terminal::Warn("tau.yml already exists in " + cwd);
    } else {
        Resource::LinuxFS fs;
        fs.write(tauYml,
            "# tau manifest\n"
            "# See https://github.com/tau-pkg/tau for documentation\n\n"
            "- name: my-project\n"
            "- version: 0.1.0\n"
            "- description: A tau-managed project\n");
        Terminal::Info("Created " + tauYml);
    }

    return cmdAdd(tauYml);
}

// ─── tau add ──────────────────────────────────────────────────────────────────

static int cmdAdd(const String &manifestArg) {
    Config::init();
    Config::ensureLayout();

    String path = manifestArg;
    if (path.isEmpty()) {
        path = findNearestManifest(resolveCwd());
        if (path.isEmpty()) {
            path = resolveCwd() + "/tau.yml";
            if (!pathExists(path)) {
                Terminal::Error("No manifest specified and no tau.yml found in current directory.");
                return 1;
            }
        }
    }

    if (!path.startsWith("/")) {
        path = resolveCwd() + "/" + path;
    }

    char realBuf[4096] = {};
    if (!::realpath(path.c_str(), realBuf)) {
        Terminal::Error("Cannot resolve manifest path: " + path);
        return 1;
    }
    String realPath(realBuf);

    String hash = Store::computeHash(realPath);
    if (hash.isEmpty()) {
        Terminal::Error("Failed to parse and hash manifest: " + realPath);
        return 1;
    }

    String manifestsDir = Config::targetManifestsDir();
    mkdirP(manifestsDir);

    // Remove any existing symlinks pointing to this real path
    DIR *d = ::opendir(manifestsDir.c_str());
    if (d) {
        struct dirent *ent;
        while ((ent = ::readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            String entryPath = manifestsDir + "/" + ent->d_name;
            char targetBuf[4096] = {};
            ssize_t len = ::readlink(entryPath.c_str(), targetBuf, sizeof(targetBuf) - 1);
            if (len > 0) {
                targetBuf[len] = '\0';
                String linkTarget = targetBuf;
                if (!linkTarget.startsWith("/")) {
                    linkTarget = manifestsDir + "/" + linkTarget;
                }
                char realLinkTarget[4096] = {};
                if (::realpath(linkTarget.c_str(), realLinkTarget)) {
                    if (String(realLinkTarget) == realPath) {
                        ::unlink(entryPath.c_str());
                    }
                }
            }
        }
        ::closedir(d);
    }

    String symlinkPath = manifestsDir + "/" + hash + ".yml";
    ::unlink(symlinkPath.c_str());
    if (::symlink(realPath.c_str(), symlinkPath.c_str()) != 0) {
        Terminal::Error("Failed to create symlink: " + symlinkPath + " -> " + realPath);
        return 1;
    }

    Terminal::Success("Registered " + realPath + " [" + hash.substring(0, 12) + "...]");
    launchStoreInBackground(realPath);
    return 0;
}

// ─── tau remove ───────────────────────────────────────────────────────────────

static int cmdRemove(const String &manifestArg) {
    Config::init();
    Config::ensureLayout();

    String path = manifestArg;
    if (path.isEmpty()) {
        path = findNearestManifest(resolveCwd());
        if (path.isEmpty()) {
            path = resolveCwd() + "/tau.yml";
        }
    }

    if (!path.startsWith("/")) {
        path = resolveCwd() + "/" + path;
    }

    char realBuf[4096] = {};
    if (!::realpath(path.c_str(), realBuf)) {
        Terminal::Error("Cannot resolve manifest path: " + path);
        return 1;
    }
    String realPath(realBuf);

    bool removed = false;
    Array<String> dirs;
    dirs.push(Config::targetManifestsDir());
    if (Config::manifestsPath() != Config::targetManifestsDir()) {
        dirs.push(Config::manifestsPath());
    }

    for (size_t di = 0; di < dirs.length(); ++di) {
        const String &manifestsDir = dirs[di];
        DIR *d = ::opendir(manifestsDir.c_str());
        if (!d) continue;

        struct dirent *ent;
        while ((ent = ::readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            String entryPath = manifestsDir + "/" + ent->d_name;
            char targetBuf[4096] = {};
            ssize_t len = ::readlink(entryPath.c_str(), targetBuf, sizeof(targetBuf) - 1);
            if (len > 0) {
                targetBuf[len] = '\0';
                String linkTarget = targetBuf;
                if (!linkTarget.startsWith("/")) {
                    linkTarget = manifestsDir + "/" + linkTarget;
                }
                char realLinkTarget[4096] = {};
                if (::realpath(linkTarget.c_str(), realLinkTarget)) {
                    if (String(realLinkTarget) == realPath) {
                        ::unlink(entryPath.c_str());
                        removed = true;
                    }
                }
            }
        }
        ::closedir(d);
    }

    if (removed) {
        Terminal::Success("Removed manifest: " + realPath);
        launchStoreInBackground("");
        return 0;
    } else {
        Terminal::Warn("Manifest not found in registered manifests: " + realPath);
        return 1;
    }
}

// ─── tau activate ─────────────────────────────────────────────────────────────

static int cmdActivate(const String &manifestArg) {
    String manifestPath = manifestArg;
    if (manifestPath.isEmpty()) {
        manifestPath = findNearestManifest(resolveCwd());
        if (manifestPath.isEmpty()) {
            manifestPath = resolveCwd() + "/tau.yml";
        }
    }

    String binDirPath = resolveCwd() + "/bin";

    if (pathExists(manifestPath)) {
        Array<String> visited;
        ParseError err;
        ParsedManifest parsed = Manifest::load(manifestPath, visited, err);
        if (err.ok) {
            String baseDir = manifestPath;
            long long sl = rfind(baseDir, '/');
            if (sl >= 0) baseDir = baseDir.substring(0, (size_t)sl);

            for (size_t i = 0; i < parsed.directives.length(); ++i) {
                Directive *d = parsed.directives[i];
                if (d && d->kind == DirectiveKind::BinDir) {
                    auto *bd = static_cast<DBinDir *>(d);
                    if (!bd->path.isEmpty()) {
                        String p = bd->path;
                        if (!p.startsWith("/")) p = baseDir + "/" + p;
                        binDirPath = p;
                        break;
                    }
                }
            }
        }
    }

    char realBin[4096] = {};
    if (::realpath(binDirPath.c_str(), realBin)) {
        binDirPath = String(realBin);
    }

    ::printf("export PATH=\"%s:$PATH\"\n", binDirPath.c_str());
    return 0;
}

static int launchStoreInBackground(const String &manifestPath) {
    String realPath = manifestPath;
    char rbuf[4096];
    if (::realpath(manifestPath.c_str(), rbuf)) realPath = String(rbuf);

    String storeBin = resolveExecutable("tau-store");

    bool isGlobal = storeBin.startsWith(Config::tauGlobal());
    bool useSudo = false;

    if (isGlobal) {
        if (::getuid() == 0) {
            useSudo = false;
        } else {
            if (::system("sudo -n true >/dev/null 2>&1") == 0) {
                useSudo = true;
            } else {
                Terminal::Warn("sudo permission not available for global store; falling back to user-local tau at " + Config::tauPath());
                String userStore = Config::tauPath() + "/source/build/tau-store";
                if (pathExists(userStore) && ::access(userStore.c_str(), X_OK) == 0) {
                    storeBin = userStore;
                }
                useSudo = false;
            }
        }
    } else {
        Terminal::Warn("Using user-local tau-store (" + storeBin + "); global store is not active.");
        useSudo = false;
    }

    int pipefd[2];
    if (::pipe(pipefd) != 0) return 1;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return 1;
    }

    if (pid == 0) {
        ::close(pipefd[0]);
        ::setsid();

        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);

        if (useSudo) {
            ::execlp("sudo", "sudo", "-n", storeBin.c_str(), nullptr);
        }
        ::execlp(storeBin.c_str(), storeBin.c_str(), nullptr);
        ::_exit(127);
    }

    ::close(pipefd[1]);

    FILE *fp = ::fdopen(pipefd[0], "r");
    if (fp) {
        char lineBuf[4096];
        String expectedCompletion = realPath + " completed";
        while (::fgets(lineBuf, sizeof(lineBuf), fp)) {
            String line(lineBuf);
            line = line.trim();
            if (line.find(expectedCompletion) >= 0) {
                break;
            }
        }
        ::fclose(fp);
    } else {
        ::close(pipefd[0]);
    }
    return 0;
}

// ─── tau github ───────────────────────────────────────────────────────────────

static int cmdGitHub(const Array<String> &repos,
                      const String &targetOverride,
                      bool global_) {
    String manifestPath;
    if (global_) {
        manifestPath = Config::globalManifestPath();
    } else {
        manifestPath = findNearestManifest(resolveCwd());
    }

    for (size_t i = 0; i < repos.length(); ++i) {
        const String &spec = repos[i];
        GHRepo repo;
        if (!GHRepo::parse(spec, repo)) {
            Terminal::Error("Invalid repo spec: " + spec);
            continue;
        }

        Terminal::Info("Adding " + repo.owner + "/" + repo.repo + " …");

        Resource::LinuxFS fs;
        String branchSpec = repo.ref.isEmpty() ? "main" : repo.ref;
        String relTarget = targetOverride.isEmpty() ? ("./deps/" + repo.owner + "/" + repo.repo) : targetOverride;
        String newEntry =
            "\n- git:\n"
            "    source: \"https://github.com/" + repo.owner + "/" + repo.repo + ".git\"\n"
            "    branch: " + branchSpec + "\n"
            "    target: " + relTarget + "\n";

        fs.append(manifestPath, newEntry);
        Terminal::Info("Added " + repo.owner + "/" + repo.repo + " → " + relTarget);

        // Ensure manifest is symlinked into target manifests dir
        String targetManifests = Config::targetManifestsDir();
        Config::ensureLayout();
        long long lastSlash = rfind(manifestPath, '/');
        String baseName = (lastSlash >= 0) ? manifestPath.substring((size_t)lastSlash + 1) : manifestPath;
        if (!baseName.endsWith(".yml") && !baseName.endsWith(".yaml")) baseName += ".yml";
        String linkPath = targetManifests + "/" + baseName;
        if (!pathExists(linkPath)) {
            (void)::symlink(manifestPath.c_str(), linkPath.c_str());
        }

        // Spawn tau-store process in background and wait for this manifest to complete execution & symlinking
        launchStoreInBackground(manifestPath);
    }

    return 0;
}


// ─── tau run ──────────────────────────────────────────────────────────────────

static int cmdRun(Command &args) {
    bool   detach       = args.flag("--detach -d");
    bool   attachIn     = args.flag("--in -i");
    String name         = args.option("--name -n").string();
    String detachKey    = args.option("--detach-key -k").defaults("ctrl+b").string();
    String colOpt       = args.option("--col").defaults("auto").string();
    String rowOpt       = args.option("--row").defaults("auto").string();
    bool   headless     = args.flag("--headless");
    String headOpt      = args.option("--head").string();
    bool   copyYaml     = args.option("--copy -c").defaults("true").boolean();
    bool   watch        = args.option("--watch -w").defaults("true").boolean();
    bool   remove_      = args.flag("--remove -r");
    bool   global_      = args.flag("--global -g");
    bool   debug        = args.flag("--debug -v");
    String timestampOpt = args.option("--timestamp -t").string();
    if (debug) g_debugMode = true;

    int cols = 80, rows = 24;
    resolveTerminalSize(colOpt, rowOpt, cols, rows);

    auto optHist = args.option("--history -h");
    int historyLines = 0;
    if (optHist) {
        String val = optHist.string();
        if (val.isEmpty() || val == "true" || val == "all") {
            historyLines = 1000000;
        } else {
            historyLines = (int)optHist.integer();
            if (historyLines <= 0) historyLines = 1000000;
        }
    }

    Array<String> positionals;
    Array<String> extraArgs;
    bool afterDashDash = false;
    for (size_t i = 0; ; ++i) {
        String p = args[i];
        if (p.isEmpty()) break;
        if (p == "--") {
            afterDashDash = true;
            continue;
        }
        if (afterDashDash) {
            extraArgs.push(p);
        } else {
            positionals.push(p);
        }
    }

    if (positionals.length() == 0) {
        Terminal::Fatal("tau run: no manifest or instance specified");
        return 1;
    }

    String firstArg = positionals[0];
    String instFolderPath;

    if (firstArg.endsWith("/") || pathExists(firstArg + "/instance.yml") || pathExists(firstArg + "/snapshots")) {
        char r[4096];
        if (::realpath(firstArg.c_str(), r)) instFolderPath = String(r);
        else instFolderPath = firstArg;
    } else if (pathExists(Config::instanceDir(firstArg) + "/instance.yml") || pathExists(Config::instanceDir(firstArg) + "/snapshots")) {
        instFolderPath = Config::instanceDir(firstArg);
    }

    String snapshotTimestamp;
    if (!instFolderPath.isEmpty()) {
        snapshotTimestamp = Snapshot::findMatching(instFolderPath, timestampOpt);
        InstanceState prevInstState;
        Instance::load(instFolderPath, prevInstState);
        if (name.isEmpty() && !prevInstState.name.isEmpty()) {
            name = prevInstState.name;
        }

        positionals.clear();
        if (!snapshotTimestamp.isEmpty()) {
            SnapshotInfo sInfo;
            if (Snapshot::load(instFolderPath, snapshotTimestamp, sInfo)) {
                if (!sInfo.manifestPath.isEmpty() && pathExists(sInfo.manifestPath)) {
                    positionals.push(sInfo.manifestPath);
                } else if (pathExists(instFolderPath + "/instance.yml")) {
                    positionals.push(instFolderPath + "/instance.yml");
                }
            }
        }
        if (positionals.length() == 0) {
            if (pathExists(instFolderPath + "/instance.yml")) {
                positionals.push(instFolderPath + "/instance.yml");
            } else if (!prevInstState.manifestPath.isEmpty() && pathExists(prevInstState.manifestPath)) {
                positionals.push(prevInstState.manifestPath);
            }
        }
        if (!snapshotTimestamp.isEmpty()) {
            Terminal::Info("Restoring instance from snapshot " + snapshotTimestamp);
        }
    } else {
        for (size_t i = 0; i < positionals.length(); ++i) {
            String p = positionals[i];
            if (p[0] != '/') {
                char resolved[4096];
                if (::realpath(p.c_str(), resolved))
                    p = String(resolved);
            }
            positionals[i] = p;
            if (!pathExists(p)) {
                Terminal::Error("Manifest or instance not found: " + p);
                return 1;
            }
        }
    }

    if (positionals.length() == 0) {
        Terminal::Fatal("tau run: no valid manifest found for instance");
        return 1;
    }

    String lastEntry = positionals[positionals.length() - 1];

    if (name.isEmpty()) {
        Array<String> visited;
        ParseError ferr;
        ParsedManifest pm = Manifest::load(lastEntry, visited, ferr);
        if (ferr.ok && !pm.meta.name.isEmpty()) {
            name = pm.meta.name;
        }
    }
    String instanceName = uniqueInstanceName(name.isEmpty() ? "0" : name);
    bool attachStdin = attachIn || !detach;

    String instDir;
    if (headless) {
        instDir = "";
        copyYaml = false;
    } else if (!headOpt.isEmpty()) {
        instDir = headOpt;
        instDir = instDir.replace("NAME", instanceName).replace("%name", instanceName).replace("{NAME}", instanceName);
        if (instDir.endsWith("/")) instDir = instDir.substring(0, instDir.length() - 1);
        mkdirP(instDir);
    } else {
        Config::ensureLayout();
        instDir = Config::instanceDir(instanceName);
        mkdirP(instDir);
    }

    String workDir;
    char r[4096];
    if (::realpath(lastEntry.c_str(), r)) {
        String p(r);
        long long sl = rfind(p, '/');
        if (sl >= 0) workDir = p.substring(0, (size_t)sl);
    }

    return launchSpawnProcess(instanceName, positionals, workDir,
                               detach, attachStdin, detachKey, historyLines, debug, cols, rows,
                               headless, remove_, global_, lastEntry, instDir, copyYaml, watch, extraArgs);
}








// ─── tau attach (alias to tau cat -i) ─────────────────────────────────────────

static int cmdAttach(Command &args);


// ─── tau snapshot ─────────────────────────────────────────────────────────────

static int cmdSnapshot(Command &args) {
    String spec = args[0];
    if (spec.isEmpty()) {
        Terminal::Fatal("tau snapshot: no instance specified");
        return 1;
    }

    String instName, spawnName;
    if (!Instance::resolve(spec, Config::instancesPath(), instName, spawnName)) {
        Terminal::Error("Instance not found: " + spec);
        return 1;
    }

    String instDir = Config::instanceDir(instName);
    IPCClient client;
    if (!client.connect(instDir, /*quiet=*/true)) {
        Terminal::Error("Cannot connect to " + instName);
        return 1;
    }

    String ts = client.snapshot();
    if (!ts.isEmpty()) {
        Terminal::Success("Snapshot " + ts + " created for " + instName);
        return 0;
    }
    Terminal::Error("Failed to take snapshot for " + instName);
    return 1;
}

// ─── tau freeze ───────────────────────────────────────────────────────────────

static int cmdFreeze(Command &args) {
    String spec = args[0];
    if (spec.isEmpty()) {
        Terminal::Fatal("tau freeze: no instance specified");
        return 1;
    }

    String instName, spawnName;
    if (!Instance::resolve(spec, Config::instancesPath(), instName, spawnName)) {
        Terminal::Error("Instance not found: " + spec);
        return 1;
    }

    String instDir = Config::instanceDir(instName);
    IPCClient client;
    if (!client.connect(instDir, /*quiet=*/true)) {
        Terminal::Error("Cannot connect to " + instName);
        return 1;
    }

    String ts = client.freeze();
    if (!ts.isEmpty()) {
        Terminal::Success("Frozen instance " + instName + " (snapshot " + ts + ")");
        return 0;
    }
    Terminal::Error("Failed to freeze instance " + instName);
    return 1;
}

// ─── tau stop ─────────────────────────────────────────────────────────────────

static int cmdStop(Command &args) {
    String spec     = args[0];
    int    timeout  = (int)args.option("--timeout -t").defaults("20").integer();
    String sigStr   = args.option("--signal -s").defaults("SIGTERM").string();
    bool   kill_    = args.flag("--kill -k");

    if (spec.isEmpty()) {
        Terminal::Fatal("tau stop: no instance specified");
        return 1;
    }

    String instName, spawnName;
    if (!Instance::resolve(spec, Config::instancesPath(), instName, spawnName)) {
        Terminal::Error("Instance not found: " + spec);
        return 1;
    }

    int sig = SIGTERM;
    if (kill_)         sig = SIGKILL;
    else if (!sigStr.isEmpty()) {
        if (sigStr == "SIGKILL" || sigStr == "9")  sig = SIGKILL;
        if (sigStr == "SIGTERM" || sigStr == "15") sig = SIGTERM;
        if (sigStr == "SIGHUP"  || sigStr == "1")  sig = SIGHUP;
    }

    IPCClient client;
    if (!client.connect(Config::instanceDir(instName), /*quiet=*/true)) {
        InstanceState state;
        if (Instance::load(Config::instanceDir(instName), state)) {
            if (state.spawnPID > 0 && ::kill(state.spawnPID, 0) == 0) {
                ::kill(-state.spawnPID, sig);
                ::kill(state.spawnPID, sig);
                Terminal::Info("Sent signal to process " + intStr(state.spawnPID));
                return 0;
            }
        }
        ::system(("rm -rf '" + Config::instanceDir(instName) + "'").c_str());
        Terminal::Info("Instance " + instName + " was not running (cleaned up stale session).");
        return 0;
    }

    bool ok = client.stop(spawnName, sig, timeout * 1000);
    if (ok) {
        InstanceState state;
        if (Instance::load(Config::instanceDir(instName), state)) {
            for (size_t si = 0; si < state.spawns.length(); ++si) {
                if (state.spawns[si].pid > 0) {
                    ::kill(-state.spawns[si].pid, SIGKILL);
                    ::kill(state.spawns[si].pid, SIGKILL);
                }
            }
            if (state.spawnPID > 0 && ::kill(state.spawnPID, 0) == 0) {
                ::kill(-state.spawnPID, SIGKILL);
                ::kill(state.spawnPID, SIGKILL);
            }
        }
        ::system(("rm -rf '" + Config::instanceDir(instName) + "'").c_str());
        Terminal::Info("Stopped " + spec);

        // Terminate any lingering fuse2fs and socat forwarder processes for the instance
        if (state.spawnPID > 0) {
            String killFwd = String("pkill -9 -f 'nsenter -t ") + intStr(state.spawnPID) + "' 2>/dev/null";
            (void)::system(killFwd.c_str());
        }
        if (!state.manifestPath.isEmpty()) {
            String pkillCmd = "pkill -9 -f 'fuse2fs' 2>/dev/null";
            (void)::system(pkillCmd.c_str());
        }

        Monitor::broadcast("STOP", instName, 0, "exit_code=0");
        return 0;
    }

    InstanceState state;
    if (Instance::load(Config::instanceDir(instName), state)) {
        for (size_t si = 0; si < state.spawns.length(); ++si) {
            if (state.spawns[si].pid > 0) {
                ::kill(-state.spawns[si].pid, SIGKILL);
                ::kill(state.spawns[si].pid, SIGKILL);
            }
        }
        if (state.spawnPID > 0 && ::kill(state.spawnPID, 0) == 0) {
            ::kill(-state.spawnPID, SIGKILL);
            ::kill(state.spawnPID, SIGKILL);
        }
    }
    ::system(("rm -rf '" + Config::instanceDir(instName) + "'").c_str());
    Terminal::Info("Stopped " + spec);
    Monitor::broadcast("STOP", instName, 0, "exit_code=0");
    return 0;
}



// ─── tau cat ──────────────────────────────────────────────────────────────────

static int cmdCat(Command &args) {
    String spec       = args[0];
    bool   attachIn   = args.flag("--in -i");
    bool   detach     = args.flag("--detach -d");
    String detachKey  = args.option("--detach-key -k").defaults("ctrl+b").string();
    String colOpt     = args.option("--col -c").defaults("auto").string();
    String rowOpt     = args.option("--row -r").defaults("auto").string();

    int cols = 80, rows = 24;
    resolveTerminalSize(colOpt, rowOpt, cols, rows);

    int historyLines = 1000000;
    auto optHist = args.option("--history -h");
    if (!optHist.string().isEmpty() && optHist.string() != "false") {
        String val = optHist.string();
        if (val == "true" || val == "all") historyLines = 1000000;
        else historyLines = (int)optHist.integer();
        if (historyLines <= 0) historyLines = 1000000;
    } else if (optHist.string() == "false") {
        historyLines = 0;
    }

    if (spec.isEmpty()) {
        spec = "0:0";
    }

    String instName, spawnName;
    if (!Instance::resolve(spec, Config::instancesPath(), instName, spawnName)) {
        Terminal::Error("Instance not found: " + spec);
        return 1;
    }

    if (detach) {
        return 0;
    }

    bool streamOutput = attachIn;
    return attachToInstance(instName, spawnName, attachIn, detachKey, historyLines, cols, rows, streamOutput);
}



static int cmdAttach(Command &args) {
    String spec       = args[0];
    String detachKey  = args.option("--detach-key -k").defaults("ctrl+b").string();
    String colOpt     = args.option("--col -c").defaults("auto").string();
    String rowOpt     = args.option("--row -r").defaults("auto").string();

    int cols = 80, rows = 24;
    resolveTerminalSize(colOpt, rowOpt, cols, rows);

    int historyLines = 1000000;
    auto optHist = args.option("--history -h");
    if (!optHist.string().isEmpty() && optHist.string() != "false") {
        String val = optHist.string();
        if (val == "true" || val == "all") historyLines = 1000000;
        else historyLines = (int)optHist.integer();
        if (historyLines <= 0) historyLines = 1000000;
    } else if (optHist.string() == "false") {
        historyLines = 0;
    }

    if (spec.isEmpty()) {
        spec = "0:0";
    }

    String instName, spawnName;
    if (!Instance::resolve(spec, Config::instancesPath(), instName, spawnName)) {
        Terminal::Error("Instance not found: " + spec);
        return 1;
    }

    return attachToInstance(instName, spawnName, /*attachStdin=*/true, detachKey, historyLines, cols, rows, /*streamOutput=*/true);
}


// ─── tau ls ───────────────────────────────────────────────────────────────────

static String formatMemBytes(unsigned long long bytes) {
    char buf[64];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        ::snprintf(buf, sizeof(buf), "%.1fG", (double)bytes / (1024.0 * 1024.0 * 1024.0));
        return String(buf);
    }
    if (bytes >= 1024ULL * 1024ULL) {
        ::snprintf(buf, sizeof(buf), "%.1fM", (double)bytes / (1024.0 * 1024.0));
        return String(buf);
    }
    if (bytes >= 1024ULL) {
        ::snprintf(buf, sizeof(buf), "%.1fK", (double)bytes / 1024.0);
        return String(buf);
    }
    return intStr(bytes) + "B";
}

struct CpuSample {
    unsigned long long ticks = 0;
    double timestamp = 0.0;
};

static Map<int, CpuSample> s_prevCpuSamples;

static void collectProcessTree(pid_t rootPid, Array<pid_t> &outPids) {
    if (rootPid <= 0) return;
    outPids.push(rootPid);

    DIR *procDir = ::opendir("/proc");
    if (!procDir) return;

    // Scan all /proc/[0-9]+/stat to find child -> parent relations
    Array<pid_t> currentGeneration;
    currentGeneration.push(rootPid);

    // Read all processes and their PPID once
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

    // Iteratively collect all descendant PIDs
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

static unsigned long long getPidRssBytes(pid_t pid) {
    if (pid <= 0) return 0;
    Array<pid_t> pids;
    collectProcessTree(pid, pids);

    unsigned long long totalRss = 0;
    long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;

    for (size_t i = 0; i < pids.length(); ++i) {
        String path = "/proc/" + intStr(pids[i]) + "/statm";
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;
        char buf[128];
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        unsigned long size = 0, resident = 0;
        if (::sscanf(buf, "%lu %lu", &size, &resident) == 2) {
            totalRss += (unsigned long long)resident * (unsigned long long)pageSize;
        }
    }
    return totalRss;
}

static String getPidCpuUsage(pid_t pid) {
    if (pid <= 0) return "0%";
    Array<pid_t> pids;
    collectProcessTree(pid, pids);

    unsigned long long totalTicks = 0;
    unsigned long long rootStarttime = 0;
    long hertz = ::sysconf(_SC_CLK_TCK);
    if (hertz <= 0) hertz = 100;

    for (size_t i = 0; i < pids.length(); ++i) {
        String path = "/proc/" + intStr(pids[i]) + "/stat";
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;
        char buf[1024];
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';

        const char *p = ::strrchr(buf, ')');
        if (!p) continue;
        p++;

        char state;
        int ppid, pgrp, session, tty_nr, tpgid;
        unsigned int flags;
        unsigned long minflt, cminflt, majflt, cmajflt, utime = 0, stime = 0;
        long cutime, cstime, priority, nice, num_threads, itrealvalue;
        unsigned long long starttime = 0;

        if (::sscanf(p, " %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu",
                     &state, &ppid, &pgrp, &session, &tty_nr, &tpgid,
                     &flags, &minflt, &cminflt, &majflt, &cmajflt,
                     &utime, &stime, &cutime, &cstime, &priority, &nice,
                     &num_threads, &itrealvalue, &starttime) >= 13) {
            totalTicks += (utime + stime);
            if (pids[i] == pid) {
                rootStarttime = starttime;
            }
        }
    }

    double sysUptime = 0.0;
    int ufd = ::open("/proc/uptime", O_RDONLY);
    if (ufd >= 0) {
        char ubuf[64];
        ssize_t un = ::read(ufd, ubuf, sizeof(ubuf) - 1);
        ::close(ufd);
        if (un > 0) {
            ubuf[un] = '\0';
            ::sscanf(ubuf, "%lf", &sysUptime);
        }
    }

    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

    double pct = 0.0;
    CpuSample *prev = s_prevCpuSamples.get(pid);
    if (prev && prev->timestamp > 0 && (now - prev->timestamp) >= 0.1) {
        double deltaSec = now - prev->timestamp;
        double deltaTicks = (double)(totalTicks >= prev->ticks ? totalTicks - prev->ticks : 0);
        pct = ((deltaTicks / (double)hertz) / deltaSec) * 100.0;
    } else {
        double processAge = sysUptime - ((double)rootStarttime / (double)hertz);
        if (processAge <= 0.05) processAge = 0.05;
        double cpuSec = (double)totalTicks / (double)hertz;
        pct = (cpuSec / processAge) * 100.0;
    }

    s_prevCpuSamples[pid] = { totalTicks, now };

    char pbuf[32];
    ::snprintf(pbuf, sizeof(pbuf), "%.1f%%", pct);
    return String(pbuf);
}

// ─── tau inspect ─────────────────────────────────────────────────────────────

static int cmdInspect(Command &args) {
    String name = args[0];
    if (name.isEmpty()) {
        Terminal::Fatal("tau inspect: no instance specified");
        return 1;
    }

    String instName, spawnName;
    if (!Instance::resolve(name, Config::instancesPath(), instName, spawnName)) {
        Terminal::Error("Instance not found: " + name);
        return 1;
    }

    String instDir = Config::instanceDir(instName);
    String instYml = instDir + "/instance.yml";
    InstanceState state;
    Instance::load(instDir, state);

    if (pathExists(instYml)) {
        Array<String> visited;
        ParseError perr;
        ParsedManifest pm = Manifest::load(instYml, visited, perr);
        if (perr.ok) {
            for (auto *dir : pm.directives) {
                if (dir->kind == DirectiveKind::Spawn) {
                    auto *spDir = static_cast<DSpawn*>(dir);
                    String sname = spDir->name.isEmpty() ? "0" : spDir->name;
                    const SpawnState *st = state.findSpawn(sname);
                    if (st) {
                        if (st->status == SpawnStatus::Exited || st->status == SpawnStatus::Failed) {
                            spDir->exitCode = st->exitCode;
                        } else if (st->pid > 0) {
                            spDir->pid = st->pid;
                        }
                    }
                }
            }
            String content = Manifest::toYAML(pm.directives);
            ::fputs(content.c_str(), stdout);
            return 0;
        }
    }

    if (!state.name.isEmpty()) {
        ::puts(Instance::toYAML(state).c_str());
        return 0;
    }

    Terminal::Error("Instance not found: " + name);
    return 1;
}

// ─── tau ls ──────────────────────────────────────────────────────────────────

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

static String renderInstancesYAML(const String &regexPattern, bool showMem, bool showCpuset, bool showCpu, bool showDesc, bool showFrozen = false) {
    Array<String> names = Instance::listAll(Config::instancesPath());
    String out;

    for (size_t i = 0; i < names.length(); ++i) {
        String instName = names[i];
        if (!matchInstanceRegex(regexPattern, instName)) {
            continue;
        }

        String instDir = Config::instanceDir(instName);
        InstanceState state;
        if (!Instance::load(instDir, state)) continue;

        bool isAlive = (state.spawnPID > 0 && ::kill(state.spawnPID, 0) == 0);
        bool isFrozen = (state.status == InstanceStatus::Frozen);
        if (!isAlive && !isFrozen) {
            ::system(("rm -rf '" + instDir + "'").c_str());
            continue;
        }
        if (showFrozen && !isFrozen) continue;
        if (!showFrozen && isFrozen && regexPattern.isEmpty()) continue;

        out += "- name: " + instName + "\n";
        if (isFrozen) {
            out += "  status: frozen\n";
        }

        String desc;
        String maxMem;
        String maxCpu;
        String cpuSet;

        String instYml = instDir + "/instance.yml";
        if (pathExists(instYml)) {
            Array<String> visited;
            ParseError perr;
            ParsedManifest pm = Manifest::load(instYml, visited, perr);
            if (perr.ok) {
                for (auto *dir : pm.directives) {
                    if (!dir) continue;
                    if (dir->kind == DirectiveKind::Description) {
                        desc = static_cast<DDescription*>(dir)->value;
                    } else if (dir->kind == DirectiveKind::Memory) {
                        auto *m = static_cast<DMemory*>(dir);
                        if (!m->rawMax.isEmpty()) maxMem = m->rawMax;
                        else maxMem = m->raw;
                    } else if (dir->kind == DirectiveKind::CPU) {
                        auto *c = static_cast<DCPU*>(dir);
                        if (c->quotaMax >= 0) maxCpu = intStr(c->quotaMax) + "%";
                        else if (c->quota >= 0) maxCpu = intStr(c->quota) + "%";
                    } else if (dir->kind == DirectiveKind::CPUSet) {
                        cpuSet = static_cast<DCPUSet*>(dir)->cpus;
                    }
                }
            }
        }

        if (showDesc && !desc.isEmpty()) {
            out += "  description: \"" + desc.replace("\"", "\\\"") + "\"\n";
        }

        out += "  spawn_groups:\n";
        out += "    -";

        bool hasGroupLimit = (showCpu && !maxCpu.isEmpty()) ||
                             (showCpuset && !cpuSet.isEmpty()) ||
                             (showMem && !maxMem.isEmpty());

        if (hasGroupLimit) {
            out += "\n";
            if (showCpu && !maxCpu.isEmpty()) {
                out += "      max_cpu: " + maxCpu + "\n";
            }
            if (showCpuset && !cpuSet.isEmpty()) {
                out += "      cpu_set: " + cpuSet + "\n";
            }
            if (showMem && !maxMem.isEmpty()) {
                out += "      max_memory: " + maxMem + "\n";
            }
            out += "      spawns:\n";
        } else {
            out += " spawns:\n";
        }

        if (state.spawns.length() == 0 && state.spawnPID > 0) {
            out += "        - command: \"tau-instance\"\n";
            if (showCpu) out += "          cpu: " + getPidCpuUsage(state.spawnPID) + "\n";
            if (showMem) out += "          memory: " + formatMemBytes(getPidRssBytes(state.spawnPID)) + "\n";
            out += "          pid: " + intStr(state.spawnPID) + "\n";
        } else {
            for (size_t si = 0; si < state.spawns.length(); ++si) {
                const auto &sp = state.spawns[si];
                out += "        - command: \"" + sp.command.replace("\"", "\\\"") + "\"\n";
                if (showCpu) {
                    out += "          cpu: " + getPidCpuUsage(sp.pid > 0 ? sp.pid : state.spawnPID) + "\n";
                }
                if (showMem) {
                    out += "          memory: " + formatMemBytes(getPidRssBytes(sp.pid > 0 ? sp.pid : state.spawnPID)) + "\n";
                }
                out += "          pid: " + intStr(sp.pid > 0 ? sp.pid : state.spawnPID) + "\n";
            }
        }
    }

    return out;
}

static int cmdLs(Command &args) {
    bool showMem    = args.flag("--memory -m");
    bool showCpuset = args.flag("--cpuset -s");
    bool showCpu    = args.flag("--cpu -c");
    bool showDesc   = args.flag("--description -d");
    bool showFrozen = args.flag("--frozen -f");
    bool watch      = args.flag("--watch -w");
    String intervalStr = args.option("--interval -i").defaults("3s").string();

    String regexPattern;
    for (size_t i = 0; ; ++i) {
        String p = args[i];
        if (p.isEmpty()) break;
        if (!p.startsWith("-")) {
            regexPattern = p;
            break;
        }
    }

    double intervalSec = Manifest::parseDuration(intervalStr);
    if (intervalSec <= 0) intervalSec = 3.0;

    while (true) {
        String output = renderInstancesYAML(regexPattern, showMem, showCpuset, showCpu, showDesc, showFrozen);
        if (!output.isEmpty()) {
            ::fputs(output.c_str(), stdout);
        } else {
            ::puts("[]");
        }
        if (!watch) break;
        ::puts("---");
        ::fflush(stdout);
        ::usleep((useconds_t)(intervalSec * 1000000.0));
    }
    return 0;
}

// ─── tau monitor ──────────────────────────────────────────────────────────────

static int cmdMonitor(Command &args) {
    bool   powerEvents  = args.flag("--power -p");
    bool   ipEvents     = args.flag("--ip -i");
    bool   allowNewer   = args.flag("--new -n");
    String regexPattern;

    for (size_t i = 0; ; ++i) {
        String p = args[i];
        if (p.isEmpty()) break;
        if (!p.startsWith("-")) {
            regexPattern = p;
            break;
        }
    }

    if (regexPattern.isEmpty()) regexPattern = ".*";

    return Monitor::runListener(regexPattern, powerEvents, ipEvents, allowNewer);
}



// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    ::signal(SIGPIPE, SIG_IGN);
    Tau::Config::init();
    Tau::Config::ensureLayout();


    Command args(argc, argv);
    args
        .description("tau — package manager and containerization engine")
        .version(TAU_VERSION);

    bool isTgh = (argc > 0 && ::strstr(argv[0], "tgh") != nullptr);

    if (isTgh) {
        Array<String> repos;
        for (size_t i = 0; ; ++i) {
            String p = args[i];
            if (p.isEmpty()) break;
            repos.push(p);
        }
        return cmdGitHub(repos, "", false);
    }

    if (args.flag("--version -v")) {
        ::puts(("tau " + String(TAU_VERSION)).c_str());
        return 0;
    }

    if (args.primary().isEmpty() || args.primary() == "help" || args.flag("--help")) {
        ::puts(Terminal::Box(
            Terminal::Bold("tau") + " v" + String(TAU_VERSION) + "\n\n"
            "  " + Terminal::Cyan("tau init") + "                       Create tau.yml in cwd and register it\n"
            "  " + Terminal::Cyan("tau add [manifest]") + "             Register a manifest with the store\n"
            "  " + Terminal::Cyan("tau remove <manifest>") + "          Remove a manifest from the store\n"
            "  " + Terminal::Cyan("tau activate [manifest]") + "        Output shell command to prepend bindir to PATH\n"
            "  " + Terminal::Cyan("tau github <user/repo>")  + "         Add a GitHub dependency\n"
            "  " + Terminal::Cyan("tgh <user/repo>")          + "         Shorthand for tau github\n"
            "  " + Terminal::Cyan("tau run <manifest>")       + "         Run an instance\n"
            "  " + Terminal::Cyan("tau cat <name[:spawn]>")   + "         Stream output, view history, or attach stdin\n"
            "  " + Terminal::Cyan("tau stop <name[:spawn]>")  + "         Stop a spawn or instance\n"
            "  " + Terminal::Cyan("tau snapshot <name>")      + "         Take snapshot of memory & sockets\n"
            "  " + Terminal::Cyan("tau freeze <name>")        + "         Freeze instance and save snapshot\n"
            "  " + Terminal::Cyan("tau ls [regex]")           + "         List instances (--memory,-m, --cpu,-c, --cpuset,-s, --frozen,-f, --watch,-w)\n"
            "  " + Terminal::Cyan("tau inspect <name>")       + "         Inspect instance manifest and state\n"
            "  " + Terminal::Cyan("tau monitor [regex]")      + "         Monitor instance events (--power,-p, --ip,-i, --new,-n)\n",
            "tau " + String(TAU_VERSION)
        ).c_str());
        return 0;
    }


    String cmd = args.primary();

    if (cmd == "init") return cmdInit();

    if (cmd == "add") {
        Command &addCmd = args.command("add");
        String path = addCmd[0];
        return cmdAdd(path);
    }

    if (cmd == "remove" || cmd == "rm") {
        Command &removeCmd = args.command("remove rm");
        String path = removeCmd[0];
        return cmdRemove(path);
    }

    if (cmd == "activate") {
        Command &activateCmd = args.command("activate");
        String path = activateCmd[0];
        return cmdActivate(path);
    }

    if (cmd == "github" || cmd == "gh") {
        Command &githubCmd = args.command("github gh");
        bool global = githubCmd.flag("--global");
        String target = githubCmd.option("--target").string();
        Array<String> repos;
        for (size_t i = 0; ; ++i) {
            String p = githubCmd[i];
            if (p.isEmpty()) break;
            repos.push(p);
        }
        return cmdGitHub(repos, target, global);
    }

    if (cmd == "run")      return cmdRun     (args.command("run"));
    if (cmd == "attach")   return cmdAttach  (args.command("attach"));
    if (cmd == "stop")     return cmdStop    (args.command("stop"));
    if (cmd == "snapshot") return cmdSnapshot(args.command("snapshot"));
    if (cmd == "freeze")   return cmdFreeze  (args.command("freeze"));
    if (cmd == "cat")      return cmdCat     (args.command("cat"));
    if (cmd == "inspect")  return cmdInspect (args.command("inspect"));
    if (cmd == "ls")       return cmdLs      (args.command("ls"));
    if (cmd == "monitor")  return cmdMonitor (args.command("monitor"));

    Terminal::Error("Unknown command: " + cmd);
    Terminal::Info("Run `tau --help` for usage.");
    return 1;
}

