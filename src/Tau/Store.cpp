/**
 * @file Store.cpp
 * @brief Store manifest hashing, sandbox execution, and GC.
 */

#include <Tau/Store.hpp>
#include <Tau/Config.hpp>
#include <Tau/Namespace.hpp>
#include <Tau/Runner.hpp>
#include <Tau/GitHub.hpp>
#include <Tau/Util.hpp>

#include <Resource/File.hpp>
#include <Security/Crypto.hpp>

#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

namespace Tau {

using namespace Collection;

// ─── hashContent ──────────────────────────────────────────────────────────────

String Store::hashContent(const String &content) {
    // BLAKE2b with 32-byte output → 64 hex chars
    String raw = Security::hash(content, 32);
    return hexEncode(raw);
}

// ─── injectCommits ────────────────────────────────────────────────────────────

String Store::injectCommits(const String &yaml, const String & /*manifestDir*/) {
    Array<String> lines = yaml.split("\n");
    String result;
    bool inGitBlock = false;

    for (size_t i = 0; i < lines.length(); ++i) {
        String line = lines[i];

        if (line.find("- git:") >= 0 || line.find("-git:") >= 0)
            inGitBlock = true;

        if (inGitBlock && line.find("source:") >= 0) {
            long long colon = line.find("source:");
            String url = line.substring((size_t)colon + 7).trim();
            if ((url.startsWith("'") && url.endsWith("'")) ||
                (url.startsWith("\"") && url.endsWith("\"")))
                url = url.substring(1, url.length() - 1);

            GHRepo repo;
            if (GHRepo::parse(url, repo)) {
                String commit = GitHub::resolveCommit(repo);
                if (!commit.isEmpty())
                    result += "# __commit: " + commit + "\n";
            }
            inGitBlock = false;
        }

        result += line + "\n";
    }
    return result;
}

// ─── injectTimestamps ─────────────────────────────────────────────────────────

String Store::injectTimestamps(const String &yaml, const String &manifestDir) {
    Array<String> lines = yaml.split("\n");
    String result;
    bool inLocalBlock = false;

    for (size_t i = 0; i < lines.length(); ++i) {
        String line = lines[i];

        if (line.find("- local:") >= 0 || line.find("-local:") >= 0)
            inLocalBlock = true;

        if (inLocalBlock && line.find("source:") >= 0) {
            long long colon = line.find("source:");
            String path = line.substring((size_t)colon + 7).trim();
            if (!path.startsWith("/")) path = manifestDir + "/" + path;

            struct stat st;
            if (::stat(path.c_str(), &st) == 0) {
                long long mtime = (long long)st.st_mtim.tv_sec * 1000000000LL +
                                  st.st_mtim.tv_nsec;
                result += "# __mtime: " + intStr(mtime) + "\n";
            }
            inLocalBlock = false;
        }

        result += line + "\n";
    }
    return result;
}

// ─── computeHash ──────────────────────────────────────────────────────────────

String Store::computeHash(const String &manifestPath) {
    Resource::LinuxFS fs;
    String content = fs.read(manifestPath);
    if (content.isEmpty()) return "";

    String basePath = manifestPath;
    long long sl = rfind(basePath, '/');
    if (sl >= 0) basePath = basePath.substring(0, (size_t)sl);

    content = injectCommits(content, basePath);
    content = injectTimestamps(content, basePath);
    return hashContent(content);
}

// ─── isRan / markRan ─────────────────────────────────────────────────────────

bool Store::isRan(const String &hash) {
    String sentinel = Config::storePath() + "/" + hash + "/ran";
    Resource::LinuxFS fs;
    return fs.read(sentinel).trim() == "1";
}

void Store::markRan(const String &hash) {
    String dir = Config::storePath() + "/" + hash;
    mkdirP(dir);
    Resource::LinuxFS fs;
    fs.write(dir + "/ran", "1\n");
}

static String getRealPath(const String &path) {
    char buf[4096];
    if (::realpath(path.c_str(), buf)) return String(buf);
    return path;
}

static void storeLogInfo(const String &manifestPath, const String &msg) {
    String real = getRealPath(manifestPath);
    ::fprintf(stdout, "%s info: %s\n", real.c_str(), msg.c_str());
    ::fflush(stdout);
}

static void storeLogError(const String &manifestPath, const String &msg) {
    String real = getRealPath(manifestPath);
    ::fprintf(stdout, "%s error: %s\n", real.c_str(), msg.c_str());
    ::fflush(stdout);
}

static void storeLogCompleted(const String &manifestPath) {
    String real = getRealPath(manifestPath);
    ::fprintf(stdout, "%s completed\n", real.c_str());
    ::fflush(stdout);
}

// ─── runManifest ──────────────────────────────────────────────────────────────

bool Store::runManifest(const StoreEntry &entry) {
    storeLogInfo(entry.manifestPath, "running manifest [hash=" + entry.hash + "]");

    Array<String> visited;
    ParseError err;
    ParsedManifest parsed = Manifest::load(entry.manifestPath, visited, err);
    if (!err.ok) {
        storeLogError(entry.manifestPath, String("parse: ") + err.message);
        return false;
    }
    storeLogInfo(entry.manifestPath, "directives count=" + intStr(parsed.directives.length()));
    RunnerOptions hostOpts;
    hostOpts.instanceName = entry.hash;
    hostOpts.manifestPath = entry.manifestPath;
    hostOpts.storeMode    = true;
    hostOpts.attachStdin  = false;

    Runner hostRunner(hostOpts);
    try {
        hostRunner.run(parsed.directives);
    } catch (const TauThrow &t) {
        storeLogError(entry.manifestPath, String("throw: ") + t.message);
        return false;
    }

    // Run isolated sandbox execution
    pid_t pid = ::fork();
    if (pid < 0) {
        storeLogError(entry.manifestPath, String("fork: ") + ::strerror(errno));
        return false;
    }

    if (pid == 0) {
        IsolationContext ctx;
        if (!Namespace::enterUser(ctx)) ::_exit(1);

        Array<String> noNet;
        if (!Namespace::enterNet(noNet, ctx)) ::_exit(1);

        if (!Namespace::doChroot(entry.storeDir, ctx)) ::_exit(1);
        Namespace::dropCapabilities();

        ::_exit(0);
    }

    int status;
    ::waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    if (ok) {
        // Enforce store immutability: mark build output read-only to prevent tampering
        String lockCmd = String("chmod -R a-w '") + entry.storeDir + "' 2>/dev/null";
        (void)::system(lockCmd.c_str());

        markRan(entry.hash);
        storeLogCompleted(entry.manifestPath);
    } else {
        storeLogError(entry.manifestPath, "execution failed (kept in store)");
    }
    return ok;
}

// ─── collectDirectivesHashes ──────────────────────────────────────────────────

static void collectDirectivesHashes(const DirectiveList &directives,
                                     const String &manifestDir,
                                     Array<String> &activeHashes) {
    for (size_t i = 0; i < directives.length(); ++i) {
        Directive *d = directives[i];
        if (!d) continue;
        if (d->kind == DirectiveKind::Git) {
            auto *gd = static_cast<DGit *>(d);
            GHRepo repo;
            if (!GHRepo::parse(gd->source, repo)) {
                repo.owner = "";
                repo.repo  = gd->source;
                repo.ref   = gd->branch;
            } else {
                repo.ref = gd->branch;
            }
            String commit = GitHub::resolveCommit(repo);
            String gitUrl = gd->source;
            if (!gitUrl.startsWith("http://") && !gitUrl.startsWith("https://") && !gitUrl.startsWith("git@")) {
                gitUrl = String("https://github.com/") + repo.owner + "/" + repo.repo + ".git";
            }
            String hashName = "git_" + hexEncode(Security::hash(gitUrl + ":" + commit, 8));
            activeHashes.push(hashName);
        } else if (d->kind == DirectiveKind::Local) {
            auto *ld = static_cast<DLocal *>(d);
            String src = ld->source;
            if (!src.startsWith("/")) src = manifestDir + "/" + src;
            String hashName = "local_" + hexEncode(Security::hash(src, 8));
            activeHashes.push(hashName);
        } else if (d->kind == DirectiveKind::And || d->kind == DirectiveKind::Or ||
                   d->kind == DirectiveKind::Nand || d->kind == DirectiveKind::Nor ||
                   d->kind == DirectiveKind::Xor) {
            auto *lb = static_cast<DLogicBlock *>(d);
            collectDirectivesHashes(lb->children, manifestDir, activeHashes);
        } else if (d->kind == DirectiveKind::WaitExit) {
            auto *we = static_cast<DWaitExit *>(d);
            collectDirectivesHashes(we->children, manifestDir, activeHashes);
        }
    }
}

// ─── garbageCollect ───────────────────────────────────────────────────────────

void Store::garbageCollect(const Array<String> &activeHashes) {
    String storePath = Config::storePath();
    DIR *d = ::opendir(storePath.c_str());
    if (!d) return;

    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        String name(ent->d_name);
        if (name.startsWith("oci_layer_") || name.startsWith("local_")) continue;

        bool found = false;
        for (size_t i = 0; i < activeHashes.length(); ++i) {
            if (activeHashes[i] == name) { found = true; break; }
        }

        if (!found) {
            logInfo("tau-store: GC removing ", name);
            ::system(("rm -rf '" + storePath + "/" + name + "'").c_str());
        }
    }
    ::closedir(d);
}

// ─── run ──────────────────────────────────────────────────────────────────────

int Store::run() {
    Config::init();
    Config::ensureLayout();

    String pidPath = Config::tauPath() + "/store_pid";
    Resource::LinuxFS fs;

    // Enforce single instance per TAU_PATH & non-gracefully terminate old store
    String oldPidStr = fs.read(pidPath).trim();
    if (!oldPidStr.isEmpty()) {
        long long oldPid = parseLong(oldPidStr);
        if (oldPid > 0 && (pid_t)oldPid != getpid()) {
            if (::kill((pid_t)oldPid, 0) == 0) {
                ::kill((pid_t)oldPid, SIGKILL);
            }
        }
    }
    fs.write(pidPath, intStr(getpid()) + "\n");

    String manifestsPath = Config::manifestsPath();
    Array<StoreEntry> entries;
    Array<String>     activeHashes;

    DIR *d = ::opendir(manifestsPath.c_str());
    if (!d) {
        logError("tau-store: cannot open manifests/: ", ::strerror(errno));
        return 1;
    }

    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        String name(ent->d_name);
        if (!name.endsWith(".yml") && !name.endsWith(".yaml")) continue;

        String manifestPath = manifestsPath + "/" + name;
        char realBuf[4096] = {};
        if (!::realpath(manifestPath.c_str(), realBuf)) {
            // Broken symlink -> clean up
            ::unlink(manifestPath.c_str());
            continue;
        }
        String realManifestPath(realBuf);

        String currentHash = computeHash(realManifestPath);
        if (currentHash.isEmpty()) { continue; }

        String symlinkHash = name;
        if (symlinkHash.endsWith(".yml")) symlinkHash = symlinkHash.substring(0, symlinkHash.length() - 4);
        else if (symlinkHash.endsWith(".yaml")) symlinkHash = symlinkHash.substring(0, symlinkHash.length() - 5);

        if (symlinkHash != currentHash) {
            ::unlink(manifestPath.c_str());
            String newSymlink = manifestsPath + "/" + currentHash + ".yml";
            ::unlink(newSymlink.c_str());
            ::symlink(realManifestPath.c_str(), newSymlink.c_str());
        }

        activeHashes.push(currentHash);

        // Collect referenced git/local store dependencies
        Array<String> visited;
        ParseError err;
        ParsedManifest parsed = Manifest::load(realManifestPath, visited, err);
        if (err.ok) {
            String dir = realManifestPath;
            long long sl = rfind(dir, '/');
            if (sl >= 0) dir = dir.substring(0, (size_t)sl);
            collectDirectivesHashes(parsed.directives, dir, activeHashes);
        }

        bool previouslyRan = (symlinkHash == currentHash) && isRan(currentHash);

        StoreEntry entry;
        entry.hash         = currentHash;
        entry.manifestPath = realManifestPath;
        entry.storeDir     = Config::storePath() + "/" + currentHash;
        entry.ran          = previouslyRan;
        entries.push(entry);
    }
    ::closedir(d);

    // Global tau.yml
    String global = Config::globalManifestPath();
    if (pathExists(global)) {
        String hash = computeHash(global);
        if (!hash.isEmpty()) {
            activeHashes.push(hash);

            Array<String> visited;
            ParseError err;
            ParsedManifest parsed = Manifest::load(global, visited, err);
            if (err.ok) {
                String dir = global;
                long long sl = rfind(dir, '/');
                if (sl >= 0) dir = dir.substring(0, (size_t)sl);
                collectDirectivesHashes(parsed.directives, dir, activeHashes);
            }

            StoreEntry e;
            e.hash         = hash;
            e.manifestPath = global;
            e.storeDir     = Config::storePath() + "/" + hash;
            e.ran          = isRan(hash);
            entries.push(e);
        }
    }

    bool anyFailed = false;
    for (size_t i = 0; i < entries.length(); ++i) {
        if (!entries[i].ran) {
            mkdirP(entries[i].storeDir);
            if (!runManifest(entries[i])) anyFailed = true;
        } else {
            storeLogCompleted(entries[i].manifestPath);
        }
    }

    garbageCollect(activeHashes);

    String currentPidStr = fs.read(pidPath).trim();
    if (parseLong(currentPidStr) == getpid()) {
        ::unlink(pidPath.c_str());
    }

    return anyFailed ? 1 : 0;
}

} // namespace Tau
