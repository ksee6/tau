/**
 * @file Util.hpp
 * @brief Internal utility functions that paper over differences between
 *        tau's usage patterns and the xic API.
 *
 * All functions are in the Tau namespace.
 */

#pragma once

#include <Collection/String.hpp>
#include <Collection/Tree.hpp>
#include <Xi/Log.hpp>

#include <initializer_list>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Tau {

using namespace Collection;
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

inline bool g_debugMode = false;

template <typename... Args>
inline void logInfo(Args &&... args) {
    if (!g_debugMode) return;
    String msg;
    (void)std::initializer_list<int>{
        ((msg += String(args)), 0)...
    };
    Xi::Log::getInstance().info(msg);
}

template <typename... Args>
inline void logWarn(Args &&... args) {
    String msg;
    (void)std::initializer_list<int>{
        ((msg += String(args)), 0)...
    };
    Xi::Log::getInstance().warn(msg);
}

template <typename... Args>
inline void logError(Args &&... args) {
    String msg;
    (void)std::initializer_list<int>{
        ((msg += String(args)), 0)...
    };
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

// ─── YAML node helpers ────────────────────────────────────────────────────────

/// Get a string value from a named child of a NodeBase.
inline String yamlChildString(const Collection::NodeBase *node,
                               const String &key,
                               const String &def = "") {
    if (!node) return def;
    const Collection::NodeBase *child = node->get(key);
    if (!child) return def;
    if (auto *n = dynamic_cast<const Collection::Node<String> *>(child))
        return n->value;
    if (auto *n = dynamic_cast<const Collection::Node<long long> *>(child))
        return intStr(n->value);
    if (auto *n = dynamic_cast<const Collection::Node<int> *>(child))
        return intStr(n->value);
    if (auto *n = dynamic_cast<const Collection::Node<size_t> *>(child))
        return intStr(n->value);
    return def;
}

/// Get a boolean value from a named child of a NodeBase.
inline bool yamlChildBool(const Collection::NodeBase *node,
                           const String &key,
                           bool def = false) {
    String val = yamlChildString(node, key, "");
    if (val.isEmpty()) return def;
    if (val == "true"  || val == "1" || val == "yes" || val == "on")  return true;
    if (val == "false" || val == "0" || val == "no"  || val == "off") return false;
    return def;
}

} // namespace Tau
