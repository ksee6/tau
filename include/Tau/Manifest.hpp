/**
 * @file Manifest.hpp
 * @brief Tau manifest parsing: YAML → typed DirectiveList.
 *
 * A manifest is a YAML file whose top-level value is a sequence (list).
 * Each element of the list is a directive.  Nested directives appear as
 * child sequences inside certain directive keys (waitexit, or, and, …).
 *
 * parseManifest() is the primary entry point.  It resolves `manifest:`
 * includes recursively (cycle detection included) and expands `slot:`
 * against template manifests supplied by the caller.
 */

#pragma once

#include "Directives.hpp"
#include <Collection/String.hpp>
#include <Collection/Array.hpp>
#include <Collection/Map.hpp>
#include <Collection/Tree.hpp>

namespace Tau {

using namespace Collection;
using namespace Xi;

// ─── Manifest metadata (populated from metadata directives) ───────────────────
struct ManifestMeta {
    String name;
    String description;
    String version;
    String website;
    String issues;
    String license;
    String languages;
    long long stars = 0;
};

// ─── Parsed manifest ──────────────────────────────────────────────────────────
struct ParsedManifest {
    DirectiveList directives;   ///< Top-level directive list (block 1)
    ManifestMeta  meta;

    ParsedManifest() = default;
    ~ParsedManifest() { for (auto *d : directives) delete d; }

    // Non-copyable (owns the directive tree)
    ParsedManifest(const ParsedManifest &) = delete;
    ParsedManifest &operator=(const ParsedManifest &) = delete;

    ParsedManifest(ParsedManifest &&o) noexcept
        : directives(Xi::Move(o.directives)), meta(Xi::Move(o.meta)) {}

    ParsedManifest &operator=(ParsedManifest &&o) noexcept {
        if (this != &o) {
            for (auto *d : directives) delete d;
            directives = Xi::Move(o.directives);
            meta       = Xi::Move(o.meta);
        }
        return *this;
    }
};

// ─── Parse error ──────────────────────────────────────────────────────────────
struct ParseError {
    String message;
    int    line = -1;
    bool   ok   = true;  ///< true = no error
};

// ─── Parser ───────────────────────────────────────────────────────────────────
class Manifest {
public:
    /**
     * @brief Parse a manifest YAML string into a directive list.
     *
     * @param yaml        The manifest content (UTF-8 YAML).
     * @param basePath    Directory of the manifest file (for resolving
     *                    relative paths in manifest: includes).
     * @param visited     Set of already-visited absolute paths (cycle guard).
     * @param err         Populated on parse failure.
     * @return Parsed manifest, or empty directives list if err.ok == false.
     */
    static ParsedManifest parse(const String &yaml,
                                const String &basePath,
                                Array<String> &visited,
                                ParseError    &err);

    /**
     * @brief Load and parse a manifest from a file path.
     *
     * Resolves `manifest:` includes relative to the file's directory.
     * Detects cycles via the visited set.
     */
    static ParsedManifest load(const String &path,
                               Array<String> &visited,
                               ParseError    &err);

    /**
     * @brief Apply slot substitution.
     *
     * Given a list of template manifests and a final manifest, inject the
     * final manifest's directives into the first `slot:` directive found
     * in the nearest (closest-to-final) template that contains one.
     *
     * If no slot is found, the final manifest's directives are appended
     * to the last template's directive list.
     *
     * @param templates   Ordered list of template ParsedManifests (by
     *                    the order they appear on the `tau run` command line).
     * @param final_      The "real" manifest to inject.
     * @return            A merged DirectiveList (caller takes ownership).
     */
    static DirectiveList applySlots(Array<ParsedManifest *> &templates,
                                    ParsedManifest          &final_);

    /**
     * @brief Interpolate $VarName references in a string.
     *
     * Variables come from the runner's current environment map.
     */
    static String interpolate(const String &s,
                              Map<String, String> &vars);
    static String interpolate(const String &s,
                              const Map<String, String> &vars);

    static void resolveVariables(DirectiveList &directives,
                                 Map<String, String> &vars);

    static DirectiveList cloneDirectives(const DirectiveList &src);

    static String toYAML(const DirectiveList &directives, int indent = 0);



    static double parseDuration(const String &s);   ///< "1.5s" → 1.5
    static size_t parseSize(const String &s);        ///< "15M" → 15728640

private:
    // ─── Internal YAML-to-directive conversion ───────────────────────────────
    static Directive *nodeToDirective(Collection::NodeBase *node,
                                      const String &basePath,
                                      Array<String> &visited,
                                      ParseError    &err);

    static DirectiveList nodeListToDirectives(Collection::NodeBase *list,
                                              const String &basePath,
                                              Array<String> &visited,
                                              ParseError    &err);

    static VarOp  parseVarOp(const String &expr,
                              String &key,
                              String &value);

    static String childString(Collection::NodeBase *node, const String &key,
                              const String &def = "");
    static bool   childBool  (Collection::NodeBase *node, const String &key,
                              bool def = false);
};

} // namespace Tau
