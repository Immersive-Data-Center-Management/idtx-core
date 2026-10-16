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
     * @brief Outcome of a SetActive() request.
     */
    enum class SetActiveStatus
    {
        Ok,             ///< The flag was authored; a broadcast should follow.
        AlreadyInState, ///< The prim already had the requested active state.
        InvalidPath,    ///< @p prim_path is empty or not a valid prim path.
        NotFound,       ///< No prim is defined at @p prim_path.
        NotDeletable,   ///< The prim is the pseudo-root, or is introduced across
                        ///< a reference/payload or defined in a sublayer, so it
                        ///< cannot be deleted via a session-layer opinion.
        Failed          ///< USD rejected the authoring call.
    };

    /**
     * @brief Set the active state of the prim at @p prim_path (soft delete when
     *        @p active is false, undo when true).
     *
     * Must be called from the session's command consumer, with the stage's edit
     * target set to the session layer. Authoring `active=false` is the only
     * composition-safe way to remove a prim a weaker layer defines; it prunes
     * the prim and its subtree from the composed stage. The prim itself remains
     * retrievable via GetPrimAtPath (inactive), which is why callers must gate
     * the resulting resync notice with AuthoringContext::setting_active.
     *
     * When @p active is false the prim must be deletable: it must exist, must
     * not be the pseudo-root, and every opinion contributing to it must live on
     * the stage's root layer or this session's layer (never a sublayer or a
     * layer pulled in across a reference/payload). This covers both a
     * root-authored prim and a prim created earlier in this session (whose only
     * `def` is on the session layer). When @p active is true (undo) only
     * existence is required.
     *
     * @return The granular outcome; never throws.
     */
    static SetActiveStatus SetActive(const pxr::UsdStageRefPtr& stage,
                                     const std::string& prim_path,
                                     bool active);

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

    /**
     * @brief Fill @p out with the schema fallback geometry attributes for
     *        @p type, read from the USD schema registry rather than a live
     *        prim. Only the members that apply to @p type are set.
     *
     * The unified Sdf-based join snapshot describes session-created prims
     * without composing a stage, so it cannot read fallbacks off a UsdPrim
     * (ReadAttributes). A created prim never authors its shape attributes into
     * the session layer, so its values are exactly the type's schema
     * fallbacks, which this returns stage-free.
     */
    static void ReadSchemaDefaults(idtxcore::PrimType type, CreatedPrim& out);
};

} // namespace session
} // namespace idtx
