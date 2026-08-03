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
     * The caller MUST hold the session's stage_mutex.
     *
     * @return true if the change was applied successfully, false if the prim
     *         could not be located or the message was malformed. The function
     *         never throws.
     */
    static bool Apply(const pxr::UsdStageRefPtr& stage,
                      const idtxcore::TransformUpdate& upd);
};

} // namespace session
} // namespace idtx