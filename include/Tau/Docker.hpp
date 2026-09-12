/**
 * @file Docker.hpp
 * @brief OCI / Docker Registry v2 image resolution, layer caching, and OverlayFS mounting.
 */

#pragma once

#include <Tau/Directives.hpp>
#include <Xi/String.hpp>
#include <Xi/Array.hpp>

namespace Tau {

using namespace Xi;

class Docker {
public:
    /**
     * @brief Parse a Docker / OCI image reference into registry, repository, and tag.
     * @example "alpine:latest" -> registry: "registry-1.docker.io", repo: "library/alpine", tag: "latest"
     * @example "ghcr.io/org/app:v1" -> registry: "ghcr.io", repo: "org/app", tag: "v1"
     */
    static bool parseImage(const String &source, String &registry, String &repository, String &tag);

    /**
     * @brief Pull Docker / OCI layers, unpack to content store, and mount OverlayFS on target.
     * @param d The DDocker directive.
     * @param globalStorePath The default /var/tau/store path.
     * @param isStoreMode True if executing in tau-store.
     * @param outLayers Populated with paths to unpacked layer directories.
     * @return True if image layers were resolved and mounted successfully.
     */
    static bool pullAndMount(const DDocker &d,
                             const String &globalStorePath,
                             bool isStoreMode,
                             Array<String> &outLayers);

    /**
     * @brief Retrieve auth token for Docker Registry v2.
     */
    static String getAuthToken(const String &registry, const String &repository);

    /**
     * @brief Query manifest from registry and extract layer digests in order (bottom to top).
     */
    static bool fetchManifest(const String &registry,
                              const String &repository,
                              const String &tag,
                              const String &token,
                              Array<String> &layerDigests);

    /**
     * @brief Download a single layer blob and extract into store layer directory.
     */
    static bool downloadAndExtractLayer(const String &registry,
                                        const String &repository,
                                        const String &digest,
                                        const String &token,
                                        const String &destDir);
};

} // namespace Tau
