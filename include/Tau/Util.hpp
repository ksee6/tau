/**
 * @file Util.hpp
 * @brief Internal utility functions that paper over differences between
 *        tau's usage patterns and the xic API.
 *
 * All functions are in the Tau namespace.
 */

#pragma once

#include <Xi/String.hpp>
#include <Xi/Tree.hpp>
#include <Debug/Log.hpp>

#include <initializer_list>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Tau {

using namespace Xi;

// ─── Integer → String ─────────────────────────────────────────────────────────

/// Convert any integer or pid_t to a decimal String.
template <typename T>
inline String intStr(T n) {
    char buf[32];
    ::snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return String(buf);
}

inline String doubleStr(double n) {
    char buf[32];
    ::snprintf(buf, sizeof(buf), "%.2f", n);
    return String(buf);
}

// ─── rfind (last occurrence of needle) ───────────────────────────────────────

inline long long rfind(const String &s, const String &needle) {
    if (needle.isEmpty() || s.length() < needle.length()) return -1;
    long long last = -1;
    size_t start = 0;
    for (;;) {
        long long pos = s.find(needle, start);
        if (pos < 0) break;
        last  = pos;
        start = (size_t)pos + 1;
    }
    return last;
}

inline long long rfind(const String &s, char c) {
    char buf[2] = { c, 0 };
    return rfind(s, String(buf));
}

// ─── Hex encoding ─────────────────────────────────────────────────────────────

inline String hexEncode(const String &raw) {
    String result;
    for (size_t i = 0; i < raw.length(); ++i) {
        char buf[3];
        ::snprintf(buf, sizeof(buf), "%02x", (unsigned char)raw[i]);
        result += String(buf);
    }
    return result;
}

// ─── Logging helpers (single-string wrappers with concat) ────────────────────

static inline bool g_enableLogging = false;
inline bool g_debugMode = false;

inline void logToFile(const String &msg) {
    (void)msg;
}

template <typename... Args>
inline void logInfo(Args &&... args) {
    if (!g_enableLogging) return;
    String msg;
    (void)std::initializer_list<int>{
        ((msg += String(args)), 0)...
    };
    logToFile("[INFO] " + msg);
    Xi::Log::getInstance().info(msg);
}

template <typename... Args>
inline void logWarn(Args &&... args) {
    if (!g_enableLogging) return;
    String msg;
    (void)std::initializer_list<int>{
        ((msg += String(args)), 0)...
    };
    logToFile("[WARN] " + msg);
    Xi::Log::getInstance().warn(msg);
}

template <typename... Args>
inline void logError(Args &&... args) {
    if (!g_enableLogging) return;
    String msg;
    (void)std::initializer_list<int>{
        ((msg += String(args)), 0)...
    };
    logToFile("[ERROR] " + msg);
    Xi::Log::getInstance().error(msg);
}

// ─── Filesystem helpers ───────────────────────────────────────────────────────

inline bool pathExists(const String &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

inline bool isDir(const String &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool removeDirRecursive(const String &path) {
    if (path.isEmpty() || path == "/" || path == "/root" || path == "/home" || path == "/dev" ||
        path == "/proc" || path == "/sys" || path == "/run" || path == "/etc" || path == "/usr" ||
        path == "/var" || path == "/bin" || path == "/sbin" || path == "/lib" || path == "/lib64") {
        return false;
    }
    ::chmod(path.c_str(), 0777);
    DIR *d = ::opendir(path.c_str());
    if (!d) {
        return (::unlink(path.c_str()) == 0 || errno == ENOENT);
    }
    struct dirent *ent;
    bool ok = true;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))
                continue;
        }
        String child = path + "/" + ent->d_name;
        ::chmod(child.c_str(), 0777);
        struct stat st;
        if (::lstat(child.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                ok = removeDirRecursive(child) && ok;
            } else {
                ::unlink(child.c_str());
            }
        }
    }
    ::closedir(d);
    return (::rmdir(path.c_str()) == 0) && ok;
}


inline bool mkdirP(const String &path, mode_t mode = 0755) {
    if (isDir(path)) return true;
    long long sl = rfind(path, '/');
    if (sl > 0) {
        String parent = path.substring(0, (size_t)sl);
        if (!mkdirP(parent, mode)) return false;
    }
    return (::mkdir(path.c_str(), mode) == 0 || errno == EEXIST);
}

inline bool copyRecursive(const String &src, const String &dst) {
    struct stat st;
    if (::lstat(src.c_str(), &st) != 0) {
        logError("tau: copy: source not found: ", src);
        return false;
    }

    long long sl = rfind(dst, '/');
    if (sl > 0) mkdirP(dst.substring(0, (size_t)sl));

    if (S_ISLNK(st.st_mode)) {
        char linkTarget[4096];
        ssize_t len = ::readlink(src.c_str(), linkTarget, sizeof(linkTarget) - 1);
        if (len > 0) {
            linkTarget[len] = '\0';
            ::unlink(dst.c_str());
            return ::symlink(linkTarget, dst.c_str()) == 0;
        }
        return false;
    }

    if (S_ISREG(st.st_mode)) {
        int inFd = ::open(src.c_str(), O_RDONLY);
        if (inFd < 0) return false;
        int outFd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
        if (outFd < 0) { ::close(inFd); return false; }

        char buf[65536];
        ssize_t n;
        while ((n = ::read(inFd, buf, sizeof(buf))) > 0) {
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = ::write(outFd, buf + written, (size_t)(n - written));
                if (w <= 0) break;
                written += w;
            }
        }
        ::close(inFd);
        ::close(outFd);
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        mkdirP(dst, st.st_mode & 0777);
        DIR *dir = ::opendir(src.c_str());
        if (!dir) return false;
        struct dirent *ent;
        bool ok = true;
        while ((ent = ::readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
                continue;
            ok = copyRecursive(src + "/" + ent->d_name, dst + "/" + ent->d_name) && ok;
        }
        ::closedir(dir);
        return ok;
    }
    return true;
}

// ─── YAML node helpers ────────────────────────────────────────────────────────

using NodeBase = Xi::Node<void>;

/// Get a string value from a named child of a NodeBase.
inline String yamlChildString(const NodeBase *node,
                               const String &key,
                               const String &def = "") {
    if (!node) return def;
    const NodeBase *child = node->get(key);
    if (!child) return def;
    if (auto *n = dynamic_cast<const Node<String> *>(child))
        return n->value;
    if (auto *n = dynamic_cast<const Node<long long> *>(child))
        return intStr(n->value);
    if (auto *n = dynamic_cast<const Node<int> *>(child))
        return intStr(n->value);
    if (auto *n = dynamic_cast<const Node<size_t> *>(child))
        return intStr(n->value);
    return def;
}

/// Get a boolean value from a named child of a NodeBase.
inline bool yamlChildBool(const NodeBase *node,
                           const String &key,
                           bool def = false) {
    String val = yamlChildString(node, key, "");
    if (val.isEmpty()) return def;
    if (val == "true"  || val == "1" || val == "yes" || val == "on")  return true;
    if (val == "false" || val == "0" || val == "no"  || val == "off") return false;
    return def;
}

} // namespace Tau
