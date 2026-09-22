/**
 * @file StageCommitter.h
 * @brief Merge a session's sidecar (session-layer) overrides back into the
 *        original on-disk USD file while preserving composition arcs.
 *
 * The server authors all client edits into the stage's session layer (a named
 * sidecar file). Committing means folding those opinions into the original
 * file so they survive beyond the session. We must NOT use
 * @c UsdStage::Export() / @c UsdStage::Flatten(): those follow references and
 * payloads and bake every external asset inline, destroying the composition
 * graph. Instead we use @c pxr::UsdUtilsFlattenLayerStack(), which merges the
 * layer-stack opinions (session/sidecar layer over the root layer stack) while
 * leaving references, payloads, inherits, specializes and variants intact.
 *
 * For flat files (.usd/.usda/.usdc) the merged layer is exported straight over
 * the original path. For .usdz packages — which are zip archives that cannot be
 * patched in place — the package is unpacked, the root layer entry is replaced
 * with the merged layer, and the archive is re-zipped in its original entry
 * order (root layer first, per the usdz spec).
 */
#pragma once

#include <string>

#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>

#include <idtx/utils/Logger.h>

namespace idtx
{
namespace session
{

class StageCommitter
{
    IDTX_LOG_CATEGORY("StageCommitter")

public:
    /**
     * @brief Commit the composed layer-stack opinions of @p stage into the
     *        original file at @p original_path, dispatching to the flat-file or
     *        usdz strategy based on the file extension.
     * @param stage          Live server-side stage (sidecar session layer over root).
     * @param original_path  Absolute path of the original USD file to overwrite.
     * @param out_error      Human-readable failure description on false.
     * @return true on success; false (with @p out_error set) on any failure.
     *         The original file is only replaced atomically once the write has
     *         fully succeeded, so a failed commit never leaves a truncated file.
     */
    static bool Commit(const pxr::UsdStageRefPtr& stage,
                       const std::string& original_path,
                       std::string& out_error);

private:
    static bool CommitFlat(const pxr::UsdStageRefPtr& stage,
                           const std::string& original_path,
                           std::string& out_error);

    static bool CommitUsdz(const pxr::UsdStageRefPtr& stage,
                           const std::string& original_path,
                           std::string& out_error);
};

} // namespace session
} // namespace idtx
