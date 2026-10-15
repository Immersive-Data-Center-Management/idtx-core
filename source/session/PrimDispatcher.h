/**
 * @file PrimDispatcher.h
 * @brief Author built-in USD gprims requested by collaboration clients.
 *
 * This is the single place that turns a CreatePrimRequest's type + parent path
 * into USD authoring operations (UsdGeomCube/Sphere/Cone/Cylinder/Capsule) on
 * the session's stage. Keeping it isolated mirrors TransformDispatcher and
 * keeps SessionManager free of USD schema specifics.
 */
#pragma once

#include <string>

#include <pxr/pxr.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include <idtx/utils/Logger.h>

#include <idtx/proto/prim.pb.h>

namespace idtx
{
namespace session
{

/**
 * @brief Result of a successful prim creation: the resolved name/path and the
 *        default attribute values authored for the type.
 *
 * Only the attribute members flagged present apply to the created type:
 * Cube -> size; Sphere -> radius; Cone/Cylinder/Capsule -> height, radius,
 * axis. The SessionManager copies the present members into the outgoing
 * CreatePrimBroadcast.
 */
struct CreatedPrim
{
    std::string name;       ///< Server-assigned unique child name.
    std::string prim_path;  ///< Absolute path of the created prim.

    bool        has_size = false;
    double      size = 0.0;
    bool        has_height = false;
    double      height = 0.0;
    bool        has_radius = false;
    double      radius = 0.0;
    bool        has_axis = false;
    std::string axis;
};

class PrimDispatcher
{
    IDTX_LOG_CATEGORY("PrimDispatcher")

public:
    /**
     * @brief Create a built-in primitive of @p type under @p parent_path.
     *
     * Must be called from the session's command consumer, which has exclusive
     * access to the stage, with the stage's edit target set to the session
     * layer. Authors inside an SdfChangeBlock so a single notice fires.
     *
     * Validation (all checked before anything is authored, so a failure leaves
     * the stage untouched):
     *   - @p parent_path is a valid, existing prim path (the absolute root
     *     "/" is accepted);
     *   - neither the parent nor any of its ancestors is a UsdGeomGprim, since
     *     USD forbids nesting gprims;
     *   - @p type is a known built-in type.
     *
     * @param out On success, receives the resolved name/path and the authored
     *            default attribute values.
     * @return true if the prim was created, false otherwise. Never throws.
     */
    static bool Create(const pxr::UsdStageRefPtr& stage,
                       const std::string& parent_path,
                       idtxcore::PrimType type,
                       CreatedPrim& out);

    /**
     * @brief Map a USD prim type name (e.g. "Cube") to its PrimType, or
     *        PRIM_TYPE_UNSPECIFIED when it is not one of the built-ins this
     *        dispatcher creates. Used by the join snapshot to recognize prims
     *        that late joiners must re-create.
     */
    static idtxcore::PrimType TypeFromName(const std::string& type_name);

    /**
     * @brief Read the default geometry attributes of an existing @p prim of
     *        @p type into @p out (name/path untouched). Only the members that
     *        apply to @p type are set. Used by the join snapshot to describe a
     *        previously created prim.
     */
    static void ReadAttributes(const pxr::UsdPrim& prim,
                               idtxcore::PrimType type,
                               CreatedPrim& out);
};

} // namespace session
} // namespace idtx
