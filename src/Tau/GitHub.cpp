/**
 * @file GitHub.cpp
 * @brief GitHub API: tag/commit resolution, release downloads, disk cache.
 */

#include <Tau/GitHub.hpp>
#include <Tau/Config.hpp>
#include <Tau/Util.hpp>

#include <Encoding/Yaml.hpp>
#include <Encoding/Regex.hpp>
#include <Resource/File.hpp>
#include <Security/Crypto.hpp>
#include <Collection/Tree.hpp>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace Tau {

using namespace Encoding;
using namespace Collection;

// ─── GHRepo::parse ────────────────────────────────────────────────────────────

bool GHRepo::parse(const String &spec, GHRepo &out) {
    String s = spec;
    if (s.startsWith("https://github.com/")) s = s.substring(19);
    if (s.startsWith("http://github.com/"))  s = s.substring(18);
    if (s.startsWith("github.com/"))       s = s.substring(11);
    if (s.endsWith(".git")) s = s.substring(0, s.length() - 4);

    long long hash = s.find("#");
    if (hash >= 0) {
        out.ref = s.substring((size_t)hash + 1);
        s       = s.substring(0, (size_t)hash);
    } else {
        out.ref = "main";
    }

    long long slash = s.find("/");
    if (slash < 0) return false;

    out.owner = s.substring(0, (size_t)slash);
    out.repo  = s.substring((size_t)slash + 1);
    return !out.owner.isEmpty() && !out.repo.isEmpty();
}

// ─── Cache key ────────────────────────────────────────────────────────────────

String GitHub::cacheKey(const String &url) {
    // BLAKE2b-8 of url → 16 hex chars
    String raw = Security::hash(url, 8);
    return hexEncode(raw);
}

// ─── Cache load / save ────────────────────────────────────────────────────────

String GitHub::loadCache(const String &url) {
    String key  = cacheKey(url);
    String path = Config::githubCachePath() + "/" + key + ".json";

    Resource::LinuxFS fs;
    String content = fs.read(path);
    if (content.isEmpty()) return "";

    // Check expiry via a simple prefix scan
    long long exLine = content.find("expires_at: ");
    if (exLine < 0) return "";

    long long end = content.find("\n", (size_t)exLine);
    String exStr = content.substring((size_t)exLine + 12,
                                     end >= 0 ? (size_t)end : content.length());
    long long expiresAt = parseLong(exStr.trim());

    // Get current epoch seconds via a syscall
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    long long now = ts.tv_sec;

    if (now > expiresAt) return "";

    // Extract body (everything after "body:\n")
    long long bodyLine = content.find("body:\n");
    if (bodyLine < 0) return content;

    return content.substring((size_t)bodyLine + 6);
}

void GitHub::saveCache(const String &url, const String &body) {
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    long long now    = ts.tv_sec;
    int spread       = GITHUB_CACHE_MAX_SECS - GITHUB_CACHE_MIN_SECS;
    int ttl          = GITHUB_CACHE_MIN_SECS + (int)(now % spread);
    long long expires = now + ttl;

    String content = "expires_at: " + intStr(expires) + "\n";
    content += "body:\n" + body;

    String key  = cacheKey(url);
    String path = Config::githubCachePath() + "/" + key + ".json";
    Resource::LinuxFS fs;
    fs.write(path, content);
}

// ─── apiGet ───────────────────────────────────────────────────────────────────

String GitHub::apiGet(const String &path) {
    String url = String("https://api.github.com") + path;

    String cached = loadCache(url);
    if (!cached.isEmpty()) return cached;

    // Use curl (available on most Linux systems)
    String cmd = "curl -sf";
    cmd += " -H 'Accept: application/vnd.github+json'";
    cmd += " -H 'User-Agent: tau/" + String(TAU_VERSION) + "'";

    const char *token = ::getenv("GITHUB_TOKEN");
    if (token && token[0]) {
        cmd += String(" -H 'Authorization: Bearer ") + token + "'";
    }
    cmd += " '" + url + "' 2>/dev/null";

    FILE *fp = ::popen(cmd.c_str(), "r");
    if (!fp) return "";

    String result;
    char buf[4096];
    while (::fgets(buf, sizeof(buf), fp)) result += String(buf);
    ::pclose(fp);

    if (!result.isEmpty()) saveCache(url, result);
    return result;
}

// ─── semverCompare ────────────────────────────────────────────────────────────

int GitHub::semverCompare(const String &a, const String &b) {
    String sa = a.startsWith("v") ? a.substring(1) : a;
    String sb = b.startsWith("v") ? b.substring(1) : b;

    auto parsePart = [](const String &s, int &maj, int &min, int &pat) {
        Array<String> parts = s.split(".");
        maj = parts.length() > 0 ? (int)parseLong(parts[0]) : 0;
        min = parts.length() > 1 ? (int)parseLong(parts[1]) : 0;
        pat = parts.length() > 2 ? (int)parseLong(parts[2]) : 0;
    };

    int aMaj, aMin, aPat, bMaj, bMin, bPat;
    parsePart(sa, aMaj, aMin, aPat);
    parsePart(sb, bMaj, bMin, bPat);

    if (aMaj != bMaj) return aMaj < bMaj ? -1 : 1;
    if (aMin != bMin) return aMin < bMin ? -1 : 1;
    if (aPat != bPat) return aPat < bPat ? -1 : 1;
    return 0;
}

// ─── selectTag ────────────────────────────────────────────────────────────────

String GitHub::selectTag(const Array<String> &tags, const String &expr) {
    if (tags.length() == 0) return "";
    if (expr.isEmpty()) return tags[0];

    if (expr.startsWith("reg ")) {
        String pattern = expr.substring(4);
        Encoding::Regex re(pattern);
        for (size_t i = 0; i < tags.length(); ++i) {
            if (re.matchAll(tags[i], 1).length() > 0) return tags[i];
        }
        return "";
    }

    // Operator matching — check operators in order of length
    struct OpEntry { const char *op; bool (*cmp)(int); };
    OpEntry ops[] = {
        { ">=", [](int c){ return c >= 0; } },
        { "<=", [](int c){ return c <= 0; } },
        { ">",  [](int c){ return c >  0; } },
        { "<",  [](int c){ return c <  0; } },
        { nullptr, nullptr }
    };

    for (int i = 0; ops[i].op; ++i) {
        if (expr.startsWith(String(ops[i].op))) {
            String ref = expr.substring(::strlen(ops[i].op));
            for (size_t ti = 0; ti < tags.length(); ++ti) {
                if (ops[i].cmp(semverCompare(tags[ti], ref))) return tags[ti];
            }
            return "";
        }
    }

    // Exact match
    for (size_t i = 0; i < tags.length(); ++i)
        if (tags[i] == expr) return tags[i];

    return "";
}

// ─── listTags ─────────────────────────────────────────────────────────────────

Array<String> GitHub::listTags(const GHRepo &repo) {
    Array<String> tags;
    String json = apiGet(String("/repos/") + repo.owner + "/" + repo.repo +
                         "/tags?per_page=100");
    if (json.isEmpty()) return tags;

    NodeBase root;
    if (!YAML::parse(json, root)) return tags;

    for (size_t i = 0; i < root.size(); ++i) {
        NodeBase *entry = root[i];
        if (!entry) continue;
        String tagName = yamlChildString(entry, "name");
        if (!tagName.isEmpty()) tags.push(tagName);
    }
    return tags;
}

// ─── resolveCommit ────────────────────────────────────────────────────────────

String GitHub::resolveCommit(const GHRepo &repo) {
    String resolvedRef = repo.ref.isEmpty() ? "main" : repo.ref;

    bool isOp = resolvedRef.startsWith(">") || resolvedRef.startsWith("<") ||
                resolvedRef.startsWith("reg ");
    if (isOp) {
        Array<String> tags = listTags(repo);
        resolvedRef = selectTag(tags, resolvedRef);
        if (resolvedRef.isEmpty()) resolvedRef = "main";
    }

    // Try GitHub REST API
    String json = apiGet(String("/repos/") + repo.owner + "/" + repo.repo +
                         "/commits/" + resolvedRef);
    if (!json.isEmpty()) {
        NodeBase root;
        if (YAML::parse(json, root)) {
            String sha = yamlChildString(&root, "sha");
            if (!sha.isEmpty()) return sha;
        }
    }

    // Fallback 1: git ls-remote for resolvedRef (avoids REST API rate limits)
    String url = String("https://github.com/") + repo.owner + "/" + repo.repo + ".git";
    String cmd = String("git ls-remote '") + url + "' '" + resolvedRef + "' 2>/dev/null | awk '{print $1}'";
    FILE *fp = ::popen(cmd.c_str(), "r");
    if (fp) {
        char buf[256] = {0};
        if (::fgets(buf, sizeof(buf), fp)) {
            String sha = String(buf).trim();
            ::pclose(fp);
            if (sha.length() >= 7) return sha;
        } else {
            ::pclose(fp);
        }
    }

    // Fallback 2: git ls-remote for HEAD
    cmd = String("git ls-remote '") + url + "' HEAD 2>/dev/null | awk '{print $1}'";
    fp = ::popen(cmd.c_str(), "r");
    if (fp) {
        char buf[256] = {0};
        if (::fgets(buf, sizeof(buf), fp)) {
            String sha = String(buf).trim();
            ::pclose(fp);
            if (sha.length() >= 7) return sha;
        } else {
            ::pclose(fp);
        }
    }

    return resolvedRef;
}

// ─── resolveRelease ───────────────────────────────────────────────────────────

GHRelease GitHub::resolveRelease(const GHRepo &repo) {
    GHRelease result;

    String tag;
    if (repo.ref.isEmpty()) {
        // Latest release
        String json = apiGet(String("/repos/") + repo.owner + "/" + repo.repo +
                             "/releases/latest");
        NodeBase root;
        if (!json.isEmpty() && YAML::parse(json, root))
            tag = yamlChildString(&root, "tag_name");
    } else {
        Array<String> tags = listTags(repo);
        tag = selectTag(tags, repo.ref);
    }

    if (tag.isEmpty()) return result;
    result.tag = tag;

    String json = apiGet(String("/repos/") + repo.owner + "/" + repo.repo +
                         "/releases/tags/" + tag);
    NodeBase root;
    if (json.isEmpty() || !YAML::parse(json, root)) return result;

    result.commitSHA = yamlChildString(&root, "target_commitish");

    NodeBase *assetsNode = root.get("assets");
    if (assetsNode) {
        for (size_t i = 0; i < assetsNode->size(); ++i) {
            NodeBase *a = (*assetsNode)[i];
            if (!a) continue;
            GHAsset asset;
            asset.name        = yamlChildString(a, "name");
            asset.downloadUrl = yamlChildString(a, "browser_download_url");
            asset.size        = parseLong(yamlChildString(a, "size", "0"));
            if (!asset.name.isEmpty()) result.assets.push(asset);
        }
    }
    return result;
}

// ─── cloneOrUpdate ────────────────────────────────────────────────────────────

bool GitHub::cloneOrUpdate(const String &url, const String &commit,
                             const String &target, bool store) {
    if (store && !url.isEmpty()) {
        String storeDir = Config::storePath() + "/git_" +
                          hexEncode(Security::hash(url + ":" + commit, 8));
        struct stat st;
        bool storeExists = (::stat(storeDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        if (!storeExists) {
            long long lastSlash = rfind(storeDir, '/');
            if (lastSlash > 0) {
                mkdirP(storeDir.substring(0, (size_t)lastSlash));
            }
            String cmd = String("git clone --quiet '") + url + "' '" + storeDir + "' 2>&1";
            if (::system(cmd.c_str()) != 0) {
                logError("tau: git clone failed: ", url);
                return false;
            }
            if (!commit.isEmpty()) {
                String checkoutCmd = String("git -C '") + storeDir + "' fetch --quiet origin 2>&1 && git -C '" + storeDir + "' checkout --quiet '" + commit + "' 2>&1";
                ::system(checkoutCmd.c_str());
            }
        }
        if (!target.isEmpty() && target != storeDir) {
            long long lastSlash = rfind(target, '/');
            if (lastSlash > 0) {
                mkdirP(target.substring(0, (size_t)lastSlash));
            }
            String copyCmd = String("cp -r '") + storeDir + "/.' '" + target + "'";
            return ::system(copyCmd.c_str()) == 0;
        }
        return true;
    }

    struct stat st;
    bool exists = (::stat(target.c_str(), &st) == 0 && S_ISDIR(st.st_mode));

    if (!exists) {
        long long lastSlash = rfind(target, '/');
        if (lastSlash > 0) {
            mkdirP(target.substring(0, (size_t)lastSlash));
        }
        String cmd = String("git clone --quiet '") + url + "' '" + target + "' 2>&1";
        if (::system(cmd.c_str()) != 0) {
            logError("tau: git clone failed: ", url);
            return false;
        }
    }

    if (!commit.isEmpty()) {
        String cmd = String("git -C '") + target +
                     "' fetch --quiet origin 2>&1 && git -C '" + target +
                     "' checkout --quiet '" + commit + "' 2>&1";
        if (::system(cmd.c_str()) != 0) {
            logError("tau: git checkout ", commit, " failed");
            return false;
        }
    }
    return true;
}

// ─── downloadAsset ────────────────────────────────────────────────────────────

bool GitHub::downloadAsset(const GHAsset &asset, const String &target) {
    mkdirP(target);
    String dest = target + "/" + asset.name;
    String cmd  = String("curl -sfL '") + asset.downloadUrl +
                  "' -o '" + dest + "' 2>&1";
    return ::system(cmd.c_str()) == 0;
}

// ─── fetchRepo ────────────────────────────────────────────────────────────────

bool GitHub::fetchRepo(const String &spec, const String &targetDir, bool /*global*/) {
    GHRepo repo;
    if (!GHRepo::parse(spec, repo)) {
        logError("tau: invalid repo spec: ", spec);
        return false;
    }
    String commit = resolveCommit(repo);
    if (commit.isEmpty()) {
        logError("tau: cannot resolve commit for ", spec);
        return false;
    }
    String url = String("https://github.com/") + repo.owner + "/" + repo.repo + ".git";
    return cloneOrUpdate(url, commit, targetDir, false);
}

} // namespace Tau
