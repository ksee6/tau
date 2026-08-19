/**
 * @file Docker.cpp
 * @brief OCI / Docker Registry v2 image resolution, layer caching, and OverlayFS mounting.
 */

#include <Tau/Docker.hpp>
#include <Tau/Config.hpp>
#include <Tau/Util.hpp>
#include <Tau/Namespace.hpp>

#include <Resource/File.hpp>
#include <Security/Crypto.hpp>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>

namespace Tau {

using namespace Collection;

// ─── parseImage ───────────────────────────────────────────────────────────────

bool Docker::parseImage(const String &source, String &registry, String &repository, String &tag) {
    String src = source.trim();
    if (src.startsWith("docker://")) src = src.substring(9);
    else if (src.startsWith("docker.io/")) src = src.substring(10);

    // Extract tag
    long long colon = src.find(":");
    if (colon >= 0) {
        tag = src.substring((size_t)colon + 1);
        src = src.substring(0, (size_t)colon);
    } else {
        tag = "latest";
    }

    // Extract registry vs repository
    long long firstSlash = src.find("/");
    if (firstSlash >= 0) {
        String firstPart = src.substring(0, (size_t)firstSlash);
        if (firstPart.find(".") >= 0 || firstPart.find(":") >= 0 || firstPart == "localhost") {
            registry   = firstPart;
            repository = src.substring((size_t)firstSlash + 1);
            return true;
        }
    }

    // Default to Docker Hub
    registry = "registry-1.docker.io";
    if (src.find("/") < 0) {
        repository = "library/" + src;
    } else {
        repository = src;
    }
    return true;
}

// ─── getAuthToken ─────────────────────────────────────────────────────────────

String Docker::getAuthToken(const String &registry, const String &repository) {
    String authUrl;
    if (registry == "registry-1.docker.io") {
        authUrl = "https://auth.docker.io/token?service=registry.docker.io&scope=repository:" + repository + ":pull";
    } else if (registry == "ghcr.io") {
        authUrl = "https://ghcr.io/token?service=ghcr.io&scope=repository:" + repository + ":pull";
    } else {
        authUrl = "https://" + registry + "/v2/token?service=" + registry + "&scope=repository:" + repository + ":pull";
    }

    String cmd = "curl -s -f -L --max-time 15 '" + authUrl + "' 2>/dev/null";
    FILE *fp = ::popen(cmd.c_str(), "r");
    if (!fp) return "";

    char buf[4096];
    String resp;
    while (::fgets(buf, sizeof(buf), fp)) resp += String(buf);
    ::pclose(fp);

    // Parse "token":"..." or "access_token":"..."
    long long tpos = resp.find("\"token\":");
    if (tpos < 0) tpos = resp.find("\"access_token\":");
    if (tpos < 0) return "";

    long long q1 = resp.find("\"", (size_t)tpos + 8);
    if (q1 < 0) return "";
    long long q2 = resp.find("\"", (size_t)q1 + 1);
    if (q2 < 0) return "";

    return resp.substring((size_t)q1 + 1, (size_t)q2);
}

// ─── fetchManifest ────────────────────────────────────────────────────────────

bool Docker::fetchManifest(const String &registry,
                           const String &repository,
                           const String &tag,
                           const String &token,
                           Array<String> &layerDigests) {
    String cacheKey = "docker_" + hexEncode(Security::hash(registry + "/" + repository + ":" + tag, 8));
    String cachePath = Config::githubCachePath() + "/" + cacheKey + ".json";

    Resource::LinuxFS fs;
    String resp;
    if (pathExists(cachePath)) {
        resp = fs.read(cachePath);
    }

    if (resp.isEmpty()) {
        String manifestUrl = "https://" + registry + "/v2/" + repository + "/manifests/" + tag;

        String cmd = "curl -s -f -L --max-time 20";
        cmd += " -H 'Accept: application/vnd.docker.distribution.manifest.v2+json, application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.index.v1+json'";
        if (!token.isEmpty()) {
            cmd += " -H 'Authorization: Bearer " + token + "'";
        }
        cmd += " '" + manifestUrl + "' 2>/dev/null";

        FILE *fp = ::popen(cmd.c_str(), "r");
        if (fp) {
            char buf[4096];
            while (::fgets(buf, sizeof(buf), fp)) resp += String(buf);
            ::pclose(fp);
        }

        if (!resp.isEmpty()) {
            mkdirP(Config::githubCachePath());
            fs.write(cachePath, resp);
        }
    }

    if (resp.isEmpty() && pathExists(cachePath)) {
        resp = fs.read(cachePath);
    }

    if (resp.isEmpty()) return false;

    // Check if multi-arch manifest list / index
    if (resp.find("\"manifests\":") >= 0) {
        // Look for linux and amd64 / x86_64 platform digest
        long long manPos = resp.find("\"manifests\":");
        String mlist = resp.substring((size_t)manPos);
        
        // Scan for amd64 architecture digest
        long long amdPos = mlist.find("\"architecture\":\"amd64\"");
        if (amdPos < 0) amdPos = mlist.find("\"architecture\": \"amd64\"");
        
        String childDigest;
        if (amdPos >= 0) {
            long long objStart = (amdPos > 500) ? (amdPos - 500) : 0;
            String chunk = mlist.substring((size_t)objStart, (size_t)amdPos + 300);
            long long dpos = chunk.find("\"digest\":");
            if (dpos >= 0) {
                long long q1 = chunk.find("\"", (size_t)dpos + 9);
                long long q2 = (q1 >= 0) ? chunk.find("\"", (size_t)q1 + 1) : -1;
                if (q1 >= 0 && q2 >= 0) {
                    childDigest = chunk.substring((size_t)q1 + 1, (size_t)q2);
                }
            }
        }

        // If child digest resolved, fetch specific manifest
        if (!childDigest.isEmpty()) {
            return fetchManifest(registry, repository, childDigest, token, layerDigests);
        }
    }

    // Parse layers array: "digest": "sha256:..."
    long long layersPos = resp.find("\"layers\":");
    if (layersPos < 0) layersPos = resp.find("\"fsLayers\":");
    if (layersPos < 0) return false;

    String layersStr = resp.substring((size_t)layersPos);
    size_t cursor = 0;
    while (cursor < layersStr.length()) {
        long long dpos = layersStr.find("\"digest\":", cursor);
        if (dpos < 0) dpos = layersStr.find("\"blobSum\":", cursor);
        if (dpos < 0) break;

        long long q1 = layersStr.find("\"", (size_t)dpos + 9);
        long long q2 = (q1 >= 0) ? layersStr.find("\"", (size_t)q1 + 1) : -1;
        if (q1 >= 0 && q2 >= 0) {
            String digest = layersStr.substring((size_t)q1 + 1, (size_t)q2);
            layerDigests.push(digest);
            cursor = (size_t)q2 + 1;
        } else {
            break;
        }
    }

    return layerDigests.length() > 0;
}

// ─── downloadAndExtractLayer ──────────────────────────────────────────────────

bool Docker::downloadAndExtractLayer(const String &registry,
                                    const String &repository,
                                    const String &digest,
                                    const String &token,
                                    const String &destDir) {
    String completeMarker = destDir + "/.complete";
    if (pathExists(completeMarker)) {
        return true; // Already cached and extracted
    }

    mkdirP(destDir);

    String blobUrl = "https://" + registry + "/v2/" + repository + "/blobs/" + digest;
    String tmpBlob = destDir + "/layer.tar.gz";

    String dlCmd = "curl -s -f -L --max-time 180";
    if (!token.isEmpty()) {
        dlCmd += " -H 'Authorization: Bearer " + token + "'";
    }
    dlCmd += " -o '" + tmpBlob + "' '" + blobUrl + "' 2>/dev/null";

    if (::system(dlCmd.c_str()) != 0 || !pathExists(tmpBlob)) {
        ::unlink(tmpBlob.c_str());
        return false;
    }

    // Extract tarball into destDir
    String extractCmd = "tar -xzf '" + tmpBlob + "' -C '" + destDir + "' 2>/dev/null || tar -xf '" + tmpBlob + "' -C '" + destDir + "' 2>/dev/null";
    (void)::system(extractCmd.c_str());
    ::unlink(tmpBlob.c_str());

    // Create completion marker and protect against accidental modifications
    Resource::LinuxFS fs;
    fs.write(completeMarker, "1\n");
    (void)::system(("chmod -R a-w '" + destDir + "' 2>/dev/null").c_str());

    return true;
}

// ─── pullAndMount ─────────────────────────────────────────────────────────────

bool Docker::pullAndMount(const DDocker &d,
                          const String &globalStorePath,
                          bool /*isStoreMode*/,
                          Array<String> &outLayers) {
    String imageName = !d.image.isEmpty() ? d.image : d.source;
    if (imageName.isEmpty() || d.target.isEmpty()) return false;

    String registry, repository, tag;
    if (!parseImage(imageName, registry, repository, tag)) {
        logError("tau: docker: failed to parse image reference: ", imageName);
        return false;
    }

    logInfo("tau: [docker] resolving image ", imageName, " (", registry, "/", repository, ":", tag, ")");

    String userStore = Config::storePath();
    String targetStore = userStore;
    mkdirP(targetStore);

    String token = getAuthToken(registry, repository);
    Array<String> layerDigests;
    if (!fetchManifest(registry, repository, tag, token, layerDigests)) {
        DIR *dir = ::opendir(userStore.c_str());
        if (dir) {
            struct dirent *ent;
            while ((ent = ::readdir(dir)) != nullptr) {
                String name(ent->d_name);
                if (name.startsWith("oci_layer_") && pathExists(userStore + "/" + name + "/.complete")) {
                    layerDigests.push(name.substring(10));
                }
            }
            ::closedir(dir);
        }
        if (layerDigests.length() == 0) {
            logError("tau: [docker] failed to fetch manifest for ", imageName);
            return false;
        }
        logInfo("tau: [docker] offline/cached fallback: using ", intStr(layerDigests.length()), " layers for ", imageName);
    }

    logInfo("tau: [docker] found ", intStr(layerDigests.length()), " layers for ", imageName);

    Array<String> layerPaths;
    for (size_t i = 0; i < layerDigests.length(); ++i) {
        String digest = layerDigests[i];
        String digestKey = digest;
        long long col = digestKey.find(":");
        if (col >= 0) digestKey = digestKey.substring((size_t)col + 1);

        String layerName = "oci_layer_" + digestKey;
        String globalLayerDir = (!globalStorePath.isEmpty()) ? (globalStorePath + "/" + layerName) : "";
        String userLayerDir = userStore + "/" + layerName;

        String chosenDir;
        if (!globalLayerDir.isEmpty() && pathExists(globalLayerDir + "/.complete")) {
            chosenDir = globalLayerDir;
        } else if (pathExists(userLayerDir + "/.complete")) {
            chosenDir = userLayerDir;
        } else {
            chosenDir = userLayerDir;
            logInfo("tau: [docker] downloading layer ", intStr(i + 1), "/", intStr(layerDigests.length()), " (", digestKey.substring(0, 12), ")");
            if (!downloadAndExtractLayer(registry, repository, digest, token, chosenDir)) {
                logError("tau: [docker] failed to download layer ", digest);
                return false;
            }
        }
        layerPaths.push(chosenDir);
        outLayers.push(chosenDir);
    }

    // Build multi-lowerdirs string in reverse order (top to bottom)
    String lowerdirs;
    for (long long i = (long long)layerPaths.length() - 1; i >= 0; --i) {
        if (!lowerdirs.isEmpty()) lowerdirs += ":";
        lowerdirs += layerPaths[(size_t)i];
    }

    mkdirP(d.target);

    String upperDir = d.source;
    String workDir  = d.work;

    bool ok = Namespace::doMount(lowerdirs, workDir, d.target, d.read, d.write, upperDir);
    if (!ok) {
        logError("tau: [docker] failed to mount rootfs on ", d.target);
        return false;
    }

    logInfo("tau: [docker] mounted ", imageName, " (", intStr(layerPaths.length()), " layers) -> ", d.target);
    return true;
}

} // namespace Tau
