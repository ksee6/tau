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
#include <Tau/Util.hpp>


#include <Terminal/Command.hpp>
#include <Terminal/Format.hpp>
#include <Resource/File.hpp>

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
        InstanceState state;
        if (Instance::load(instDir, state)) {
            auto *first = state.firstSpawn();
            if (first && !first->name.isEmpty()) spawnName = first->name;
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

    // Wait until the instance completes when running attached in foreground
    while (pathExists(sockPath)) {
        InstanceState st;
        if (Instance::load(instDir, st)) {
            if (st.status != InstanceStatus::Running) {
                return (st.status == InstanceStatus::Failed) ? 1 : 0;
            }
        }
        ::usleep(20000); // 20ms
    }
    InstanceState finalSt;
    if (Instance::load(instDir, finalSt)) {
        return (finalSt.status == InstanceStatus::Failed) ? 1 : 0;
    }
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
                               const String        &sourceManifestPath = "") {

    char selfBuf[4096] = {};
    ssize_t n = ::readlink("/proc/self/exe", selfBuf, sizeof(selfBuf) - 1);
    String spawnBin;
    if (n > 0) {
        String selfPath(selfBuf);
        long long sl = rfind(selfPath, '/');
        if (sl >= 0)
            spawnBin = selfPath.substring(0, (size_t)sl) + "/tau-instance";
    }
    if (spawnBin.isEmpty() || !pathExists(spawnBin))
        spawnBin = "tau-instance";

    // Double-fork + setsid to fully detach tau-instance from the terminal session.
    // First fork: parent continues, intermediate child detaches.
    pid_t pid1 = ::fork();
    if (pid1 < 0) { Terminal::Error("tau: fork failed"); return 1; }
    if (pid1 > 0) {
        // Parent: wait for intermediate child to fork the daemon and exit
        int st; ::waitpid(pid1, &st, 0);

        String instDir = Config::instanceDir(instanceName);
        String sockPath = instDir + "/tau.sock";

        // Poll for socket readiness (up to 3 seconds)
        bool started = false;
        bool ready = false;
        for (int i = 0; i < 300; ++i) {
            if (pathExists(sockPath)) { ready = true; started = true; break; }
            InstanceState ist;
            if (Instance::load(instDir, ist)) {
                started = true;
                if (ist.status != InstanceStatus::Running) {
                    return (ist.status == InstanceStatus::Failed) ? 1 : 0;
                }
            }
            if (started && !pathExists(instDir)) {
                return 0; // Instance ran and finished cleanly
            }
            ::usleep(10000); // 10ms
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

    // Grandchild: fully detached daemon — close all inherited fds above stderr
    ::close(STDIN_FILENO);
    int devNull = ::open("/dev/null", O_RDWR);
    if (devNull >= 0) {
        ::dup2(devNull, STDIN_FILENO);
        ::close(devNull);
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
    if (headless) argv.push("--headless");
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
    argv.push(nullptr);

    ::execvp(spawnBin.c_str(), (char *const *)argv.data());
    ::_exit(127);
}






// ─── tau init ─────────────────────────────────────────────────────────────────

static int cmdInit() {
    String cwd       = resolveCwd();
    String tauYml    = cwd + "/tau.yml";
    String manifests = Config::targetManifestsDir();

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

    Config::ensureLayout();
    long long lastSlash = rfind(cwd, '/');
    String baseName = (lastSlash >= 0) ? cwd.substring((size_t)lastSlash + 1) : cwd;
    String linkName = manifests + "/" + baseName + ".yml";

    ::unlink(linkName.c_str());
    if (::symlink(tauYml.c_str(), linkName.c_str()) == 0)
        Terminal::Info("Symlinked to " + linkName);
    else
        Terminal::Warn("Could not symlink to manifests/ (not critical)");

    return 0;
}

static int launchStoreInBackground(const String &manifestPath) {
    String realPath = manifestPath;
    char rbuf[4096];
    if (::realpath(manifestPath.c_str(), rbuf)) realPath = String(rbuf);

    String storeBin;
    String targetDir = Config::targetManifestsDir();
    String globalStoreBin = Config::tauGlobal() + "/tau-store";
    if (targetDir == Config::tauGlobal() + "/manifests" &&
        pathExists(globalStoreBin) && ::access(globalStoreBin.c_str(), X_OK) == 0) {
        storeBin = globalStoreBin;
    } else {
        char selfBuf[4096] = {};
        ssize_t n = ::readlink("/proc/self/exe", selfBuf, sizeof(selfBuf) - 1);
        if (n > 0) {
            String selfPath(selfBuf);
            long long sl = rfind(selfPath, '/');
            if (sl >= 0)
                storeBin = selfPath.substring(0, (size_t)sl) + "/tau-store";
        }
        if (storeBin.isEmpty() || !pathExists(storeBin))
            storeBin = "tau-store";
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
    bool   detach     = args.flag("--detach -d");
    bool   attachIn   = args.flag("--in -i");
    String name       = args.option("--name -n").string();
    String detachKey  = args.option("--detach-key -k").defaults("ctrl+b").string();
    String colOpt     = args.option("--col -c").defaults("auto").string();
    String rowOpt     = args.option("--row -r").defaults("auto").string();
    bool   headless   = args.flag("--headless -h");
    bool   remove_    = args.flag("--remove -r");
    bool   global_    = args.flag("--global -g");
    bool   debug      = args.flag("--debug -v");
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
    for (size_t i = 0; ; ++i) {
        String p = args[i];
        if (p.isEmpty() || p == "--") break;
        if (p[0] != '/') {
            char resolved[4096];
            if (::realpath(p.c_str(), resolved))
                p = String(resolved);
        }
        positionals.push(p);
    }

    if (positionals.length() == 0) {
        Terminal::Fatal("tau run: no manifest specified");
        return 1;
    }

    for (size_t i = 0; i < positionals.length(); ++i) {
        if (!pathExists(positionals[i])) {
            Terminal::Error("Manifest not found: " + positionals[i]);
            return 1;
        }
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

    Config::ensureLayout();
    String instDir = Config::instanceDir(instanceName);
    Resource::LinuxFS fs;
    fs.mkdir(instDir);

    String workDir;
    char r[4096];
    if (::realpath(lastEntry.c_str(), r)) {
        String p(r);
        long long sl = rfind(p, '/');
        if (sl >= 0) workDir = p.substring(0, (size_t)sl);
    }

    return launchSpawnProcess(instanceName, positionals, workDir,
                               detach, attachStdin, detachKey, historyLines, debug, cols, rows,
                               headless, remove_, global_, lastEntry);
}








// ─── tau attach (alias to tau cat -i) ─────────────────────────────────────────

static int cmdAttach(Command &args);


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
            if (state.spawnPID > 0 && ::kill(state.spawnPID, 0) == 0) {
                ::kill(state.spawnPID, SIGKILL);
            }
        }
        Terminal::Info("Stopped " + spec);

        // Terminate any lingering fuse2fs processes for the instance image
        if (!state.manifestPath.isEmpty()) {
            String pkillCmd = "pkill -9 -f 'fuse2fs' 2>/dev/null";
            (void)::system(pkillCmd.c_str());
        }

        return 0;
    }

    Terminal::Error("Stop failed for " + spec);
    return 1;
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

static int cmdLs(Command &args) {
    String nameFilter = args[0];

    if (!nameFilter.isEmpty()) {
        String instDir = Config::instanceDir(nameFilter);
        String instYml = instDir + "/instance.yml";
        if (pathExists(instYml)) {
            Resource::LinuxFS fs;
            String content = fs.read(instYml);
            ::fputs(content.c_str(), stdout);
            return 0;
        }
        InstanceState state;
        if (!Instance::load(instDir, state)) {
            Terminal::Error("Instance not found: " + nameFilter);
            return 1;
        }
        ::puts(Instance::toYAML(state).c_str());
        return 0;
    }


    Array<String> names = Instance::listAll(Config::instancesPath());
    size_t activeCount = 0;

    for (size_t i = 0; i < names.length(); ++i) {
        String instDir = Config::instanceDir(names[i]);
        InstanceState state;
        if (!Instance::load(instDir, state)) continue;

        IPCClient client;
        bool isAlive = client.connect(instDir, /*quiet=*/true);
        if (!isAlive) {
            if (state.spawnPID > 0 && ::kill(state.spawnPID, 0) == 0) {
                isAlive = true;
            } else {
                ::system(("rm -rf '" + instDir + "'").c_str());
                continue;
            }
        }

        if (state.status != InstanceStatus::Running && !isAlive) continue;

        printf("%-20s  spawns: %zu\n",
               state.name.c_str(),
               state.spawns.length());
        activeCount++;
    }

    if (activeCount == 0) {
        Terminal::Info("No running instances.");
    }
    return 0;
}

// ─── tau monitor ──────────────────────────────────────────────────────────────

static int cmdMonitor(Command &args) {
    bool   powerEvents  = args.flag("--power -p");
    bool   ipEvents     = args.flag("--ip -i");
    bool   allowNewer   = args.flag("--new -n");
    String regexPattern = args[0];

    // If no regex specified or regex is empty, default to ".*"
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
            "  " + Terminal::Cyan("tau init") + "                       Create tau.yml in cwd\n"
            "  " + Terminal::Cyan("tau github <user/repo>")  + "         Add a GitHub dependency\n"
            "  " + Terminal::Cyan("tgh <user/repo>")          + "         Shorthand for tau github\n"
            "  " + Terminal::Cyan("tau run <manifest>")       + "         Run an instance\n"
            "  " + Terminal::Cyan("tau cat <name[:spawn]>")   + "         Stream output, view history, or attach stdin\n"
            "  " + Terminal::Cyan("tau stop <name[:spawn]>")  + "         Stop a spawn or instance\n"
            "  " + Terminal::Cyan("tau ls [name]")            + "         List instances\n"
            "  " + Terminal::Cyan("tau monitor [regex]")      + "         Monitor instance events (--power,-p, --ip,-i, --new,-n)\n",
            "tau " + String(TAU_VERSION)
        ).c_str());
        return 0;
    }


    String cmd = args.primary();

    Command &initCmd    = args.command("init");
    Command &githubCmd  = args.command("github gh");
    Command &runCmd     = args.command("run");
    Command &attachCmd  = args.command("attach");
    Command &stopCmd    = args.command("stop");
    Command &catCmd     = args.command("cat");
    Command &lsCmd      = args.command("ls");
    Command &monitorCmd = args.command("monitor");

    if (initCmd) return cmdInit();

    if (githubCmd) {
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

    if (runCmd)     return cmdRun    (runCmd);
    if (attachCmd)  return cmdAttach (attachCmd);
    if (stopCmd)    return cmdStop   (stopCmd);
    if (catCmd)     return cmdCat    (catCmd);
    if (lsCmd)      return cmdLs     (lsCmd);
    if (monitorCmd) return cmdMonitor(monitorCmd);

    Terminal::Error("Unknown command: " + cmd);
    Terminal::Info("Run `tau --help` for usage.");
    return 1;
}

