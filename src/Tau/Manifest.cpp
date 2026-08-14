/**
 * @file Manifest.cpp
 * @brief YAML → typed DirectiveList parser for tau manifests.
 */

#include <Tau/Manifest.hpp>
#include <Tau/Config.hpp>
#include <Tau/Util.hpp>

#include <Encoding/Yaml.hpp>
#include <Encoding/Regex.hpp>
#include <Resource/File.hpp>
#include <Collection/Tree.hpp>

#include <cmath>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace Tau {

using namespace Encoding;
using namespace Collection;

// ─── YAML node helpers ────────────────────────────────────────────────────────

static String nodeStr(const NodeBase *node) {
    if (!node) return "";
    if (auto *n = dynamic_cast<const Node<String> *>(node)) return n->value;
    if (auto *n = dynamic_cast<const Node<long long> *>(node)) return intStr(n->value);
    if (auto *n = dynamic_cast<const Node<int> *>(node)) return intStr(n->value);
    if (auto *n = dynamic_cast<const Node<double> *>(node)) return doubleStr(n->value);
    if (auto *n = dynamic_cast<const Node<bool> *>(node)) return n->value ? "true" : "false";

    for (size_t i = 0; i < node->size(); ++i) {
        const NodeBase *child = (*node)[i];
        if (!child) continue;
        String res = nodeStr(child);
        if (!res.isEmpty()) return res;
    }
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

        if (item->size() >= 2 && (*item)[0] && (*item)[0]->getName() == key) {
            return (*item)[1];
        }
        if (item->size() == 1 && (*item)[0] && (*item)[0]->getName() == key) {
            return (*item)[0];
        }
    }

    for (size_t i = 0; i < node->size(); ++i) {
        NodeBase *item = (*node)[i];
        if (!item) continue;
        NodeBase *found = getDictNode(item, key);
        if (found) return found;
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

    for (size_t i = 0; i < child->size(); ++i) {
        NodeBase *c = (*child)[i];
        if (c && !c->getName().isEmpty()) {
            if (c->getName() == "https" || c->getName() == "http") {
                String proto = c->getName();
                String rest = nodeStr(c);
                while (rest.startsWith("\"")) rest = rest.substring(1);
                while (rest.endsWith("\"")) rest = rest.substring(0, rest.length() - 1);
                return proto + "://" + rest;
            } else if (c->getName() != key) {
                String sub = nodeStr(c);
                if (!sub.isEmpty()) {
                    val = c->getName() + ":" + sub;
                    break;
                }
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
    if (s.isEmpty()) return 0.0;
    String num = s;
    if (num[num.length() - 1] == 's')
        num = num.substring(0, num.length() - 1);
    return (double)parseLong(num);   // integer seconds for now
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
    auto tryOp = [&](const String &op, VarOp vop) -> bool {
        long long idx = expr.find(op);
        if (idx < 0) return false;
        key   = expr.substring(0, (size_t)idx).trim();
        value = expr.substring((size_t)idx + op.length()).trim();
        return true;
    };

    if (tryOp(" includes ", VarOp::CheckIncludes)) return VarOp::CheckIncludes;
    if (tryOp(" reg ",      VarOp::CheckRegex))    return VarOp::CheckRegex;
    if (tryOp(">=",         VarOp::CheckGTE))      return VarOp::CheckGTE;
    if (tryOp("<=",         VarOp::CheckLTE))      return VarOp::CheckLTE;
    if (tryOp("==",         VarOp::CheckEquals))   return VarOp::CheckEquals;
    if (tryOp(">",          VarOp::CheckGT))       return VarOp::CheckGT;
    if (tryOp("<",          VarOp::CheckLT))       return VarOp::CheckLT;
    if (tryOp("=",          VarOp::Assign))        return VarOp::Assign;

    key   = expr.trim();
    value = "";
    return VarOp::Assign;
}

static NodeBase *findNamedNode(NodeBase *node) {
    if (!node) return nullptr;
    if (!node->getName().isEmpty()) return node;

    for (size_t i = 0; i < node->size(); ++i) {
        NodeBase *child = (*node)[i];
        if (child && !child->getName().isEmpty()) return child;
    }

    for (size_t i = 0; i < node->size(); ++i) {
        NodeBase *found = findNamedNode((*node)[i]);
        if (found) return found;
    }
    return nullptr;
}

// ─── Single YAML node → Directive ────────────────────────────────────────────

Directive *Manifest::nodeToDirective(NodeBase *node,
                                     const String &basePath,
                                     Array<String> &visited,
                                     ParseError &err) {
    if (!node) return nullptr;

    String key = node->getName();
    if (key.isEmpty()) {
        NodeBase *named = findNamedNode(node);
        if (named) {
            key = named->getName();
            node = named;
        }
    }
    if (key.isEmpty()) {
        logError("tau/nodeToDirective: key is empty! nodeName='", node->getName(), "' nodeSize=", intStr(node->size()));
        for (size_t di = 0; di < node->size(); ++di) {
            NodeBase *ch = (*node)[di];
            logError("  child[", intStr(di), "] name='", ch ? ch->getName() : "<null>", "' size=", intStr(ch ? ch->size() : 0), " str='", nodeStr(ch), "'");
        }
        return nullptr;
    }

    logInfo("tau/nodeToDirective: parsed key='", key, "'");





    // ── wait ──────────────────────────────────────────────────────────────────
    if (key == "wait") {
        auto *d = new DWait();
        d->seconds = parseDuration(nodeStr(node));
        return d;
    }

    // ── logic blocks ──────────────────────────────────────────────────────────
    auto parseBlock = [&](DLogicBlock *block) -> Directive * {
        NodeBase *target = getDictNode(node, "entry");
        if (!target) target = node;
        logInfo("tau/parseBlock: key=", key, " nodeName='", node->getName(), "' nodeSize=", intStr(node->size()), " targetName='", target->getName(), "' targetSize=", intStr(target->size()));
        block->children = nodeListToDirectives(target, basePath, visited, err);
        logInfo("tau/parseBlock: parsed children count=", intStr(block->children.length()));
        return block;
    };


    // ── waitexit ──────────────────────────────────────────────────────────────
    if (key == "waitexit") {
        auto *d = new DWaitExit();
        NodeBase *target = getDictNode(node, "entry");
        if (!target) target = node;
        d->children = nodeListToDirectives(target, basePath, visited, err);
        return d;
    }

    // ── spawn ─────────────────────────────────────────────────────────────────
    if (key == "spawn") {
        auto *d = new DSpawn();
        if (node->size() == 0 || getDictNode(node, "command") == nullptr) {
            d->command = nodeStr(node);
            d->waitForExit = true;
            logInfo("tau/manifest: simple spawn cmd='", d->command, "' nodeStr='", nodeStr(node), "' size=", intStr(node->size()));
        } else {
            d->command     = childString(node, "command");
            d->waitForExit = childBool(node, "wait", true);
            logInfo("tau/manifest: spawn cmd='", d->command, "' wait=", d->waitForExit ? "true" : "false");
            d->name        = childString(node, "name");
        }
        return d;
    }


    // ── memory ────────────────────────────────────────────────────────────────
    if (key == "memory") {
        auto *d = new DMemory();
        d->raw   = nodeStr(node);
        d->bytes = parseSize(d->raw);
        return d;
    }

    if (key == "cpuset") {
        auto *d = new DCPUSet();
        d->cpus = nodeStr(node);
        return d;
    }

    if (key == "cpu") {
        auto *d = new DCPU();
        d->quota = parseLong(nodeStr(node));
        return d;
    }

    if (key == "chroot") {
        auto *d = new DChroot();
        d->path = nodeStr(node);
        return d;
    }

    if (key == "isolate") {
        auto *d = new DIsolate();
        String val = nodeStr(node);
        d->enable = (val != "false" && val != "0");
        return d;
    }


    if (key == "veth") {
        auto *d = new DVeth();
        d->hostName = childString(node, "name");
        d->peerName = childString(node, "peer");
        return d;
    }

    if (key == "ip") {
        auto *d = new DIP();
        d->command = nodeStr(node);
        return d;
    }

    if (key == "newnet") {
        auto *d = new DNewNet();
        String val = nodeStr(node);
        if (!val.isEmpty() && val != "true" && val != "false" && val != "[]" && val != "{}") {
            Array<String> parts = val.split(",");
            for (size_t i = 0; i < parts.length(); ++i) {
                String t = parts[i].trim();
                if (!t.isEmpty() && t != "true" && t != "false" && t != "[]") d->allowList.push(t);
            }
        }
        return d;
    }

    if (key == "forward") {
        auto *d = new DForward();
        d->target     = childString(node, "target");
        d->source     = childString(node, "source");
        d->port       = childString(node, "port");
        d->sourcePort = childString(node, "source-port");
        String sip    = childString(node, "source-ip");
        if (!sip.isEmpty()) d->sourceIP = sip;
        d->ip         = childString(node, "ip");
        String proto  = childString(node, "protocol");
        if (!proto.isEmpty()) d->protocol = proto;
        return d;
    }

    if (key == "macvlan") {
        auto *d = new DMacvlan();
        d->parent = childString(node, "parent");
        d->name   = childString(node, "name");
        String m  = childString(node, "mode");
        if (!m.isEmpty()) d->mode = m;
        d->mac    = childString(node, "mac");
        return d;
    }

    if (key == "ipvlan") {
        auto *d = new DIPVlan();
        d->parent = childString(node, "parent");
        d->name   = childString(node, "name");
        String m  = childString(node, "mode");
        if (!m.isEmpty()) d->mode = m;
        return d;
    }

    if (key == "bridge") {
        auto *d = new DBridge();
        d->name    = childString(node, "name");
        d->attach  = childString(node, "attach");
        d->address = childString(node, "address");
        return d;
    }



    if (key == "mount") {
        auto *d = new DMount();
        d->source = childString(node, "source");
        d->work   = childString(node, "work");
        d->target = childString(node, "target");
        d->read   = childBool(node, "read",  true);
        d->write  = childBool(node, "write", true);
        return d;
    }

    if (key == "copy") {
        auto *d = new DCopy();
        d->source = childString(node, "source");
        d->target = childString(node, "target");
        return d;
    }

    if (key == "unlink") {
        auto *d = new DUnlink();
        d->path = nodeStr(node);
        if (d->path.isEmpty()) d->path = childString(node, "path");
        logInfo("tau/parse: unlink nodeStr='", nodeStr(node), "' size=", intStr(node->size()), " childString(path)='", childString(node, "path"), "' result='", d->path, "'");
        return d;
    }


    if (key == "mkdir") {
        auto *d = new DMkdir();
        d->path = nodeStr(node);
        if (d->path.isEmpty()) d->path = childString(node, "path");
        return d;
    }


    if (key == "symlink") {
        auto *d = new DSymlink();
        d->source = childString(node, "source");
        d->target = childString(node, "target");
        return d;
    }

    if (key == "image") {
        auto *d = new DImage();
        d->path   = childString(node, "path");
        d->size   = childString(node, "size");
        d->type   = childString(node, "type", "ext4");
        d->create = childBool(node, "create", true);
        d->source = childString(node, "source");
        d->work   = childString(node, "work");
        d->lower  = childString(node, "lower");
        d->target = childString(node, "target");
        d->read   = childBool(node, "read",  true);
        d->write  = childBool(node, "write", true);

        NodeBase *partNode = node->get("partitions");
        if (partNode) {
            for (size_t i = 0; i < partNode->size(); ++i) {
                auto *pn = (*partNode)[i];
                if (!pn) continue;
                ImagePartition p;
                p.name   = childString(pn, "name");
                p.uuid   = childString(pn, "uuid");
                p.size   = childString(pn, "size");
                p.source = childString(pn, "source", "@/");
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

    if (key == "local") {
        auto *d = new DLocal();
        d->source = childString(node, "source");
        d->target = childString(node, "target");
        d->store  = childBool(node, "store", true);
        return d;
    }


    if (key == "git") {
        auto *d = new DGit();
        d->source = childString(node, "source");
        d->branch = childString(node, "branch", "main");
        d->target = childString(node, "target");
        d->store  = childBool(node, "store", true);
        return d;
    }

    if (key == "ghrelease") {
        auto *d = new DGHRelease();
        d->source = childString(node, "source");
        d->branch = childString(node, "branch");
        d->name   = childString(node, "name");
        d->target = childString(node, "target");
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

    // ── bin ───────────────────────────────────────────────────────────────────
    if (key == "bin") {
        auto *d = new DBin();
        d->name        = childString(node, "name");
        d->description = childString(node, "description");
        d->icon        = childString(node, "icon");
        NodeBase *entryNode = node->get("entry");
        if (entryNode) d->entry = nodeListToDirectives(entryNode, basePath, visited, err);
        return d;
    }

    if (key == "bindir") {
        auto *d = new DBinDir();
        d->path = nodeStr(node);
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
        Directive *d = nodeToDirective(child, basePath, visited, err);
        logInfo("tau/nodeListToDirectives: i=", intStr((int)i), " childName='", child->getName(), "' childSize=", intStr(child->size()), " d=", d ? "OK" : "NULL");
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

    logInfo("tau/parse: root.size=", intStr(root.size()), " rootName='", root.getName(), "'");
    result.directives = nodeListToDirectives(&root, basePath, visited, err);
    logInfo("tau/parse: parsed directives count=", intStr(result.directives.length()));
    for (size_t i = 0; i < result.directives.length(); ++i) {
        logInfo("  [", intStr((int)i), "] kind=", intStr((int)result.directives[i]->kind));
    }


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
        if (dynamic_cast<DSlot *>(base[i])) {
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
                if (val) {
                    result += *val;
                } else {
                    char *ev = ::getenv(varName.c_str());
                    if (ev && *ev) {
                        String eStr(ev);
                        vars[varName] = eStr;
                        result += eStr;
                    } else {
                        result += "$" + varName;
                    }
                }
                i = j;
                continue;

        } else if (s[i] == '%' && i + 1 < len) {
            size_t j = i + 1;
            while (j < len && s[j] != '/' && s[j] != ' ' && s[j] != ':' &&
                   s[j] != '"' && s[j] != '\'' && s[j] != '`' && s[j] != ',' && s[j] != '$') {
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
                        String allocated = "/tmp/tau-" + varName;
                        if (pathExists(allocated)) {
                            removeDirRecursive(allocated);
                        }
                        vars[varName] = allocated;
                        ::setenv(varName.c_str(), allocated.c_str(), 1);
                        result += allocated;
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
            case DirectiveKind::Spawn: {
                auto *sp = static_cast<DSpawn *>(d);
                sp->command = interpolate(sp->command, vars);
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
                img->path = interpolate(img->path, vars);
                if (!img->size.isEmpty()) img->size = interpolate(img->size, vars);
                if (!img->source.isEmpty()) img->source = interpolate(img->source, vars);
                if (!img->work.isEmpty()) img->work = interpolate(img->work, vars);
                if (!img->lower.isEmpty()) img->lower = interpolate(img->lower, vars);
                if (!img->target.isEmpty()) img->target = interpolate(img->target, vars);
                for (size_t p = 0; p < img->partitions.length(); ++p) {
                    img->partitions[p].source = interpolate(img->partitions[p].source, vars);
                    img->partitions[p].work   = interpolate(img->partitions[p].work, vars);
                    img->partitions[p].lower  = interpolate(img->partitions[p].lower, vars);
                    img->partitions[p].target = interpolate(img->partitions[p].target, vars);
                }
                break;
            }
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
                v->hostName = interpolate(v->hostName, vars);
                v->peerName = interpolate(v->peerName, vars);
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
                b->name = interpolate(b->name, vars);
                b->attach = interpolate(b->attach, vars);
                b->address = interpolate(b->address, vars);
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
            case DirectiveKind::Or: case DirectiveKind::And:
            case DirectiveKind::Nand: case DirectiveKind::Nor: case DirectiveKind::Xor: {
                auto *lb = static_cast<DLogicBlock *>(d);
                resolveVariables(lb->children, vars);
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
            case DirectiveKind::Wait: {
                auto *v = static_cast<DWait*>(d);
                out += pad + "- wait: " + doubleStr(v->seconds) + "s\n";
                break;
            }
            case DirectiveKind::WaitExit: {
                auto *v = static_cast<DWaitExit*>(d);
                out += pad + "- waitexit:\n";
                out += pad + "    entry:\n";
                out += toYAML(v->children, indent + 8);
                break;
            }

            case DirectiveKind::Spawn: {
                auto *v = static_cast<DSpawn*>(d);
                out += pad + "- spawn:\n";
                String cmdEscaped = v->command.replace("\"", "\\\"");
                out += pad + "    command: \"" + cmdEscaped + "\"\n";
                out += pad + "    wait: " + (v->waitForExit ? "true\n" : "false\n");
                if (!v->name.isEmpty()) out += pad + "    name: " + v->name + "\n";
                break;
            }

            case DirectiveKind::Memory: {
                auto *v = static_cast<DMemory*>(d);
                out += pad + "- memory: " + v->raw + "\n";
                break;
            }
            case DirectiveKind::CPUSet: {
                auto *v = static_cast<DCPUSet*>(d);
                out += pad + "- cpuset: " + v->cpus + "\n";
                break;
            }
            case DirectiveKind::CPU: {
                auto *v = static_cast<DCPU*>(d);
                out += pad + "- cpu: " + intStr(v->quota) + "\n";
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

            case DirectiveKind::Veth: {
                auto *v = static_cast<DVeth*>(d);
                out += pad + "- veth:\n";
                out += pad + "    name: " + v->hostName + "\n";
                out += pad + "    peer: " + v->peerName + "\n";
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
                out += pad + "    name: " + v->name + "\n";
                if (!v->attach.isEmpty())  out += pad + "    attach: " + v->attach + "\n";
                if (!v->address.isEmpty()) out += pad + "    address: " + v->address + "\n";
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
                out += pad + "    path: " + v->path + "\n";
                if (!v->size.isEmpty())   out += pad + "    size: "   + v->size   + "\n";
                out += pad + "    type: " + v->type + "\n";
                out += pad + "    create: " + (v->create ? "true\n" : "false\n");
                if (!v->source.isEmpty()) out += pad + "    source: " + v->source + "\n";
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
                        out += pad + "        source: " + p.source + "\n";
                        if (!p.work.isEmpty())   out += pad + "        work: "   + p.work   + "\n";
                        if (!p.lower.isEmpty())  out += pad + "        lower: "  + p.lower  + "\n";
                        if (!p.target.isEmpty()) out += pad + "        target: " + p.target + "\n";
                    }
                }
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
            case DirectiveKind::Slot: {
                out += pad + "- slot: true\n";
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
