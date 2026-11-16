/**
 * @file ReferencePathResolver.h
 * @brief Validate and re-anchor a client-supplied USD reference/payload asset
 *        path for a CreatePrimRequest.
 *
 * A client that asks the server to create a reference (or payload) prim sends a
 * single @c reference_path string. This helper is the single place that turns
 * that string into the three reference frames the system needs, all pointing at
 * the same on-disk asset under the uploads root:
 *
 *   - @e authored  : the absolute on-disk path of the target under the uploads
 *                    root. This is what is authored into the session sidecar
 *                    layer (the stage's edit target). The sidecar is server-only
 *                    and ephemeral, so the absolute path never leaves the server:
 *                    USD composes it directly during the live session, and the
 *                    commit re-anchors it relative to the written file. Authoring
 *                    it absolute (rather than sidecar-relative) avoids the
 *                    cross-drive case where no relative path exists.
 *   - @e broadcast : relative to the stage's root layer (the shared USD file all
 *                    clients hold). This is what the server broadcasts, so each
 *                    client can re-anchor it to its own edit layer.
 *
 * Both are emitted with POSIX separators and an explicit `./` or `../` anchor so
 * USD never mistakes them for search-path-anchored asset paths.
 *
 * The accepted string format mirrors USD's native reference syntax minus the
 * `@`-quotes: an asset path optionally followed by an angle-bracketed absolute
 * prim path that targets a specific prim inside the referenced layer, e.g.
 * `./kit/chair.usd</Geom/Seat>`. An empty suffix references the layer's
 * defaultPrim.
 */
#pragma once

#include <filesystem>
#include <string>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>

#include <idtx/utils/Logger.h>

namespace idtx
{
namespace session
{

class ReferencePathResolver
{
    IDTX_LOG_CATEGORY("ReferencePathResolver")

public:
    /** @brief Outcome of resolving a client @c reference_path. */
    enum class Status
    {
        Ok,
        EmptyPath,            ///< The asset-path portion was empty.
        SearchPathRejected,   ///< A bare `folder/file.usd` search-path (unsupported).
        InvalidPrimPath,      ///< The `<...>` suffix was not a valid absolute prim path.
        OutsideRoot,          ///< The resolved target escapes the uploads root.
        LayerUnreadable,      ///< The target could not be opened as a USD layer
                              ///< (missing, not a USD file, or unparseable).
        SelfReference,        ///< The target is the session's own root layer file.
        NoDefaultPrim,        ///< No `<prim>` suffix and the layer has no usable defaultPrim.
        PrimNotFound,         ///< The target prim does not exist in the referenced layer.
    };

    /** @brief The re-anchored forms of a validated reference. */
    struct Resolved
    {
        std::string  authored_asset;  ///< Absolute on-disk asset path, NO prim suffix, '/' sep.
        pxr::SdfPath prim_path;       ///< Target prim inside the asset (empty = defaultPrim).
        std::string  broadcast_path;  ///< Root-layer-relative wire form, `<prim>` suffix kept, '/' sep.
    };

    /**
     * @brief Resolve and validate @p reference_path.
     *
     * Beyond the server's path policy (uploads-root anchoring, the search-path
     * rejection and the uploads-root escape guard), the target is opened with
     * @c SdfLayer::FindOrOpen — proving it is an accessible, parseable USD layer
     * — and the effective target prim (the `<prim>` suffix, or the layer's
     * defaultPrim when none is given) is checked to actually exist in that layer.
     *
     * @param reference_path Raw client string (asset path + optional `<prim>`).
     * @param uploads_root   Canonical-able uploads root ("root download folder").
     * @param root_layer     The open stage's root layer. Supplies the anchor
     *                       directory for `./`/`../` asset paths and the identity
     *                       used by the self-reference guard. Must be non-null.
     * @param out            Filled on Status::Ok.
     * @return The granular outcome; never throws.
     */
    static Status Resolve(const std::string& reference_path,
                          const std::filesystem::path& uploads_root,
                          const pxr::SdfLayerHandle& root_layer,
                          Resolved& out);

    /**
     * @brief Re-express a sidecar-authored asset path as a root-layer-relative
     *        broadcast path. Used by the join snapshot, which reads the authored
     *        SdfReference/SdfPayload straight off the session layer.
     *
     * @param authored_asset_path Absolute on-disk asset path as authored in the
     *                            sidecar (no `<prim>`; see Resolve).
     * @param root_layer_dir      Directory of the stage's root layer.
     */
    static std::string AuthoredToBroadcast(const std::string& authored_asset_path,
                                           const std::filesystem::path& root_layer_dir);

    /**
     * @brief Join an asset path and an (optional) prim path into the single
     *        wire string `asset</Prim>` (no suffix when @p prim_path is empty).
     */
    static std::string JoinAssetAndPrim(const std::string& asset_path,
                                        const pxr::SdfPath& prim_path);

    /**
     * @brief Express @p target relative to directory @p base as a POSIX-separated
     *        path with an explicit `./` or `../` anchor, so USD treats it as a
     *        layer-relative asset path and never as a search path. Both inputs are
     *        normalised first. Returns "" when there is no relative relation
     *        (e.g. different filesystem roots). Shared by the resolver and the
     *        commit-time asset-path re-anchoring in StageCommitter.
     */
    static std::string ToAnchoredRelative(const std::filesystem::path& target,
                                          const std::filesystem::path& base);

    static const char* StatusToString(Status s) noexcept;
};

} // namespace session
} // namespace idtx
