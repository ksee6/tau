/**
 * @file Manifest.cpp
 * @brief YAML → typed DirectiveList parser for tau manifests.
 */

#include <Tau/Manifest.hpp>
#include <Tau/Config.hpp>
#include <Tau/Util.hpp>

#include <Data/Yaml.hpp>
#include <Data/Regex.hpp>
#include <Resource/File.hpp>
#include <Xi/Tree.hpp>

#include <cmath>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace Tau {

using namespace Data;
using namespace Xi;

// ─── YAML node helpers ────────────────────────────────────────────────────────

static String nodeStr(const NodeBase *node) {
    if (!node) return "";
    if (auto *n = dynamic_cast<const Node<String> *>(node)) return n->value;
    if (auto *n = dynamic_cast<const Node<long long> *>(node)) return intStr(n->value);
    if (auto *n = dynamic_cast<const Node<int> *>(node)) return intStr(n->value);
    if (auto *n = dynamic_cast<const Node<double> *>(node)) return doubleStr(n->value);
    if (auto *n = dynamic_cast<const Node<bool> *>(node)) return n->value ? "true" : "false";
    return "";
}


static NodeBase *getDictNode(NodeBase *node, const String &key) {
    if (!node) return nullptr;

    if (!node->getName().isEmpty() && node->getName() == key) return node;

    NodeBase *direct = node->get(key);
    if (direct) return direct;

    for (size_t i = 0; i < node->size(); ++i) {
        NodeBase *item = (*node)[i];
        if (!item) continue;
        if (!item->getName().isEmpty() && item->getName() == key) return item;

        NodeBase *subDirect = item->get(key);
        if (subDirect) return subDirect;

        for (size_t k = 0; k < item->size(); ++k) {
            NodeBase *sub = (*item)[k];
            if (sub && !sub->getName().isEmpty() && sub->getName() == key) return sub;
        }
    }

    return nullptr;
}


String Manifest::childString(NodeBase *node, const String &key, const String &def) {
    if (!node) return def;
    NodeBase *child = getDictNode(node, key);
    if (!child) return def;

    String val = nodeStr(child);

    if (key == "command") {
        while (val.startsWith("\"") && val.endsWith("\"") && val.length() >= 2) {
            val = val.substring(1, val.length() - 1);
        }
        return val.isEmpty() ? def : val;
    }

    if (val.isEmpty() && child->size() > 0) {
        for (size_t i = 0; i < child->size(); ++i) {
            NodeBase *c = (*child)[i];
            if (c && (c->getName() == "https" || c->getName() == "http")) {
                String proto = c->getName();
                String rest = nodeStr(c);
                while (rest.startsWith("\"")) rest = rest.substring(1);
                while (rest.endsWith("\"")) rest = rest.substring(0, rest.length() - 1);
                return proto + "://" + rest;
            }
        }
    }

    while (val.startsWith("\"")) val = val.substring(1);
    while (val.endsWith("\"")) val = val.substring(0, val.length() - 1);
    return val.isEmpty() ? def : val;
}


bool Manifest::childBool(NodeBase *node, const String &key, bool def) {
    String val = childString(node, key, "");
    if (val.isEmpty()) return def;
    if (val == "true"  || val == "1" || val == "yes" || val == "on")  return true;
    if (val == "false" || val == "0" || val == "no"  || val == "off") return false;
    return def;
}

// ─── Duration / size parsing ─────────────────────────────────────────────────

double Manifest::parseDuration(const String &s) {
    String str = s.trim();
    if (str.isEmpty()) return 0.0;
    if (str.endsWith("ms")) {
        String num = str.substring(0, str.length() - 2).trim();
        double val = ::strtod(num.c_str(), nullptr);
        return val / 1000.0;
    }
    if (str.endsWith("s")) {
        String num = str.substring(0, str.length() - 1).trim();
        return ::strtod(num.c_str(), nullptr);
    }
    if (str.endsWith("m")) {
        String num = str.substring(0, str.length() - 1).trim();
        double val = ::strtod(num.c_str(), nullptr);
        return val * 60.0;
    }
    if (str.endsWith("h")) {
        String num = str.substring(0, str.length() - 1).trim();
        double val = ::strtod(num.c_str(), nullptr);
        return val * 3600.0;
    }
    return ::strtod(str.c_str(), nullptr);
}

size_t Manifest::parseSize(const String &s) {
    if (s.isEmpty()) return 0;
    char suffix = s[s.length() - 1];
    String numStr = s;
    size_t mult = 1;
    if (suffix == 'K' || suffix == 'k') { numStr = s.substring(0, s.length()-1); mult = 1024ULL; }
    else if (suffix == 'M' || suffix == 'm') { numStr = s.substring(0, s.length()-1); mult = 1024ULL*1024; }
    else if (suffix == 'G' || suffix == 'g') { numStr = s.substring(0, s.length()-1); mult = 1024ULL*1024*1024; }
    return (size_t)parseLong(numStr) * mult;
}

// ─── Var expression parsing ───────────────────────────────────────────────────

VarOp Manifest::parseVarOp(const String &expr, String &key, String &value) {
    auto tryOp = [&](const String &op) -> bool {
        long long idx = expr.find(op);
        if (idx < 0) return false;
        key   = expr.substring(0, (size_t)idx).trim();
        value = expr.substring((size_t)idx + op.length()).trim();
        return true;
    };

    if (tryOp(" includes ")) return VarOp::CheckIncludes;
    if (tryOp(" reg "))      return VarOp::CheckRegex;
    if (tryOp(">="))         return VarOp::CheckGTE;
    if (tryOp("<="))         return VarOp::CheckLTE;
    if (tryOp("=="))         return VarOp::CheckEquals;
    if (tryOp(">"))          return VarOp::CheckGT;
    if (tryOp("<"))          return VarOp::CheckLT;
    if (tryOp("="))          return VarOp::Assign;

    key   = expr.trim();
    value = "";
    return VarOp::Assign;
}

[[maybe_unused]] static NodeBase *findNamedNode(NodeBase *node) {
    if (!node) return nullptr;
    if (!node->getName().isEmpty()) return node;

    for (size_t i = 0; i < node->size(); ++i) {
        NodeBase *child = (*node)[i];
        if (child && !child->getName().isEmpty()) return child;
    }
    return nullptr;
}

// ─── Single YAML node → Directive ────────────────────────────────────────────

Directive *Manifest::nodeToDirective(NodeBase *node,
                                     const String &basePath,
                                     Array<String> &visited,
                                     ParseError &err) {
    if (!node) return nullptr;

    NodeBase *origNode = node;
    String key = node->getName();
    if (key.isEmpty() && node->size() > 0) {
        for (size_t i = 0; i < node->size(); ++i) {
            NodeBase *child = (*node)[i];
            if (child && !child->getName().isEmpty()) {
                key = child->getName();
                node = child;
                break;
            }
        }
    }
    if (key.isEmpty()) {
        return nullptr;
    }

    auto getProp = [&](const String &propKey, const String &def = "") -> String {
        String v = childString(node, propKey, "");
        if (v.isEmpty() && origNode && origNode != node) {
            v = childString(origNode, propKey, "");
        }
        return v.isEmpty() ? def : v;
    };

    auto getPropBool = [&](const String &propKey, bool def) -> bool {
        String v = getProp(propKey, "");
        if (v.isEmpty()) return def;
        if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
        if (v == "false" || v == "0" || v == "no" || v == "off") return false;
        return def;
    };

    auto getPropNode = [&](const String &propKey) -> NodeBase* {
        NodeBase *n = getDictNode(node, propKey);
        if (!n && origNode && origNode != node) {
            n = getDictNode(origNode, propKey);
        }
        return n;
    };

    // ── wait ──────────────────────────────────────────────────────────────────
    if (key == "wait") {
        auto *d = new DWait();
        d->isGlobal = getPropBool("global", false);
        NodeBase *tn = getPropNode("targets");
        if (tn) {
            for (size_t i = 0; i < tn->size(); ++i) {
                String s = nodeStr((*tn)[i]).trim();
                if (!s.isEmpty()) d->targets.push(s);
            }
        } else if (node->size() > 0 && (nodeStr(node).isEmpty() || nodeStr(node) == "[]")) {
            for (size_t i = 0; i < node->size(); ++i) {
                String s = nodeStr((*node)[i]).trim();
                while (s.startsWith("[") && s.endsWith("]") && s.length() >= 2) {
                    s = s.substring(1, s.length() - 1).trim();
                }
                if (!s.isEmpty() && s != "true" && s != "false" && s != "[]") {
                    Array<String> parts = s.split(",");
                    for (size_t pi = 0; pi < parts.length(); ++pi) {
                        String pt = parts[pi].trim();
                        if (!pt.isEmpty()) d->targets.push(pt);
                    }
                }
            }
        } else {
            String val = nodeStr(node).trim();
            while (val.startsWith("[") && val.endsWith("]") && val.length() >= 2) {
                val = val.substring(1, val.length() - 1).trim();
            }
            if (val.find(",") >= 0) {
                Array<String> parts = val.split(",");
                for (size_t pi = 0; pi < parts.length(); ++pi) {
                    String pt = parts[pi].trim();
                    if (!pt.isEmpty()) d->targets.push(pt);
                }
            } else if (!val.isEmpty()) {
                // Check if it is a pure duration (e.g. "1.5s", "200ms", "5m")
                bool isDur = false;
                if (val.endsWith("ms") || val.endsWith("s") || val.endsWith("m") || val.endsWith("h")) {
                    char *endp = nullptr;
                    (void)::strtod(val.c_str(), &endp);
                    if (endp && (::strcmp(endp, "ms") == 0 || ::strcmp(endp, "s") == 0 || ::strcmp(endp, "m") == 0 || ::strcmp(endp, "h") == 0)) {
                        isDur = true;
                    }
                }
                if (isDur) {
                    d->seconds = parseDuration(val);
                } else {
                    d->targets.push(val);
                }
            }
        }
        return d;
    }

    // ── trigger ───────────────────────────────────────────────────────────────
    if (key == "trigger") {
        auto *d = new DTrigger();
        d->name = nodeStr(node).trim();
        if (d->name.isEmpty()) d->name = getProp("name").trim();
        return d;
    }

    // ── nowait ────────────────────────────────────────────────────────────────
    if (key == "nowait") {
        auto *d = new DNoWait();
        NodeBase *target = getPropNode("entry");
        if (!target) target = node;
        d->children = nodeListToDirectives(target, basePath, visited, err);
        return d;
    }

    // ── waitall ───────────────────────────────────────────────────────────────
    if (key == "waitall") {
        auto *d = new DWaitAll();
        NodeBase *target = getPropNode("entry");
        if (!target) target = node;
        d->children = nodeListToDirectives(target, basePath, visited, err);
        return d;
    }

    // ── logic blocks ──────────────────────────────────────────────────────────
    auto parseBlock = [&](DLogicBlock *block) -> Directive * {
        NodeBase *target = getPropNode("entry");
        if (!target) target = node;
        block->children = nodeListToDirectives(target, basePath, visited, err);
        return block;
    };


    // ── waitexit ──────────────────────────────────────────────────────────────
    if (key == "waitexit") {
        auto *d = new DWaitExit();
        NodeBase *target = getPropNode("entry");
        if (!target) target = node;
        d->children = nodeListToDirectives(target, basePath, visited, err);
        return d;
    }

    // ── retry ─────────────────────────────────────────────────────────────────
    if (key == "retry") {
        auto *d = new DRetry();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            long long t = parseLong(scalar);
            if (t >= 0) d->times = (int)t;
        }
        if (d->times == 0) {
            String tStr = getProp("times");
            if (!tStr.isEmpty()) d->times = (int)parseLong(tStr);
        }
        String bStr = getProp("backoff", "1s");
        d->backoff = parseDuration(bStr);
        if (d->backoff <= 0) d->backoff = 1.0;

        NodeBase *chNode = getPropNode("children");
        if (!chNode) chNode = getPropNode("entry");
        if (!chNode) chNode = node;
        d->children = nodeListToDirectives(chNode, basePath, visited, err);
        return d;
    }

    // ── dotenv ────────────────────────────────────────────────────────────────
    if (key == "dotenv" || key == "env_file" || key == "envfile") {
        auto *d = new DDotenv();
        d->path = nodeStr(node).trim();
        if (d->path.isEmpty()) d->path = getProp("path").trim();
        return d;
    }

    // ── spawn ─────────────────────────────────────────────────────────────────
    if (key == "spawn") {
        auto *d = new DSpawn();
        d->command = nodeStr(node).trim();
        if (d->command.isEmpty() || (origNode && getDictNode(origNode, "command") != nullptr)) {
            String nestedCmd = getProp("command").trim();
            if (!nestedCmd.isEmpty()) d->command = nestedCmd;
        }
        while (d->command.startsWith("\"") && d->command.endsWith("\"") && d->command.length() >= 2) {
            d->command = d->command.substring(1, d->command.length() - 1);
        }
        d->waitForExit = getPropBool("wait", true);
        d->name        = getProp("name");
        String pidStr  = getProp("pid");
        if (!pidStr.isEmpty()) d->pid = (pid_t)parseLong(pidStr);
        String ecStr   = getProp("exitcode");
        if (ecStr.isEmpty()) ecStr = getProp("exit_code");
        if (!ecStr.isEmpty()) d->exitCode = (int)parseLong(ecStr);
        return d;
    }


    // ── memory ────────────────────────────────────────────────────────────────
    if (key == "memory") {
        auto *d = new DMemory();
        if (node->size() > 0 && getDictNode(node, "limit") != nullptr) {
            d->raw = childString(node, "limit");
            d->rawMax = childString(node, "max");
            if (d->rawMax.isEmpty()) d->rawMax = childString(node, "memory_max");
        } else if (node->size() > 0 && getDictNode(node, "max") != nullptr) {
            d->rawMax = childString(node, "max");
            d->raw = childString(node, "memory");
        } else {
            d->raw = nodeStr(node);
            d->rawMax = childString(node, "memory_max");
            if (d->rawMax.isEmpty()) d->rawMax = childString(node, "max");
        }
        if (!d->raw.isEmpty())    d->bytes = parseSize(d->raw);
        if (!d->rawMax.isEmpty()) d->bytesMax = parseSize(d->rawMax);
        return d;
    }

    if (key == "memory_max" || key == "memory-max") {
        auto *d = new DMemory();
        d->rawMax = nodeStr(node);
        d->bytesMax = parseSize(d->rawMax);
        return d;
    }

    if (key == "cpuset") {
        auto *d = new DCPUSet();
        d->cpus = nodeStr(node);
        return d;
    }

    if (key == "cpu") {
        auto *d = new DCPU();
        if (node->size() > 0 && getDictNode(node, "max") != nullptr) {
            String qStr = childString(node, "quota");
            if (qStr.isEmpty()) qStr = childString(node, "cpu");
            if (!qStr.isEmpty()) d->quota = parseLong(qStr);
            String mStr = childString(node, "max");
            if (mStr.isEmpty()) mStr = childString(node, "cpu_max");
            if (!mStr.isEmpty()) d->quotaMax = parseLong(mStr);
        } else {
            d->quota = parseLong(nodeStr(node));
            String mStr = childString(node, "cpu_max");
            if (mStr.isEmpty()) mStr = childString(node, "max");
            if (!mStr.isEmpty()) d->quotaMax = parseLong(mStr);
        }
        return d;
    }

    if (key == "cpu_max" || key == "cpu-max") {
        auto *d = new DCPU();
        d->quotaMax = parseLong(nodeStr(node));
        return d;
    }

    if (key == "autofreeze" || key == "auto_freeze" || key == "scale_to_zero") {
        auto *d = new DAutofreeze();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false" && scalar != "on" && scalar != "off") {
            d->rawTimer = scalar;
        }
        String wolStr = getProp("wol");
        if (!wolStr.isEmpty()) d->wol = (wolStr != "false" && wolStr != "0" && wolStr != "off");
        String cpuStr = getProp("cpu_threshold");
        if (cpuStr.isEmpty()) cpuStr = getProp("cpu");
        if (!cpuStr.isEmpty()) {
            String ctrim = cpuStr.trim();
            if (ctrim.endsWith("%")) ctrim = ctrim.substring(0, ctrim.length() - 1).trim();
            d->cpuThreshold = (double)parseLong(ctrim);
        }
        String timerStr = getProp("timer");
        if (timerStr.isEmpty()) timerStr = getProp("idle");
        if (timerStr.isEmpty()) timerStr = getProp("timeout");
        if (timerStr.isEmpty() && !d->rawTimer.isEmpty()) timerStr = d->rawTimer;
        if (!timerStr.isEmpty()) {
            d->rawTimer = timerStr;
            String t = timerStr.trim();
            if (t.endsWith("ms")) {
                d->timerSeconds = (double)parseLong(t.substring(0, t.length() - 2)) / 1000.0;
            } else if (t.endsWith("s")) {
                d->timerSeconds = (double)parseLong(t.substring(0, t.length() - 1));
            } else if (t.endsWith("m")) {
                d->timerSeconds = (double)parseLong(t.substring(0, t.length() - 1)) * 60.0;
            } else if (t.endsWith("h")) {
                d->timerSeconds = (double)parseLong(t.substring(0, t.length() - 1)) * 3600.0;
            } else {
                long long v = parseLong(t);
                if (v > 0) d->timerSeconds = (double)v;
            }
        }
        String enStr = getProp("enabled");
        if (!enStr.isEmpty()) d->enabled = (enStr != "false" && enStr != "0" && enStr != "off");
        else if (scalar == "false" || scalar == "0" || scalar == "off") d->enabled = false;
        return d;
    }

    // ── chroot ────────────────────────────────────────────────────────────────
    if (key == "chroot") {
        auto *d = new DChroot();
        d->path = nodeStr(node).trim();
        if (d->path.isEmpty()) d->path = getProp("path").trim();
        return d;
    }

    // ── isolate ───────────────────────────────────────────────────────────────
    if (key == "isolate") {
        auto *d = new DIsolate();
        String val = nodeStr(node).trim();
        if (val == "false" || val == "0" || val == "off" || val == "no") {
            d->enable = false;
        } else {
            d->enable = true;
        }
        return d;
    }

    // ── path restrictions (noread, nowrite, noexecute, nomount, nowatch) ─────
    auto parsePathList = [&](DPathRestriction *d) -> Directive * {
        Array<String> items;
        if (node->size() > 0 && (nodeStr(node).isEmpty() || nodeStr(node) == "[]")) {
            for (size_t i = 0; i < node->size(); ++i) {
                String s = nodeStr((*node)[i]).trim();
                if (!s.isEmpty()) items.push(s);
            }
        } else {
            String s = nodeStr(node).trim();
            if (!s.isEmpty()) items.push(s);
        }
        d->paths = items;
        return d;
    };

    if (key == "noread")    return parsePathList(new DNoRead());
    if (key == "nowrite")   return parsePathList(new DNoWrite());
    if (key == "noexecute") return parsePathList(new DNoExecute());
    if (key == "nomount")   return parsePathList(new DNoMount());
    if (key == "nowatch")   return parsePathList(new DNoWatch());

    if (key == "veth") {
        auto *d = new DVeth();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            long long colon = scalar.find(":");
            if (colon >= 0) {
                d->source = scalar.substring(0, (size_t)colon).trim();
                d->target = scalar.substring((size_t)colon + 1).trim();
            } else {
                d->source = scalar;
            }
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        if (d->source.isEmpty()) d->source = getProp("host");
        if (d->source.isEmpty()) d->source = getProp("name");

        if (d->target.isEmpty()) d->target = getProp("target");
        if (d->target.isEmpty()) d->target = getProp("peer");

        d->sip = getProp("sip");
        if (d->sip.isEmpty()) d->sip = getProp("source-ip");
        if (d->sip.isEmpty()) d->sip = getProp("source_ip");
        if (d->sip.isEmpty()) d->sip = getProp("host-ip");
        if (d->sip.isEmpty()) d->sip = getProp("host_ip");

        d->ip = getProp("ip");
        if (d->ip.isEmpty()) d->ip = getProp("peer-ip");
        if (d->ip.isEmpty()) d->ip = getProp("peer_ip");
        if (d->ip.isEmpty()) d->ip = getProp("target-ip");
        if (d->ip.isEmpty()) d->ip = getProp("target_ip");

        d->hostName = d->source;
        d->peerName = d->target;
        return d;
    }

    if (key == "ip") {
        auto *d = new DIP();
        d->command = nodeStr(node);
        return d;
    }

    if (key == "newnet" || key == "nolinks") {
        auto *d = new DNewNet();
        if (node->size() > 0) {
            for (size_t i = 0; i < node->size(); ++i) {
                String s = nodeStr((*node)[i]).trim();
                while (s.startsWith("[") && s.endsWith("]") && s.length() >= 2) {
                    s = s.substring(1, s.length() - 1).trim();
                }
                if (!s.isEmpty() && s != "true" && s != "false" && s != "[]") {
                    d->allowList.push(s);
                }
            }
        }
        if (d->allowList.length() == 0) {
            String val = nodeStr(node).trim();
            while (val.startsWith("[") && val.endsWith("]") && val.length() >= 2) {
                val = val.substring(1, val.length() - 1).trim();
            }
            if (!val.isEmpty() && val != "true" && val != "false" && val != "[]" && val != "{}") {
                Array<String> parts = val.split(",");
                for (size_t i = 0; i < parts.length(); ++i) {
                    String t = parts[i].trim();
                    if (!t.isEmpty() && t != "true" && t != "false" && t != "[]") d->allowList.push(t);
                }
            }
        }
        return d;
    }

    if (key == "route") {
        auto *d = new DRoute();
        String dest = nodeStr(node).trim();
        if (dest.isEmpty() || dest.startsWith("{") || dest.startsWith("[")) {
            dest = childString(node, "route", "default");
            if (dest.isEmpty() || dest == "true") dest = childString(node, "destination", "default");
        }
        d->destination = dest;

        NodeBase *viaNode = getDictNode(node, "via");
        if (viaNode) {
            if (viaNode->size() > 0 && (nodeStr(viaNode).isEmpty() || nodeStr(viaNode).startsWith("[") || nodeStr(viaNode).startsWith("{"))) {
                for (size_t i = 0; i < viaNode->size(); ++i) {
                    NodeBase *vn = (*viaNode)[i];
                    if (!vn) continue;
                    RouteHop hop;
                    if (vn->size() == 0 || getDictNode(vn, "ip") == nullptr) {
                        String s = nodeStr(vn).trim();
                        if (!s.isEmpty()) {
                            hop.ip = s;
                            d->hops.push(hop);
                        }
                    } else {
                        hop.ip = childString(vn, "ip");
                        hop.link = childString(vn, "link");
                        if (hop.link.isEmpty()) hop.link = childString(vn, "dev");
                        String wStr = childString(vn, "weight", "1");
                        hop.weight = !wStr.isEmpty() ? (int)parseLong(wStr) : 1;
                        d->hops.push(hop);
                    }
                }
            } else {
                String viaStr = nodeStr(viaNode).trim();
                if (!viaStr.isEmpty()) {
                    RouteHop hop;
                    hop.ip = viaStr;
                    d->hops.push(hop);
                }
            }
        }
        return d;
    }

    if (key == "forward") {
        auto *d = new DForward();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->port = scalar;
        }
        if (d->port.isEmpty()) d->port = getProp("port");
        d->target     = getProp("target");
        d->source     = getProp("source");
        d->sourcePort = getProp("source-port");
        String sip    = getProp("source-ip");
        if (sip.isEmpty()) sip = getProp("sip");
        if (!sip.isEmpty()) d->sourceIP = sip;
        d->ip         = getProp("ip");
        String proto  = getProp("protocol");
        if (!proto.isEmpty()) d->protocol = proto;
        return d;
    }

    if (key == "macvlan") {
        auto *d = new DMacvlan();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->name = scalar;
        }
        d->parent = getProp("parent");
        if (d->name.isEmpty()) d->name = getProp("name");
        String m  = getProp("mode");
        if (!m.isEmpty()) d->mode = m;
        d->mac    = getProp("mac");
        return d;
    }

    if (key == "ipvlan") {
        auto *d = new DIPVlan();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->name = scalar;
        }
        d->parent = getProp("parent");
        if (d->name.isEmpty()) d->name = getProp("name");
        String m  = getProp("mode");
        if (!m.isEmpty()) d->mode = m;
        return d;
    }

    if (key == "bridge") {
        auto *d = new DBridge();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->source = scalar;
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        if (d->source.isEmpty()) d->source = getProp("name");
        d->target  = getProp("target");
        if (d->target.isEmpty()) d->target = getProp("attach");
        d->address = getProp("address");
        if (d->address.isEmpty()) d->address = getProp("ip");
        d->name    = d->source;
        d->attach  = d->target;
        return d;
    }

    if (key == "mount") {
        auto *d = new DMount();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            long long colon = scalar.find(":");
            if (colon >= 0) {
                d->source = scalar.substring(0, (size_t)colon).trim();
                d->target = scalar.substring((size_t)colon + 1).trim();
            } else {
                d->source = scalar;
            }
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        d->work   = getProp("work");
        if (d->target.isEmpty()) d->target = getProp("target");
        d->read   = getPropBool("read",  true);
        d->write  = getPropBool("write", true);
        return d;
    }

    if (key == "copy") {
        auto *d = new DCopy();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            long long arrow = scalar.find("->");
            long long colon = scalar.find(":");
            if (arrow >= 0) {
                d->source = scalar.substring(0, (size_t)arrow).trim();
                d->target = scalar.substring((size_t)arrow + 2).trim();
            } else if (colon >= 0) {
                d->source = scalar.substring(0, (size_t)colon).trim();
                d->target = scalar.substring((size_t)colon + 1).trim();
            } else {
                d->source = scalar;
            }
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        if (d->target.isEmpty()) d->target = getProp("target");
        return d;
    }

    if (key == "unlink") {
        auto *d = new DUnlink();
        d->path = nodeStr(node).trim();
        if (d->path.isEmpty()) d->path = getProp("path").trim();
        return d;
    }

    if (key == "mkdir") {
        auto *d = new DMkdir();
        d->path = nodeStr(node).trim();
        if (d->path.isEmpty()) d->path = getProp("path").trim();
        return d;
    }

    if (key == "symlink") {
        auto *d = new DSymlink();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            long long arrow = scalar.find("->");
            long long colon = scalar.find(":");
            if (arrow >= 0) {
                d->source = scalar.substring(0, (size_t)arrow).trim();
                d->target = scalar.substring((size_t)arrow + 2).trim();
            } else if (colon >= 0) {
                d->source = scalar.substring(0, (size_t)colon).trim();
                d->target = scalar.substring((size_t)colon + 1).trim();
            } else {
                d->source = scalar;
            }
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        if (d->target.isEmpty()) d->target = getProp("target");
        return d;
    }

    if (key == "image") {
        auto *d = new DImage();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->source = scalar;
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        if (d->source.isEmpty()) d->source = getProp("path");
        d->path   = d->source;
        d->size   = getProp("size", "10G");
        d->sizeMax = getProp("size_max");
        if (d->sizeMax.isEmpty()) d->sizeMax = getProp("max_size");
        d->type   = getProp("type", "ext4");
        d->table  = getProp("table");
        if (d->table.isEmpty() && (d->type == "gpt" || d->type == "mbr" || d->type == "dos")) {
            d->table = d->type;
        }
        d->format = getProp("format", "raw");
        d->label  = getProp("label");
        d->create = getPropBool("create", true);
        d->resize = getPropBool("resize", true);
        d->fsck   = getPropBool("fsck",   true);
        d->upper  = getProp("upper");
        d->work   = getProp("work");
        d->lower  = getProp("lower");
        d->target = getProp("target");
        d->read   = getPropBool("read",  true);
        d->write  = getPropBool("write", true);

        NodeBase *partNode = getPropNode("partitions");
        if (partNode) {
            for (size_t i = 0; i < partNode->size(); ++i) {
                auto *pn = (*partNode)[i];
                if (!pn) continue;
                ImagePartition p;
                p.name   = childString(pn, "name");
                p.uuid   = childString(pn, "uuid");
                p.size   = childString(pn, "size");
                p.offset = childString(pn, "offset");
                p.type   = childString(pn, "type");
                if (p.type.isEmpty()) p.type = childString(pn, "fstype", "ext4");
                p.fstype = p.type;
                p.label  = childString(pn, "label");
                p.flags  = childString(pn, "flags");
                String numStr = childString(pn, "number");
                p.number = !numStr.isEmpty() ? (int)::atoi(numStr.c_str()) : (int)(i + 1);
                p.upper  = childString(pn, "upper");
                if (p.upper.isEmpty()) p.upper = childString(pn, "source", "@/");
                p.source = p.upper;
                p.work   = childString(pn, "work");
                p.lower  = childString(pn, "lower");
                p.target = childString(pn, "target");
                p.read   = childBool(pn, "read",  true);
                p.write  = childBool(pn, "write", true);
                d->partitions.push(p);
            }
        }
        return d;
    }

    if (key == "docker") {
        auto *d = new DDocker();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->image = scalar;
        }
        if (d->image.isEmpty()) d->image = getProp("image");
        if (d->image.isEmpty()) d->image = getProp("source");

        // Handle unquoted image:tag where tag was parsed as child node (e.g. :latest)
        for (size_t di = 0; di < node->size(); ++di) {
            NodeBase *ch = (*node)[di];
            if (ch && ch->getName().startsWith(":")) {
                if (d->image.find(":") < 0) {
                    d->image += ch->getName();
                }
                if (d->target.isEmpty() || d->target == "true") {
                    d->target = childString(ch, "target");
                    if (d->target.isEmpty() || d->target == "true") d->target = nodeStr(ch);
                }
            }
        }

        d->download = getProp("download");
        d->lower    = getProp("lower");
        d->target   = getProp("target");
        d->source   = getProp("source");
        if (d->source == d->image) d->source = "";
        if (d->source.isEmpty()) d->source = getProp("upper");
        d->work     = getProp("work");
        d->read     = getPropBool("read", true);
        d->write    = getPropBool("write", true);
        return d;
    }

    if (key == "vm") {
        auto *d = new DVM();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            VMDrive drv;
            drv.source = scalar;
            drv.format = "auto";
            d->drives.push(drv);
        }
        d->kernel      = getProp("kernel");
        d->initrd      = getProp("initrd");
        d->cmdline     = getProp("cmdline");
        String hyp     = getProp("hypervisor");
        if (!hyp.isEmpty()) d->hypervisor = hyp;
        String acc     = getProp("accel");
        if (!acc.isEmpty()) d->accel = acc;
        d->waitForExit = getPropBool("wait", true);
        d->name        = getProp("name");

        NodeBase *drivesNode = getPropNode("drives");
        if (!drivesNode) drivesNode = getPropNode("drive");

        if (drivesNode) {
            for (size_t i = 0; i < drivesNode->size(); ++i) {
                NodeBase *dn = (*drivesNode)[i];
                if (!dn) continue;
                VMDrive drv;
                if (dn->size() == 0 || getDictNode(dn, "source") == nullptr) {
                    drv.source = nodeStr(dn);
                    drv.format = "auto";
                    drv.read   = true;
                    drv.write  = true;
                } else {
                    drv.source = childString(dn, "source");
                    if (drv.source.isEmpty()) drv.source = childString(dn, "path");
                    drv.format = childString(dn, "format", "auto");
                    drv.read   = childBool(dn, "read", true);
                    drv.write  = childBool(dn, "write", true);
                }
                if (!drv.source.isEmpty()) d->drives.push(drv);
            }
        } else {
            String singleDrive = getProp("drive");
            if (!singleDrive.isEmpty()) {
                VMDrive drv;
                drv.source = singleDrive;
                drv.format = getProp("format", "auto");
                drv.read   = getPropBool("read", true);
                drv.write  = getPropBool("write", true);
                d->drives.push(drv);
            }
        }
        return d;
    }

    if (key == "local") {
        auto *d = new DLocal();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->source = scalar;
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        if (d->source.isEmpty()) d->source = getProp("path");
        d->target   = getProp("target");
        d->store    = getPropBool("store", true);
        d->download = getPropBool("download", false);
        return d;
    }

    if (key == "git" || key == "github") {
        auto *d = new DGit();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->source = scalar;
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        if (d->source.isEmpty()) d->source = getProp("repo");
        if (d->source.isEmpty()) d->source = getProp("url");
        d->branch   = getProp("branch", "main");
        if (d->branch == "main" && !getProp("tag").isEmpty()) d->branch = getProp("tag");
        d->target   = getProp("target");
        d->store    = getPropBool("store", true);
        d->download = getPropBool("download", false);
        return d;
    }

    if (key == "ghrelease") {
        auto *d = new DGHRelease();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->source = scalar;
        }
        if (d->source.isEmpty()) d->source = getProp("source");
        if (d->source.isEmpty()) d->source = getProp("repo");
        d->branch = getProp("branch");
        if (d->branch.isEmpty()) d->branch = getProp("tag");
        d->name   = getProp("name");
        d->target = getProp("target");
        return d;
    }

    if (key == "manifest") {
        String incPath = nodeStr(node);
        if (incPath[0] != '/') incPath = basePath + "/" + incPath;

        for (size_t i = 0; i < visited.length(); ++i) {
            if (visited[i] == incPath) {
                err.message = String("Cyclic manifest include: ") + incPath;
                err.ok = false;
                return nullptr;
            }
        }

        ParsedManifest inc = Manifest::load(incPath, visited, err);
        if (!err.ok) return nullptr;

        auto *andBlock = new DAnd();
        for (auto *child : inc.directives) andBlock->children.push(child);
        inc.directives.clear();
        return andBlock;
    }

    if (key == "and")  return parseBlock(new DAnd());
    if (key == "or")   return parseBlock(new DOr());
    if (key == "nand") return parseBlock(new DNand());
    if (key == "nor")  return parseBlock(new DNor());
    if (key == "xor")  return parseBlock(new DXor());

    if (key == "or")   return parseBlock(new DOr());
    if (key == "and")  return parseBlock(new DAnd());
    if (key == "nand") return parseBlock(new DNand());
    if (key == "nor")  return parseBlock(new DNor());
    if (key == "xor")  return parseBlock(new DXor());

    // ── variables ─────────────────────────────────────────────────────────────
    auto parseVar = [&](DVarBase *d) -> Directive * {
        if (node->size() > 0) {
            for (size_t i = 0; i < node->size(); ++i) {
                NodeBase *c = (*node)[i];
                if (c && !c->getName().isEmpty()) {
                    String fullExpr = c->getName() + " " + nodeStr(c);
                    String k, v;
                    VarOp op = parseVarOp(fullExpr, k, v);
                    if (op != VarOp::Assign || fullExpr.find("=") >= 0) {
                        d->key = k;
                        d->value = v;
                        d->op = op;
                    } else {
                        d->key = c->getName();
                        d->value = nodeStr(c);
                        d->op = VarOp::Assign;
                    }
                    return d;
                }
            }
        }
        d->op = parseVarOp(nodeStr(node), d->key, d->value);
        return d;
    };

    if (key == "var")     return parseVar(new DVar());
    if (key == "regvar")  return parseVar(new DRegVar());
    if (key == "rallvar") return parseVar(new DRallVar());

    if (key == "newvar") {
        auto *d = new DNewVar();
        NodeBase *target = getDictNode(node, "newvar");
        if (!target) target = node;
        for (size_t i = 0; i < target->size(); ++i) {
            String s = nodeStr((*target)[i]);
            if (!s.isEmpty()) d->patterns.push(s);
        }
        if (d->patterns.length() == 0) {
            String s = nodeStr(target);
            if (!s.isEmpty()) d->patterns.push(s);
        }
        return d;
    }

    if (key == "rmvar") {
        auto *d = new DRmVar();
        NodeBase *target = getDictNode(node, "rmvar");
        if (!target) target = node;
        for (size_t i = 0; i < target->size(); ++i) {
            String s = nodeStr((*target)[i]);
            if (!s.isEmpty()) d->patterns.push(s);
        }
        if (d->patterns.length() == 0) {
            String s = nodeStr(target);
            if (!s.isEmpty()) d->patterns.push(s);
        }
        return d;
    }



    // ── control ───────────────────────────────────────────────────────────────
    if (key == "fail")  { auto *d = new DFail();  d->message = nodeStr(node); return d; }
    if (key == "throw") { auto *d = new DThrow(); d->message = nodeStr(node); return d; }

    // ── metadata ──────────────────────────────────────────────────────────────
    if (key == "name")        { auto *d = new DName();        d->value = nodeStr(node); return d; }
    if (key == "description") { auto *d = new DDescription(); d->value = nodeStr(node); return d; }
    if (key == "version")     { auto *d = new DVersion();     d->value = nodeStr(node); return d; }
    if (key == "website")     { auto *d = new DWebsite();     d->value = nodeStr(node); return d; }
    if (key == "issues")      { auto *d = new DIssues();      d->value = nodeStr(node); return d; }
    if (key == "license")     { auto *d = new DLicense();     d->value = nodeStr(node); return d; }
    if (key == "stars")       { auto *d = new DStars();       d->value = nodeStr(node); return d; }
    if (key == "languages")   { auto *d = new DLanguages();   d->value = nodeStr(node); return d; }

    // ── slot ──────────────────────────────────────────────────────────────────
    if (key == "slot") return new DSlot();

    // ── eslot ─────────────────────────────────────────────────────────────────
    if (key == "eslot") {
        auto *d = new DESlot();
        d->name = nodeStr(node).trim();
        if (d->name.isEmpty()) d->name = childString(node, "name").trim();
        if (d->name.isEmpty()) d->name = childString(origNode, "eslot").trim();
        d->slot = childBool(node, "slot", false);
        if (!d->slot) d->slot = childBool(origNode, "slot", false);

        NodeBase *entryNode = node->get("entry");
        if (!entryNode && getDictNode(node, "entry")) entryNode = getDictNode(node, "entry");
        if (!entryNode && origNode->get("entry")) entryNode = origNode->get("entry");
        if (!entryNode && getDictNode(origNode, "entry")) entryNode = getDictNode(origNode, "entry");
        if (entryNode) d->entry = nodeListToDirectives(entryNode, basePath, visited, err);

        NodeBase *escNode = node->get("escape");
        if (!escNode && getDictNode(node, "escape")) escNode = getDictNode(node, "escape");
        if (!escNode && origNode->get("escape")) escNode = origNode->get("escape");
        if (!escNode && getDictNode(origNode, "escape")) escNode = getDictNode(origNode, "escape");
        if (escNode) d->escape = nodeListToDirectives(escNode, basePath, visited, err);

        NodeBase *injNode = node->get("inject");
        if (!injNode && getDictNode(node, "inject")) injNode = getDictNode(node, "inject");
        if (!injNode && origNode->get("inject")) injNode = origNode->get("inject");
        if (!injNode && getDictNode(origNode, "inject")) injNode = getDictNode(origNode, "inject");
        if (injNode) d->inject = nodeListToDirectives(injNode, basePath, visited, err);
        return d;
    }

    // ── noeslot ───────────────────────────────────────────────────────────────
    if (key == "noeslot" || key == "no_eslot") {
        String val = nodeStr(node).trim();
        // If false or 0 or off, noeslot is ignored (no-op)
        if (val == "false" || val == "0" || val == "no" || val == "off") {
            return nullptr;
        }
        auto *d = new DNoESlot();
        d->enabled = true;
        if (val == "true" || val == "1" || val == "yes" || val == "on") {
            // d->exceptions is empty, meaning all outside eslots are masked
        } else if (node->size() > 0 && (val.isEmpty() || val == "[]")) {
            for (size_t i = 0; i < node->size(); ++i) {
                String s = nodeStr((*node)[i]).trim();
                while (s.startsWith("[") && s.endsWith("]") && s.length() >= 2) {
                    s = s.substring(1, s.length() - 1).trim();
                }
                if (!s.isEmpty() && s != "true" && s != "false" && s != "[]") {
                    d->exceptions.push(s);
                }
            }
        } else if (!val.isEmpty()) {
            while (val.startsWith("[") && val.endsWith("]") && val.length() >= 2) {
                val = val.substring(1, val.length() - 1).trim();
            }
            if (!val.isEmpty() && val != "true" && val != "false" && val != "[]" && val != "{}") {
                Array<String> parts = val.split(",");
                for (size_t i = 0; i < parts.length(); ++i) {
                    String t = parts[i].trim();
                    if (!t.isEmpty() && t != "true" && t != "false" && t != "[]") {
                        d->exceptions.push(t);
                    }
                }
            }
        }
        return d;
    }

    // ── entry / enter ────────────────────────────────────────────────────────
    if (key == "entry" || key == "enter") {
        auto *d = new DEntry();
        d->name = nodeStr(node).trim();
        if (d->name.isEmpty()) d->name = childString(node, "name").trim();
        if (d->name.isEmpty()) d->name = childString(origNode, "entry").trim();
        if (d->name.isEmpty()) d->name = childString(origNode, "enter").trim();
        NodeBase *injNode = node->get("inject");
        if (!injNode && getDictNode(node, "inject")) injNode = getDictNode(node, "inject");
        if (!injNode && origNode->get("inject")) injNode = origNode->get("inject");
        if (!injNode && getDictNode(origNode, "inject")) injNode = getDictNode(origNode, "inject");
        if (injNode) d->inject = nodeListToDirectives(injNode, basePath, visited, err);
        return d;
    }

    // ── bin ───────────────────────────────────────────────────────────────────
    if (key == "bin") {
        auto *d = new DBin();
        String scalar = nodeStr(node).trim();
        if (!scalar.isEmpty() && scalar != "true" && scalar != "false") {
            d->name = scalar;
        }
        if (d->name.isEmpty()) d->name = getProp("name");
        d->description = getProp("description");
        d->icon        = getProp("icon");
        NodeBase *entryNode = getPropNode("entry");
        if (entryNode) d->entry = nodeListToDirectives(entryNode, basePath, visited, err);
        return d;
    }

    if (key == "bindir") {
        auto *d = new DBinDir();
        d->path = nodeStr(node);
        if (!d->path.isEmpty() && d->path[0] != '/' && !basePath.isEmpty()) {
            d->path = basePath + "/" + d->path;
        }
        return d;
    }

    if (key.isEmpty() || key.startsWith("#") || key.startsWith("_") || key == "comment")
        return nullptr;

    logWarn("tau: unknown directive '", key, "' — skipping");
    return nullptr;
}

// ─── Node list → DirectiveList ────────────────────────────────────────────────

DirectiveList Manifest::nodeListToDirectives(NodeBase *list,
                                              const String &basePath,
                                              Array<String> &visited,
                                              ParseError &err) {
    DirectiveList result;
    if (!list) return result;

    if (!list->getName().isEmpty() && list->size() == 1 && (*list)[0] && (*list)[0]->getName().isEmpty()) {
        list = (*list)[0];
    }

    for (size_t i = 0; i < list->size(); ++i) {
        if (!err.ok) break;
        NodeBase *child = (*list)[i];
        if (!child) continue;

        NodeBase *vNode = (child->getName() == "var" || child->getName() == "regvar" || child->getName() == "rallvar") ? child : (child->size() > 0 ? (*child)[0] : nullptr);
        String vKey = vNode ? vNode->getName() : child->getName();
        if (vNode && (vKey == "var" || vKey == "regvar" || vKey == "rallvar") && vNode->size() > 0) {
            for (size_t k = 0; k < vNode->size(); ++k) {
                NodeBase *c = (*vNode)[k];
                if (c && !c->getName().isEmpty()) {
                    DVarBase *d = (vKey == "regvar") ? (DVarBase*)new DRegVar() : (vKey == "rallvar") ? (DVarBase*)new DRallVar() : (DVarBase*)new DVar();
                    String fullExpr = c->getName() + " " + nodeStr(c);
                    String kStr, vStr;
                    VarOp op = parseVarOp(fullExpr, kStr, vStr);
                    if (op != VarOp::Assign || fullExpr.find("=") >= 0) {
                        d->key = kStr;
                        d->value = vStr;
                        d->op = op;
                    } else {
                        d->key = c->getName();
                        d->value = nodeStr(c);
                        d->op = VarOp::Assign;
                    }
                    result.push(d);
                }
            }
            continue;
        }

        Directive *d = nodeToDirective(child, basePath, visited, err);
        if (d) result.push(d);
    }
    return result;
}

// ─── parse ────────────────────────────────────────────────────────────────────

ParsedManifest Manifest::parse(const String &yaml,
                                const String &basePath,
                                Array<String> &visited,
                                ParseError &err) {
    ParsedManifest result;
    err.ok = true;

    Array<String> lines = yaml.split("\n");
    String cleanYaml;
    for (size_t i = 0; i < lines.length(); ++i) {
        String l = lines[i];
        String trimmed = l.trim();
        if (trimmed.startsWith("#")) continue;
        cleanYaml += l + "\n";
    }

    NodeBase root;
    if (!YAML::parse(cleanYaml, root)) {
        err.ok = false;
        err.message = "YAML syntax error";
        return result;
    }

    result.directives = nodeListToDirectives(&root, basePath, visited, err);


    for (auto *d : result.directives) {
        if (!d) continue;
        if (auto *m = dynamic_cast<DName *>(d))        result.meta.name        = m->value;
        if (auto *m = dynamic_cast<DDescription *>(d)) result.meta.description = m->value;
        if (auto *m = dynamic_cast<DVersion *>(d))     result.meta.version     = m->value;
        if (auto *m = dynamic_cast<DWebsite *>(d))     result.meta.website     = m->value;
        if (auto *m = dynamic_cast<DIssues *>(d))      result.meta.issues      = m->value;
        if (auto *m = dynamic_cast<DLicense *>(d))     result.meta.license     = m->value;
        if (auto *m = dynamic_cast<DStars *>(d))       result.meta.stars       = parseLong(m->value);
        if (auto *m = dynamic_cast<DLanguages *>(d))   result.meta.languages   = m->value;
    }

    return result;
}

// ─── load ─────────────────────────────────────────────────────────────────────

ParsedManifest Manifest::load(const String &path,
                               Array<String> &visited,
                               ParseError &err) {
    ParsedManifest empty;

    String absPath = path;
    if (absPath[0] != '/') {
        char buf[4096];
        if (::realpath(path.c_str(), buf)) absPath = String(buf);
    }

    for (size_t i = 0; i < visited.length(); ++i) {
        if (visited[i] == absPath) {
            err.ok = false;
            err.message = String("Cyclic manifest: ") + absPath;
            return empty;
        }
    }
    visited.push(absPath);

    Resource::LinuxFS fs;
    String content = fs.read(absPath);
    if (content.isEmpty()) {
        err.ok = false;
        err.message = String("Cannot read manifest: ") + absPath;
        return empty;
    }

    String basePath = absPath;
    long long sl = rfind(basePath, '/');
    if (sl >= 0) basePath = basePath.substring(0, (size_t)sl);

    return parse(content, basePath, visited, err);
}

// ─── applySlots ───────────────────────────────────────────────────────────────

DirectiveList Manifest::cloneDirectives(const DirectiveList &list) {
    if (list.length() == 0) return DirectiveList();
    String yaml = Manifest::toYAML(list);
    Array<String> dummyVisited;
    ParseError dummyErr;
    ParsedManifest pm = Manifest::parse(yaml, "", dummyVisited, dummyErr);
    DirectiveList res;
    for (auto *d : pm.directives) res.push(d);
    pm.directives.clear();
    return res;
}


static void injectIntoSlot(DirectiveList &base, DirectiveList &child) {
    int slotIdx = -1;
    for (size_t i = 0; i < base.length(); ++i) {
        if (base[i] && base[i]->kind == DirectiveKind::Slot) {
            slotIdx = (int)i;
            break;
        }
    }
    if (slotIdx >= 0) {
        delete base[(size_t)slotIdx];
        DirectiveList merged;
        for (size_t i = 0; i < base.length(); ++i) {
            if ((int)i == slotIdx) {
                for (auto *c : child) merged.push(c);
            } else {
                merged.push(base[i]);
            }
        }
        base = Xi::Move(merged);
        child.clear();
    } else {
        for (auto *c : child) base.push(c);
        child.clear();
    }
}

DirectiveList Manifest::applySlots(Array<ParsedManifest *> &templates,
                                    ParsedManifest &final_) {
    if (templates.length() == 0) {
        DirectiveList result;
        for (auto *d : final_.directives) result.push(d);
        final_.directives.clear();
        return result;
    }

    DirectiveList current = cloneDirectives(templates[0]->directives);
    for (size_t i = 1; i < templates.length(); ++i) {
        DirectiveList nextChunk = cloneDirectives(templates[i]->directives);
        injectIntoSlot(current, nextChunk);
    }

    DirectiveList finalChunk;
    for (auto *d : final_.directives) finalChunk.push(d);
    final_.directives.clear();

    injectIntoSlot(current, finalChunk);

    // Clean up any remaining un-injected DSlot directives
    for (size_t i = 0; i < current.length(); ) {
        if (current[i] && current[i]->kind == DirectiveKind::Slot) {
            delete current[i];
            current.splice(i, 1);
        } else {
            ++i;
        }
    }

    return current;
}


// ─── interpolate ─────────────────────────────────────────────────────────────

String Manifest::interpolate(const String &s, Map<String, String> &vars) {
    String result;
    size_t len = s.length();
    for (size_t i = 0; i < len; ) {
        if (s[i] == '$' && i + 1 < len) {
            size_t j = i + 1;
            while (j < len && (::isalnum(s[j]) || s[j] == '_')) ++j;
            String varName = s.substring(i + 1, j);
            const String *val = vars.get(varName);
            if (val && !val->isEmpty()) {
                result += *val;
            } else {
                char *ev = ::getenv(varName.c_str());
                if (ev && *ev) {
                    String eStr(ev);
                    vars[varName] = eStr;
                    result += eStr;
                } else if (val) {
                    result += *val;
                } else {
                    result += "$" + varName;
                }
            }
            i = j;
            continue;
        } else if (s[i] == '%' && i + 1 < len) {
            size_t j = i + 1;
            while (j < len && (::isalnum(s[j]) || s[j] == '_' || s[j] == '-')) {
                ++j;
            }
            if (j > i + 1) {
                String varName = s.substring(i + 1, j);
                const String *val = vars.get(varName);
                if (val && !val->isEmpty()) {
                    result += *val;
                } else {
                    char *ev = ::getenv(varName.c_str());
                    if (ev && *ev) {
                        String eStr(ev);
                        vars[varName] = eStr;
                        result += eStr;
                    } else {
                        bool isNumeric = true;
                        for (size_t k = 0; k < varName.length(); ++k) {
                            if (varName[k] < '0' || varName[k] > '9') { isNumeric = false; break; }
                        }
                        if (isNumeric) {
                            if (s.startsWith("%" + varName) && s.length() == varName.length() + 1) {
                                String allocated = "/tmp/tau-" + varName;
                                if (pathExists(allocated)) {
                                    removeDirRecursive(allocated);
                                }
                                vars[varName] = allocated;
                                ::setenv(varName.c_str(), allocated.c_str(), 1);
                                result += allocated;
                            } else {
                                result += varName;
                            }
                        } else {
                            result += "%" + varName;
                        }
                    }
                }
                i = j;
                continue;
            }
        }
        result.push(s[i++]);
    }
    return result;
}

String Manifest::interpolate(const String &s, const Map<String, String> &vars) {
    Map<String, String> copy = vars;
    return interpolate(s, copy);
}

void Manifest::resolveVariables(DirectiveList &directives, Map<String, String> &vars) {
    for (size_t i = 0; i < directives.length(); ++i) {
        Directive *d = directives[i];
        if (!d) continue;

        switch (d->kind) {
            case DirectiveKind::Name: {
                auto *n = static_cast<DName *>(d);
                if (!n->value.isEmpty()) {
                    n->value = interpolate(n->value, vars);
                    vars["name"] = n->value;
                    vars["NAME"] = n->value;
                    vars["instance"] = n->value;
                    vars["INSTANCE"] = n->value;
                }
                break;
            }
            case DirectiveKind::Description: {
                auto *desc = static_cast<DDescription *>(d);
                desc->value = interpolate(desc->value, vars);
                break;
            }
            case DirectiveKind::Spawn: {
                auto *sp = static_cast<DSpawn *>(d);
                if (!sp->name.isEmpty()) sp->name = interpolate(sp->name, vars);
                break;
            }
            case DirectiveKind::Mkdir: {
                auto *m = static_cast<DMkdir *>(d);
                m->path = interpolate(m->path, vars);
                break;
            }
            case DirectiveKind::Unlink: {
                auto *u = static_cast<DUnlink *>(d);
                u->path = interpolate(u->path, vars);
                break;
            }
            case DirectiveKind::Copy: {
                auto *c = static_cast<DCopy *>(d);
                c->source = interpolate(c->source, vars);
                c->target = interpolate(c->target, vars);
                break;
            }
            case DirectiveKind::Symlink: {
                auto *s = static_cast<DSymlink *>(d);
                s->source = interpolate(s->source, vars);
                s->target = interpolate(s->target, vars);
                break;
            }
            case DirectiveKind::Mount: {
                auto *m = static_cast<DMount *>(d);
                m->source = interpolate(m->source, vars);
                if (!m->work.isEmpty()) m->work = interpolate(m->work, vars);
                m->target = interpolate(m->target, vars);
                break;
            }
            case DirectiveKind::Image: {
                auto *img = static_cast<DImage *>(d);
                if (!img->source.isEmpty()) img->source = interpolate(img->source, vars);
                if (!img->path.isEmpty()) img->path = interpolate(img->path, vars);
                if (img->source.isEmpty()) img->source = img->path;
                img->path = img->source;
                if (!img->upper.isEmpty()) img->upper = interpolate(img->upper, vars);
                if (!img->size.isEmpty()) img->size = interpolate(img->size, vars);
                if (!img->label.isEmpty()) img->label = interpolate(img->label, vars);
                if (!img->table.isEmpty()) img->table = interpolate(img->table, vars);
                if (!img->work.isEmpty()) img->work = interpolate(img->work, vars);
                if (!img->lower.isEmpty()) img->lower = interpolate(img->lower, vars);
                if (!img->target.isEmpty()) img->target = interpolate(img->target, vars);
                for (size_t p = 0; p < img->partitions.length(); ++p) {
                    if (!img->partitions[p].name.isEmpty()) img->partitions[p].name = interpolate(img->partitions[p].name, vars);
                    if (!img->partitions[p].size.isEmpty()) img->partitions[p].size = interpolate(img->partitions[p].size, vars);
                    if (!img->partitions[p].offset.isEmpty()) img->partitions[p].offset = interpolate(img->partitions[p].offset, vars);
                    if (!img->partitions[p].label.isEmpty()) img->partitions[p].label = interpolate(img->partitions[p].label, vars);
                    if (!img->partitions[p].flags.isEmpty()) img->partitions[p].flags = interpolate(img->partitions[p].flags, vars);
                    if (!img->partitions[p].upper.isEmpty()) img->partitions[p].upper = interpolate(img->partitions[p].upper, vars);
                    if (!img->partitions[p].source.isEmpty()) img->partitions[p].source = interpolate(img->partitions[p].source, vars);
                    if (img->partitions[p].upper.isEmpty()) img->partitions[p].upper = img->partitions[p].source;
                    img->partitions[p].source = img->partitions[p].upper;
                    img->partitions[p].work   = interpolate(img->partitions[p].work, vars);
                    img->partitions[p].lower  = interpolate(img->partitions[p].lower, vars);
                    img->partitions[p].target = interpolate(img->partitions[p].target, vars);
                }
                break;
            }
            case DirectiveKind::Docker: {
                auto *dk = static_cast<DDocker *>(d);
                if (!dk->image.isEmpty())  dk->image  = interpolate(dk->image, vars);
                if (!dk->target.isEmpty()) dk->target = interpolate(dk->target, vars);
                if (!dk->source.isEmpty()) dk->source = interpolate(dk->source, vars);
                if (!dk->work.isEmpty())   dk->work   = interpolate(dk->work, vars);
                break;
            }
            case DirectiveKind::VM: {
                auto *vm = static_cast<DVM *>(d);
                if (!vm->kernel.isEmpty())  vm->kernel  = interpolate(vm->kernel, vars);
                if (!vm->initrd.isEmpty())  vm->initrd  = interpolate(vm->initrd, vars);
                if (!vm->cmdline.isEmpty()) vm->cmdline = interpolate(vm->cmdline, vars);
                if (!vm->name.isEmpty())    vm->name    = interpolate(vm->name, vars);
                for (size_t i = 0; i < vm->drives.length(); ++i) {
                    vm->drives[i].source = interpolate(vm->drives[i].source, vars);
                }
                break;
            }
            case DirectiveKind::Autofreeze: break;
            case DirectiveKind::Chroot: {
                auto *c = static_cast<DChroot *>(d);
                c->path = interpolate(c->path, vars);
                break;
            }
            case DirectiveKind::IP: {
                auto *ip = static_cast<DIP *>(d);
                ip->command = interpolate(ip->command, vars);
                break;
            }
            case DirectiveKind::Veth: {
                auto *v = static_cast<DVeth *>(d);
                if (!v->source.isEmpty()) v->source = interpolate(v->source, vars);
                if (!v->target.isEmpty()) v->target = interpolate(v->target, vars);
                if (!v->hostName.isEmpty()) v->hostName = interpolate(v->hostName, vars);
                if (!v->peerName.isEmpty()) v->peerName = interpolate(v->peerName, vars);
                if (v->source.isEmpty()) v->source = v->hostName;
                if (v->target.isEmpty()) v->target = v->peerName;
                v->hostName = v->source;
                v->peerName = v->target;
                if (!v->sip.isEmpty()) v->sip = interpolate(v->sip, vars);
                if (!v->ip.isEmpty()) v->ip = interpolate(v->ip, vars);
                break;
            }
            case DirectiveKind::Route: {
                auto *r = static_cast<DRoute *>(d);
                r->destination = interpolate(r->destination, vars);
                for (size_t hi = 0; hi < r->hops.length(); ++hi) {
                    if (!r->hops[hi].ip.isEmpty()) r->hops[hi].ip = interpolate(r->hops[hi].ip, vars);
                    if (!r->hops[hi].link.isEmpty()) r->hops[hi].link = interpolate(r->hops[hi].link, vars);
                }
                break;
            }
            case DirectiveKind::Trigger: {
                auto *t = static_cast<DTrigger *>(d);
                t->name = interpolate(t->name, vars);
                break;
            }
            case DirectiveKind::Wait: {
                auto *w = static_cast<DWait *>(d);
                for (size_t wi = 0; wi < w->targets.length(); ++wi) {
                    w->targets[wi] = interpolate(w->targets[wi], vars);
                }
                break;
            }
            case DirectiveKind::NoRead:
            case DirectiveKind::NoWrite:
            case DirectiveKind::NoExecute:
            case DirectiveKind::NoMount:
            case DirectiveKind::NoWatch: {
                auto *pr = static_cast<DPathRestriction *>(d);
                for (size_t pi = 0; pi < pr->paths.length(); ++pi) {
                    pr->paths[pi] = interpolate(pr->paths[pi], vars);
                }
                break;
            }
            case DirectiveKind::NewNet: {
                auto *nn = static_cast<DNewNet *>(d);
                for (size_t a = 0; a < nn->allowList.length(); ++a) {
                    nn->allowList[a] = interpolate(nn->allowList[a], vars);
                }
                break;
            }
            case DirectiveKind::Forward: {
                auto *f = static_cast<DForward *>(d);
                f->target = interpolate(f->target, vars);
                f->source = interpolate(f->source, vars);
                f->port = interpolate(f->port, vars);
                f->sourcePort = interpolate(f->sourcePort, vars);
                f->sourceIP = interpolate(f->sourceIP, vars);
                f->ip = interpolate(f->ip, vars);
                f->protocol = interpolate(f->protocol, vars);
                break;
            }
            case DirectiveKind::Macvlan: {
                auto *m = static_cast<DMacvlan *>(d);
                m->parent = interpolate(m->parent, vars);
                m->name = interpolate(m->name, vars);
                m->mac = interpolate(m->mac, vars);
                break;
            }
            case DirectiveKind::IPVlan: {
                auto *iv = static_cast<DIPVlan *>(d);
                iv->parent = interpolate(iv->parent, vars);
                iv->name = interpolate(iv->name, vars);
                break;
            }
            case DirectiveKind::Bridge: {
                auto *b = static_cast<DBridge *>(d);
                if (!b->source.isEmpty()) b->source = interpolate(b->source, vars);
                if (!b->name.isEmpty()) b->name = interpolate(b->name, vars);
                if (b->source.isEmpty()) b->source = b->name;
                b->name = b->source;
                if (!b->target.isEmpty()) b->target = interpolate(b->target, vars);
                if (!b->attach.isEmpty()) b->attach = interpolate(b->attach, vars);
                if (b->target.isEmpty()) b->target = b->attach;
                b->attach = b->target;
                if (!b->address.isEmpty()) b->address = interpolate(b->address, vars);
                break;
            }
            case DirectiveKind::Local: {
                auto *l = static_cast<DLocal *>(d);
                l->source = interpolate(l->source, vars);
                l->target = interpolate(l->target, vars);
                break;
            }
            case DirectiveKind::Git: {
                auto *g = static_cast<DGit *>(d);
                g->source = interpolate(g->source, vars);
                g->branch = interpolate(g->branch, vars);
                g->target = interpolate(g->target, vars);
                break;
            }
            case DirectiveKind::GHRelease: {
                auto *gh = static_cast<DGHRelease *>(d);
                gh->source = interpolate(gh->source, vars);
                gh->branch = interpolate(gh->branch, vars);
                gh->name = interpolate(gh->name, vars);
                gh->target = interpolate(gh->target, vars);
                break;
            }
            case DirectiveKind::Fail: {
                auto *f = static_cast<DFail *>(d);
                f->message = interpolate(f->message, vars);
                break;
            }
            case DirectiveKind::Throw: {
                auto *t = static_cast<DThrow *>(d);
                t->message = interpolate(t->message, vars);
                break;
            }
            case DirectiveKind::WaitExit: {
                auto *we = static_cast<DWaitExit *>(d);
                resolveVariables(we->children, vars);
                break;
            }
            case DirectiveKind::Retry: {
                auto *re = static_cast<DRetry *>(d);
                resolveVariables(re->children, vars);
                break;
            }
            case DirectiveKind::Dotenv: {
                auto *de = static_cast<DDotenv *>(d);
                de->path = interpolate(de->path, vars);
                String p = de->path;
                int fd = ::open(p.c_str(), O_RDONLY);
                if (fd < 0 && p.length() > 0 && p[0] != '/') {
                    char cwdBuf[4096];
                    if (::getcwd(cwdBuf, sizeof(cwdBuf))) {
                        String cand = String(cwdBuf) + "/" + p;
                        fd = ::open(cand.c_str(), O_RDONLY);
                    }
                }
                if (fd >= 0) {
                    char buf[16384];
                    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
                    ::close(fd);
                    if (n > 0) {
                        buf[n] = '\0';
                        String content(buf);
                        Array<String> lines = content.split("\n");
                        for (size_t li = 0; li < lines.length(); ++li) {
                            String line = lines[li].trim();
                            if (line.isEmpty() || line.startsWith("#")) continue;
                            long long eq = line.find("=");
                            if (eq < 0) continue;
                            String k = line.substring(0, (size_t)eq).trim();
                            String v = line.substring((size_t)eq + 1).trim();
                            if (v.startsWith("\"") && v.endsWith("\"") && v.length() >= 2) {
                                v = v.substring(1, v.length() - 1);
                            } else if (v.startsWith("'") && v.endsWith("'") && v.length() >= 2) {
                                v = v.substring(1, v.length() - 1);
                            }
                            if (!k.isEmpty()) {
                                vars[k] = v;
                                ::setenv(k.c_str(), v.c_str(), 1);
                            }
                        }
                    }
                }
                break;
            }
            case DirectiveKind::Var:
            case DirectiveKind::RegVar:
            case DirectiveKind::RallVar: {
                auto *v = static_cast<DVarBase *>(d);
                v->key = interpolate(v->key, vars);
                v->value = interpolate(v->value, vars);
                if (!v->key.isEmpty() && v->op == VarOp::Assign) {
                    vars[v->key] = v->value;
                    ::setenv(v->key.c_str(), v->value.c_str(), 1);
                }
                break;
            }
            case DirectiveKind::Or: case DirectiveKind::And:
            case DirectiveKind::Nand: case DirectiveKind::Nor: case DirectiveKind::Xor: {
                auto *lb = static_cast<DLogicBlock *>(d);
                resolveVariables(lb->children, vars);
                break;
            }

            case DirectiveKind::NoWait: {
                auto *nw = static_cast<DNoWait *>(d);
                resolveVariables(nw->children, vars);
                break;
            }
            case DirectiveKind::WaitAll: {
                auto *wa = static_cast<DWaitAll *>(d);
                resolveVariables(wa->children, vars);
                break;
            }
            case DirectiveKind::ESlot: {
                auto *es = static_cast<DESlot *>(d);
                es->name = interpolate(es->name, vars);
                resolveVariables(es->entry, vars);
                resolveVariables(es->escape, vars);
                resolveVariables(es->inject, vars);
                break;
            }
            case DirectiveKind::NoESlot: {
                auto *nes = static_cast<DNoESlot *>(d);
                for (size_t i = 0; i < nes->exceptions.length(); ++i) {
                    nes->exceptions[i] = interpolate(nes->exceptions[i], vars);
                }
                break;
            }
            case DirectiveKind::Entry: {
                auto *en = static_cast<DEntry *>(d);
                en->name = interpolate(en->name, vars);
                resolveVariables(en->inject, vars);
                break;
            }
            case DirectiveKind::Bin: {
                auto *b = static_cast<DBin *>(d);
                b->name = interpolate(b->name, vars);
                resolveVariables(b->entry, vars);
                break;
            }
            case DirectiveKind::BinDir: {
                auto *bd = static_cast<DBinDir *>(d);
                bd->path = interpolate(bd->path, vars);
                break;
            }
            default:
                break;

        }
    }
}



String Manifest::toYAML(const DirectiveList &directives, int indent) {
    String out;
    String pad;
    for (int i = 0; i < indent; ++i) pad += " ";

    for (auto *d : directives) {
        if (!d) continue;
        switch (d->kind) {
            case DirectiveKind::WaitExit: {
                auto *v = static_cast<DWaitExit*>(d);
                out += pad + "- waitexit:\n";
                out += pad + "    entry:\n";
                out += toYAML(v->children, indent + 8);
                break;
            }
            case DirectiveKind::Retry: {
                auto *v = static_cast<DRetry*>(d);
                out += pad + "- retry:\n";
                out += pad + "    times: " + intStr(v->times) + "\n";
                out += pad + "    backoff: " + doubleStr(v->backoff) + "s\n";
                out += pad + "    children:\n";
                out += toYAML(v->children, indent + 8);
                break;
            }
            case DirectiveKind::Dotenv: {
                auto *v = static_cast<DDotenv*>(d);
                out += pad + "- dotenv: " + v->path + "\n";
                break;
            }

            case DirectiveKind::Spawn: {
                auto *v = static_cast<DSpawn*>(d);
                String cmdEscaped = v->command.replace("\"", "\\\"");
                if (v->waitForExit && v->name.isEmpty() && v->exitCode < 0 && v->pid <= 0) {
                    out += pad + "- spawn: \"" + cmdEscaped + "\"\n";
                } else {
                    out += pad + "- spawn:\n";
                    out += pad + "    command: \"" + cmdEscaped + "\"\n";
                    out += pad + "    wait: " + (v->waitForExit ? "true\n" : "false\n");
                    if (!v->name.isEmpty()) out += pad + "    name: " + v->name + "\n";
                    if (v->exitCode >= 0) {
                        out += pad + "    exitcode: " + intStr(v->exitCode) + "\n";
                    } else if (v->pid > 0) {
                        out += pad + "    pid: " + intStr(v->pid) + "\n";
                    }
                }
                break;
            }

            case DirectiveKind::Memory: {
                auto *v = static_cast<DMemory*>(d);
                out += pad + "- memory:\n";
                if (!v->raw.isEmpty())    out += pad + "    limit: " + v->raw + "\n";
                if (!v->rawMax.isEmpty()) out += pad + "    max: "   + v->rawMax + "\n";
                break;
            }
            case DirectiveKind::CPUSet: {
                auto *v = static_cast<DCPUSet*>(d);
                out += pad + "- cpuset: " + v->cpus + "\n";
                break;
            }
            case DirectiveKind::CPU: {
                auto *v = static_cast<DCPU*>(d);
                out += pad + "- cpu:\n";
                if (v->quota >= 0)    out += pad + "    quota: " + intStr(v->quota) + "\n";
                if (v->quotaMax >= 0) out += pad + "    max: "   + intStr(v->quotaMax) + "\n";
                break;
            }
            case DirectiveKind::Autofreeze: {
                auto *v = static_cast<DAutofreeze*>(d);
                out += pad + "- autofreeze:\n";
                out += pad + "    wol: " + (v->wol ? "true\n" : "false\n");
                out += pad + "    cpu_threshold: " + intStr((long long)v->cpuThreshold) + "%\n";
                out += pad + "    timer: " + v->rawTimer + "\n";
                if (!v->enabled) out += pad + "    enabled: false\n";
                break;
            }
            case DirectiveKind::Chroot: {
                auto *v = static_cast<DChroot*>(d);
                out += pad + "- chroot: " + v->path + "\n";
                break;
            }
            case DirectiveKind::Isolate: {
                auto *v = static_cast<DIsolate*>(d);
                out += pad + "- isolate: " + (v->enable ? "true\n" : "false\n");
                break;
            }

            case DirectiveKind::Wait: {
                auto *v = static_cast<DWait*>(d);
                if (v->seconds > 0) {
                    out += pad + "- wait: " + doubleStr(v->seconds) + "s\n";
                } else if (v->targets.length() == 1) {
                    out += pad + "- wait: " + v->targets[0] + "\n";
                } else if (v->targets.length() > 1) {
                    out += pad + "- wait: [";
                    for (size_t ti = 0; ti < v->targets.length(); ++ti) {
                        if (ti > 0) out += ", ";
                        out += v->targets[ti];
                    }
                    out += "]\n";
                } else {
                    out += pad + "- wait: 0s\n";
                }
                break;
            }
            case DirectiveKind::Trigger: {
                auto *v = static_cast<DTrigger*>(d);
                out += pad + "- trigger: " + v->name + "\n";
                break;
            }
            case DirectiveKind::NoRead: {
                auto *v = static_cast<DNoRead*>(d);
                if (v->paths.length() == 1) out += pad + "- noread: " + v->paths[0] + "\n";
                else {
                    out += pad + "- noread:\n";
                    for (size_t pi = 0; pi < v->paths.length(); ++pi) out += pad + "    - " + v->paths[pi] + "\n";
                }
                break;
            }
            case DirectiveKind::NoWrite: {
                auto *v = static_cast<DNoWrite*>(d);
                if (v->paths.length() == 1) out += pad + "- nowrite: " + v->paths[0] + "\n";
                else {
                    out += pad + "- nowrite:\n";
                    for (size_t pi = 0; pi < v->paths.length(); ++pi) out += pad + "    - " + v->paths[pi] + "\n";
                }
                break;
            }
            case DirectiveKind::NoExecute: {
                auto *v = static_cast<DNoExecute*>(d);
                if (v->paths.length() == 1) out += pad + "- noexecute: " + v->paths[0] + "\n";
                else {
                    out += pad + "- noexecute:\n";
                    for (size_t pi = 0; pi < v->paths.length(); ++pi) out += pad + "    - " + v->paths[pi] + "\n";
                }
                break;
            }
            case DirectiveKind::NoMount: {
                auto *v = static_cast<DNoMount*>(d);
                if (v->paths.length() == 1) out += pad + "- nomount: " + v->paths[0] + "\n";
                else {
                    out += pad + "- nomount:\n";
                    for (size_t pi = 0; pi < v->paths.length(); ++pi) out += pad + "    - " + v->paths[pi] + "\n";
                }
                break;
            }
            case DirectiveKind::NoWatch: {
                auto *v = static_cast<DNoWatch*>(d);
                if (v->paths.length() == 1) out += pad + "- nowatch: " + v->paths[0] + "\n";
                else {
                    out += pad + "- nowatch:\n";
                    for (size_t pi = 0; pi < v->paths.length(); ++pi) out += pad + "    - " + v->paths[pi] + "\n";
                }
                break;
            }
            case DirectiveKind::Route: {
                auto *v = static_cast<DRoute*>(d);
                out += pad + "- route: " + v->destination + "\n";
                if (v->hops.length() == 1 && v->hops[0].link.isEmpty() && v->hops[0].weight == 1) {
                    out += pad + "  via: " + v->hops[0].ip + "\n";
                } else if (v->hops.length() > 0) {
                    out += pad + "  via:\n";
                    for (size_t hi = 0; hi < v->hops.length(); ++hi) {
                        out += pad + "    - ip: " + v->hops[hi].ip + "\n";
                        if (!v->hops[hi].link.isEmpty()) out += pad + "      link: " + v->hops[hi].link + "\n";
                        if (v->hops[hi].weight > 1) out += pad + "      weight: " + intStr(v->hops[hi].weight) + "\n";
                    }
                }
                break;
            }
            case DirectiveKind::Veth: {
                auto *v = static_cast<DVeth*>(d);
                out += pad + "- veth:\n";
                String src = !v->source.isEmpty() ? v->source : v->hostName;
                String tgt = !v->target.isEmpty() ? v->target : v->peerName;
                out += pad + "    source: " + src + "\n";
                if (!v->sip.isEmpty()) out += pad + "    sip: " + v->sip + "\n";
                out += pad + "    target: " + tgt + "\n";
                if (!v->ip.isEmpty())  out += pad + "    ip: " + v->ip + "\n";
                break;
            }
            case DirectiveKind::IP: {
                auto *v = static_cast<DIP*>(d);
                out += pad + "- ip: " + v->command + "\n";
                break;
            }
            case DirectiveKind::NewNet: {
                auto *v = static_cast<DNewNet*>(d);
                String allow;
                for (size_t i = 0; i < v->allowList.length(); ++i) {
                    if (i > 0) allow += ",";
                    allow += v->allowList[i];
                }
                out += pad + "- newnet: " + allow + "\n";
                break;
            }
            case DirectiveKind::Forward: {
                auto *v = static_cast<DForward*>(d);
                out += pad + "- forward:\n";
                out += pad + "    target: " + v->target + "\n";
                if (!v->source.isEmpty())     out += pad + "    source: " + v->source + "\n";
                if (!v->port.isEmpty())       out += pad + "    port: " + v->port + "\n";
                if (!v->sourcePort.isEmpty()) out += pad + "    source-port: " + v->sourcePort + "\n";
                if (!v->sourceIP.isEmpty())   out += pad + "    source-ip: " + v->sourceIP + "\n";
                if (!v->ip.isEmpty())         out += pad + "    ip: " + v->ip + "\n";
                if (!v->protocol.isEmpty())   out += pad + "    protocol: " + v->protocol + "\n";
                break;
            }
            case DirectiveKind::Macvlan: {
                auto *v = static_cast<DMacvlan*>(d);
                out += pad + "- macvlan:\n";
                out += pad + "    parent: " + v->parent + "\n";
                out += pad + "    name: " + v->name + "\n";
                if (!v->mode.isEmpty()) out += pad + "    mode: " + v->mode + "\n";
                if (!v->mac.isEmpty())  out += pad + "    mac: " + v->mac + "\n";
                break;
            }
            case DirectiveKind::IPVlan: {
                auto *v = static_cast<DIPVlan*>(d);
                out += pad + "- ipvlan:\n";
                out += pad + "    parent: " + v->parent + "\n";
                out += pad + "    name: " + v->name + "\n";
                if (!v->mode.isEmpty()) out += pad + "    mode: " + v->mode + "\n";
                break;
            }
            case DirectiveKind::Bridge: {
                auto *v = static_cast<DBridge*>(d);
                out += pad + "- bridge:\n";
                String brName = !v->source.isEmpty() ? v->source : v->name;
                String brTarget = !v->target.isEmpty() ? v->target : v->attach;
                out += pad + "    source: " + brName + "\n";
                if (!brTarget.isEmpty())   out += pad + "    target: " + brTarget + "\n";
                if (!v->address.isEmpty()) out += pad + "    ip: " + v->address + "\n";
                break;
            }

            case DirectiveKind::Mount: {
                auto *v = static_cast<DMount*>(d);
                out += pad + "- mount:\n";
                out += pad + "    source: " + v->source + "\n";
                if (!v->work.isEmpty()) out += pad + "    work: " + v->work + "\n";
                out += pad + "    target: " + v->target + "\n";
                out += pad + "    read: " + (v->read ? "true\n" : "false\n");
                out += pad + "    write: " + (v->write ? "true\n" : "false\n");
                break;
            }
            case DirectiveKind::Copy: {
                auto *v = static_cast<DCopy*>(d);
                out += pad + "- copy:\n";
                out += pad + "    source: " + v->source + "\n";
                out += pad + "    target: " + v->target + "\n";
                break;
            }
            case DirectiveKind::Unlink: {
                auto *v = static_cast<DUnlink*>(d);
                out += pad + "- unlink: " + v->path + "\n";
                break;
            }
            case DirectiveKind::Mkdir: {
                auto *v = static_cast<DMkdir*>(d);
                out += pad + "- mkdir: " + v->path + "\n";
                break;
            }
            case DirectiveKind::Symlink: {
                auto *v = static_cast<DSymlink*>(d);
                out += pad + "- symlink:\n";
                out += pad + "    source: " + v->source + "\n";
                out += pad + "    target: " + v->target + "\n";
                break;
            }
            case DirectiveKind::Image: {
                auto *v = static_cast<DImage*>(d);
                out += pad + "- image:\n";
                String imgSource = !v->source.isEmpty() ? v->source : v->path;
                out += pad + "    source: " + imgSource + "\n";
                if (!v->size.isEmpty())    out += pad + "    size: "     + v->size    + "\n";
                if (!v->sizeMax.isEmpty()) out += pad + "    size_max: " + v->sizeMax + "\n";
                out += pad + "    type: " + v->type + "\n";
                if (!v->table.isEmpty())  out += pad + "    table: "  + v->table  + "\n";
                if (!v->label.isEmpty())  out += pad + "    label: "  + v->label  + "\n";
                out += pad + "    create: " + (v->create ? "true\n" : "false\n");
                if (!v->upper.isEmpty()) out += pad + "    upper: " + v->upper + "\n";
                if (!v->work.isEmpty())   out += pad + "    work: "   + v->work   + "\n";
                if (!v->lower.isEmpty())  out += pad + "    lower: "  + v->lower  + "\n";
                if (!v->target.isEmpty()) out += pad + "    target: " + v->target + "\n";
                if (v->partitions.length() > 0) {
                    out += pad + "    partitions:\n";
                    for (size_t i = 0; i < v->partitions.length(); ++i) {
                        const auto &p = v->partitions[i];
                        out += pad + "      - name: " + p.name + "\n";
                        if (!p.uuid.isEmpty())   out += pad + "        uuid: "   + p.uuid   + "\n";
                        if (!p.size.isEmpty())   out += pad + "        size: "   + p.size   + "\n";
                        if (!p.type.isEmpty())   out += pad + "        type: "   + p.type   + "\n";
                        if (!p.label.isEmpty())  out += pad + "        label: "  + p.label  + "\n";
                        if (!p.flags.isEmpty())  out += pad + "        flags: "  + p.flags  + "\n";
                        String pUpper = !p.upper.isEmpty() ? p.upper : p.source;
                        if (!pUpper.isEmpty() && pUpper != "@/") out += pad + "        upper: " + pUpper + "\n";
                        if (!p.work.isEmpty())   out += pad + "        work: "   + p.work   + "\n";
                        if (!p.lower.isEmpty())  out += pad + "        lower: "  + p.lower  + "\n";
                        if (!p.target.isEmpty()) out += pad + "        target: " + p.target + "\n";
                    }
                }
                break;
            }
            case DirectiveKind::Docker: {
                auto *v = static_cast<DDocker*>(d);
                out += pad + "- docker:\n";
                if (!v->image.isEmpty())  out += pad + "    image: "  + v->image  + "\n";
                if (!v->target.isEmpty()) out += pad + "    target: " + v->target + "\n";
                if (!v->source.isEmpty()) out += pad + "    source: " + v->source + "\n";
                if (!v->work.isEmpty())   out += pad + "    work: "   + v->work   + "\n";
                break;
            }
            case DirectiveKind::VM: {
                auto *v = static_cast<DVM*>(d);
                out += pad + "- vm:\n";
                if (v->drives.length() > 0) {
                    out += pad + "    drives:\n";
                    for (size_t i = 0; i < v->drives.length(); ++i) {
                        const auto &drv = v->drives[i];
                        out += pad + "      - source: " + drv.source + "\n";
                        if (!drv.format.isEmpty() && drv.format != "auto") {
                            out += pad + "        format: " + drv.format + "\n";
                        }
                        out += pad + "        read: " + (drv.read ? "true\n" : "false\n");
                        out += pad + "        write: " + (drv.write ? "true\n" : "false\n");
                    }
                }
                if (!v->kernel.isEmpty())  out += pad + "    kernel: " + v->kernel + "\n";
                if (!v->initrd.isEmpty())  out += pad + "    initrd: " + v->initrd + "\n";
                if (!v->cmdline.isEmpty()) out += pad + "    cmdline: \"" + v->cmdline + "\"\n";
                if (!v->name.isEmpty())    out += pad + "    name: " + v->name + "\n";
                break;
            }
            case DirectiveKind::Local: {
                auto *v = static_cast<DLocal*>(d);
                out += pad + "- local:\n";
                out += pad + "    source: " + v->source + "\n";
                if (!v->target.isEmpty()) out += pad + "    target: " + v->target + "\n";
                break;
            }
            case DirectiveKind::Git: {
                auto *v = static_cast<DGit*>(d);
                out += pad + "- git:\n";
                out += pad + "    source: " + v->source + "\n";
                out += pad + "    branch: " + v->branch + "\n";
                if (!v->target.isEmpty()) out += pad + "    target: " + v->target + "\n";
                out += pad + "    store: " + (v->store ? "true\n" : "false\n");
                break;
            }
            case DirectiveKind::GHRelease: {
                auto *v = static_cast<DGHRelease*>(d);
                out += pad + "- ghrelease:\n";
                out += pad + "    source: " + v->source + "\n";
                out += pad + "    branch: " + v->branch + "\n";
                out += pad + "    name: " + v->name + "\n";
                if (!v->target.isEmpty()) out += pad + "    target: " + v->target + "\n";
                break;
            }
            case DirectiveKind::Or: case DirectiveKind::And:
            case DirectiveKind::Nand: case DirectiveKind::Nor: case DirectiveKind::Xor: {
                auto *v = static_cast<DLogicBlock*>(d);
                String tag = "and";
                if (d->kind == DirectiveKind::Or) tag = "or";
                else if (d->kind == DirectiveKind::Nand) tag = "nand";
                else if (d->kind == DirectiveKind::Nor) tag = "nor";
                else if (d->kind == DirectiveKind::Xor) tag = "xor";
                out += pad + "- " + tag + ":\n";
                out += pad + "    entry:\n";
                out += toYAML(v->children, indent + 8);
                break;
            }

            case DirectiveKind::Var: case DirectiveKind::RegVar: case DirectiveKind::RallVar: {
                auto *v = static_cast<DVarBase*>(d);
                String tag = "var";
                if (d->kind == DirectiveKind::RegVar) tag = "regvar";
                else if (d->kind == DirectiveKind::RallVar) tag = "rallvar";
                String opStr = "=";
                switch (v->op) {
                    case VarOp::Assign: opStr = "="; break;
                    case VarOp::CheckEquals: opStr = "=="; break;
                    case VarOp::CheckIncludes: opStr = "includes"; break;
                    case VarOp::CheckRegex: opStr = "reg"; break;
                    case VarOp::CheckGT: opStr = ">"; break;
                    case VarOp::CheckGTE: opStr = ">="; break;
                    case VarOp::CheckLT: opStr = "<"; break;
                    case VarOp::CheckLTE: opStr = "<="; break;
                }
                out += pad + "- " + tag + ": " + v->key + " " + opStr + " " + v->value + "\n";
                break;
            }
            case DirectiveKind::NewVar: {
                auto *v = static_cast<DNewVar*>(d);
                if (v->patterns.length() == 1) {
                    out += pad + "- newvar: " + v->patterns[0] + "\n";
                } else {
                    out += pad + "- newvar:\n";
                    for (size_t i = 0; i < v->patterns.length(); ++i) {
                        out += pad + "    - " + v->patterns[i] + "\n";
                    }
                }
                break;
            }
            case DirectiveKind::RmVar: {
                auto *v = static_cast<DRmVar*>(d);
                if (v->patterns.length() == 1) {
                    out += pad + "- rmvar: " + v->patterns[0] + "\n";
                } else {
                    out += pad + "- rmvar:\n";
                    for (size_t i = 0; i < v->patterns.length(); ++i) {
                        out += pad + "    - " + v->patterns[i] + "\n";
                    }
                }
                break;
            }
            case DirectiveKind::Fail: {

                auto *v = static_cast<DFail*>(d);
                out += pad + "- fail: " + v->message + "\n";
                break;
            }
            case DirectiveKind::Throw: {
                auto *v = static_cast<DThrow*>(d);
                out += pad + "- throw: " + v->message + "\n";
                break;
            }
            case DirectiveKind::Name: {
                auto *v = static_cast<DName*>(d);
                out += pad + "- name: " + v->value + "\n";
                break;
            }
            case DirectiveKind::Description: {
                auto *v = static_cast<DDescription*>(d);
                out += pad + "- description: " + v->value + "\n";
                break;
            }
            case DirectiveKind::Version: {
                auto *v = static_cast<DVersion*>(d);
                out += pad + "- version: " + v->value + "\n";
                break;
            }
            case DirectiveKind::Website: {
                auto *v = static_cast<DWebsite*>(d);
                out += pad + "- website: " + v->value + "\n";
                break;
            }
            case DirectiveKind::Issues: {
                auto *v = static_cast<DIssues*>(d);
                out += pad + "- issues: " + v->value + "\n";
                break;
            }
            case DirectiveKind::License: {
                auto *v = static_cast<DLicense*>(d);
                out += pad + "- license: " + v->value + "\n";
                break;
            }
            case DirectiveKind::Stars: {
                auto *v = static_cast<DStars*>(d);
                out += pad + "- stars: " + v->value + "\n";
                break;
            }
            case DirectiveKind::Languages: {
                auto *v = static_cast<DLanguages*>(d);
                out += pad + "- languages: " + v->value + "\n";
                break;
            }
            case DirectiveKind::NoWait: {
                auto *v = static_cast<DNoWait*>(d);
                out += pad + "- nowait:\n";
                out += pad + "    entry:\n";
                out += toYAML(v->children, indent + 8);
                break;
            }
            case DirectiveKind::WaitAll: {
                auto *v = static_cast<DWaitAll*>(d);
                out += pad + "- waitall:\n";
                out += pad + "    entry:\n";
                out += toYAML(v->children, indent + 8);
                break;
            }
            case DirectiveKind::Slot: {
                out += pad + "- slot: true\n";
                break;
            }
            case DirectiveKind::ESlot: {
                auto *v = static_cast<DESlot*>(d);
                out += pad + "- eslot: " + v->name + "\n";
                out += pad + "  slot: " + (v->slot ? "true" : "false") + "\n";
                if (v->entry.length() > 0) {
                    out += pad + "  entry:\n";
                    out += toYAML(v->entry, indent + 4);
                }
                if (v->escape.length() > 0) {
                    out += pad + "  escape:\n";
                    out += toYAML(v->escape, indent + 4);
                }
                break;
            }
            case DirectiveKind::NoESlot: {
                auto *v = static_cast<DNoESlot*>(d);
                if (!v->enabled) {
                    out += pad + "- noeslot: false\n";
                } else if (v->exceptions.length() == 0) {
                    out += pad + "- noeslot: true\n";
                } else if (v->exceptions.length() == 1) {
                    out += pad + "- noeslot: " + v->exceptions[0] + "\n";
                } else {
                    out += pad + "- noeslot:\n";
                    for (size_t i = 0; i < v->exceptions.length(); ++i) {
                        out += pad + "    - " + v->exceptions[i] + "\n";
                    }
                }
                break;
            }
            case DirectiveKind::Entry: {
                auto *v = static_cast<DEntry*>(d);
                out += pad + "- entry: " + v->name + "\n";
                if (v->inject.length() > 0) {
                    out += pad + "  inject:\n";
                    out += toYAML(v->inject, indent + 4);
                }
                break;
            }
            case DirectiveKind::Bin: {
                auto *v = static_cast<DBin*>(d);
                out += pad + "- bin:\n";
                out += pad + "    name: " + v->name + "\n";
                if (!v->description.isEmpty()) out += pad + "    description: " + v->description + "\n";
                if (!v->icon.isEmpty())        out += pad + "    icon: " + v->icon + "\n";
                if (v->entry.length() > 0) {
                    out += pad + "    entry:\n";
                    out += toYAML(v->entry, indent + 8);
                }
                break;
            }
            case DirectiveKind::BinDir: {
                auto *v = static_cast<DBinDir*>(d);
                out += pad + "- bindir: " + v->path + "\n";
                break;
            }
            default: break;
        }
    }
    return out;
}

} // namespace Tau
