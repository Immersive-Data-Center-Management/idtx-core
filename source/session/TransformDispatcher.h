/**
 * @file TransformDispatcher.h
 * @brief Apply idtxcore::TransformUpdate messages onto a UsdStage.
 *
 * This is the single place that translates a domain protobuf message into
 * USD authoring operations on a prim's UsdGeomXformable. Keeping this
 * isolated keeps SessionManager free of USD-specific composition logic and
 * makes the dispatcher easy to unit-test against in-memory stages.
 */
#pragma once

#include <pxr/pxr.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/stage.h>

#include <idtx/utils/Logger.h>

#include <idtx/proto/base.pb.h>
#include <idtx/proto/transform.pb.h>

namespace idtx
{
namespace session
{

class TransformDispatcher
{
    IDTX_LOG_CATEGORY("TransformDispatcher")

public:
    /**
     * @brief Apply a TransformUpdate to the given stage.
     *
     * Must be called from the session's command consumer, which has
     * exclusive access to the stage.
     *
     * @return true if the change was applied successfully, false if the prim
     *         could not be located, the message was malformed or an existing
     *         xform op attribute has a different precision. Everything is
     *         checked before the prim is changed, so a failed update leaves
     *         the prim untouched. The function never throws.
     */
    static bool Apply(const pxr::UsdStageRefPtr& stage,
                      const idtxcore::TransformUpdate& upd);

    /**
     * @brief Read the local transform authored on @p prim_spec straight from
     *        the layer, without composing a stage.
     *
     * Apply() always resets the op stack and authors one canonical shape:
     * either translate + rotateXYZ + scale, or a single transform matrix. This
     * reader mirrors that exact shape, so for any prim the dispatcher ever
     * moved it reconstructs the same transform the composed stage would resolve
     * — which lets the join snapshot describe prims (including the pruned
     * children of a soft-deleted prim) with pure Sdf reads.
     *
     * IMPORTANT: this is coupled to what Apply() authors. If Apply() changes
     * the op names, precisions, or t-r-s-vs-matrix choice, update this too (and
     * the round-trip test that pins them together).
     *
     * @return true and fills exactly one of @p out_sep / @p out_mat (selected
     *         by @p out_use_matrix) when the spec authors xform ops; false when
     *         it authors none (the client already holds the base transform).
     */
    static bool ReadAuthoredLocalTransform(const pxr::SdfPrimSpecHandle& prim_spec,
                                           idtxcore::SeparateTransform& out_sep,
                                           idtxcore::Matrix4dTransform& out_mat,
                                           bool& out_use_matrix);
};

} // namespace session
} // namespace idtx