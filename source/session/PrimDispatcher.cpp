#include "PrimDispatcher.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/pcp/layerStack.h>
#include <pxr/usd/pcp/node.h>
#include <pxr/usd/pcp/primIndex.h>
#include <pxr/usd/pcp/types.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/payload.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>

#include "ReferencePathResolver.h"

namespace idtx
{
namespace session
{

namespace
{

// Single source of truth: proto PrimType -> USD schema type. The proto enum is
// our own, so this one switch is the only hand-written mapping; the base name,
// gprim classification and authoring all derive from the TfType it returns. An
// unknown TfType (UNSPECIFIED or unrecognized) is treated by callers as
// "reject".
pxr::TfType UsdSchemaType(idtxcore::PrimType type)
{
    switch (type)
    {
        case idtxcore::PRIM_TYPE_CUBE:     return pxr::TfType::Find<pxr::UsdGeomCube>();
        case idtxcore::PRIM_TYPE_SPHERE:   return pxr::TfType::Find<pxr::UsdGeomSphere>();
        case idtxcore::PRIM_TYPE_CONE:     return pxr::TfType::Find<pxr::UsdGeomCone>();
        case idtxcore::PRIM_TYPE_CYLINDER: return pxr::TfType::Find<pxr::UsdGeomCylinder>();
        case idtxcore::PRIM_TYPE_CAPSULE:  return pxr::TfType::Find<pxr::UsdGeomCapsule>();
        case idtxcore::PRIM_TYPE_PLANE:    return pxr::TfType::Find<pxr::UsdGeomPlane>();
        case idtxcore::PRIM_TYPE_XFORM:    return pxr::TfType::Find<pxr::UsdGeomXform>();
        case idtxcore::PRIM_TYPE_UNSPECIFIED:
        default:                           return pxr::TfType();
    }
}

// Base (pre-disambiguation) prim name for each supported type, taken straight
// from the USD schema registry (e.g. "Cube"). Empty for an unknown/unspecified
// type, which the caller treats as "reject".
pxr::TfToken BaseNameFor(idtxcore::PrimType type)
{
    const pxr::TfType t = UsdSchemaType(type);
    return t.IsUnknown() ? pxr::TfToken()
                         : pxr::UsdSchemaRegistry::GetSchemaTypeName(t);
}

// Whether @p type is a gprim (geometric primitive). Asks USD's type system
// whether the schema derives from UsdGeomGprim rather than hard-coding the set,
// so new built-in types are classified correctly as they are added. Gprims may
// not be nested under one another; Xform is not a gprim and has no such
// restriction.
bool IsGprimType(idtxcore::PrimType type)
{
    static const pxr::TfType gprim = pxr::TfType::Find<pxr::UsdGeomGprim>();
    const pxr::TfType t = UsdSchemaType(type);
    return !t.IsUnknown() && t.IsA(gprim);
}

// True if @p prim or any of its ancestors is a gprim. USD forbids nesting
// gprims under gprims (usdchecker warns on such scenes), so a new gprim may
// not be parented beneath one.
bool HasGprimAncestor(pxr::UsdPrim prim)
{
    for (; prim && !prim.IsPseudoRoot(); prim = prim.GetParent())
    {
        if (prim.IsA<pxr::UsdGeomGprim>()) return true;
    }
    return false;
}

// Pick a child name under @p parent that collides with no existing prim:
// the type's base name, then base_1, base_2, ... The base is sanitized into a
// valid USD identifier first.
pxr::SdfPath UniqueChildPath(const pxr::UsdStageRefPtr& stage,
                             const pxr::SdfPath& parent,
                             const std::string& base_name,
                             std::string& out_name)
{
    const std::string base = pxr::TfMakeValidIdentifier(base_name);

    std::string candidate = base;
    for (std::uint64_t n = 1;; ++n)
    {
        const pxr::SdfPath child = parent.AppendChild(pxr::TfToken(candidate));
        if (!stage->GetPrimAtPath(child))
        {
            out_name = candidate;
            return child;
        }
        candidate = base + "_" + std::to_string(n);
    }
}

// Author the typed schema at @p path. Returns false if the type is unknown or
// the schema could not be defined. DefinePrim with the registry's schema type
// name is equivalent to the typed UsdGeomXxx::Define for these concrete
// schemas. Attribute values are read back separately, after the enclosing
// SdfChangeBlock closes and the stage has recomposed.
bool AuthorPrim(const pxr::UsdStageRefPtr& stage,
                const pxr::SdfPath& path,
                idtxcore::PrimType type)
{
    const pxr::TfToken type_name = BaseNameFor(type);
    if (type_name.IsEmpty()) return false;
    return static_cast<bool>(stage->DefinePrim(path, type_name));
}

// Build the LOCAL GfMatrix4d carried by @p req. Returns false when the
// request carries no transform. A Matrix4dTransform is loaded verbatim (same
// row ordering ApplyMatrix uses); a SeparateTransform is composed scale ->
// rotateXYZ -> translate via GfTransform, matching UsdGeomXformCommonAPI's
// RotationOrderXYZ convention (verified by PrimDispatcherTests). Uses the
// per-field has_* accessors so it is independent of the oneof's name.
bool LocalMatrixFromRequest(const idtxcore::CreatePrimRequest& req, pxr::GfMatrix4d& out)
{
    if (req.has_matrix())
    {
        const auto& m = req.matrix();
        out = pxr::GfMatrix4d(
            m.m00(), m.m01(), m.m02(), m.m03(),
            m.m10(), m.m11(), m.m12(), m.m13(),
            m.m20(), m.m21(), m.m22(), m.m23(),
            m.m30(), m.m31(), m.m32(), m.m33());
        return true;
    }
    if (req.has_seperate())
    {
        const auto& s  = req.seperate();
        const auto& t  = s.translation();
        const auto& r  = s.rotation();
        const auto& sc = s.scale();

        pxr::GfTransform xf;
        xf.SetScale(pxr::GfVec3d(sc.x(), sc.y(), sc.z()));
        xf.SetRotation(pxr::GfRotation(pxr::GfVec3d::XAxis(), r.x())
                     * pxr::GfRotation(pxr::GfVec3d::YAxis(), r.y())
                     * pxr::GfRotation(pxr::GfVec3d::ZAxis(), r.z()));
        xf.SetTranslation(pxr::GfVec3d(t.x(), t.y(), t.z()));
        out = xf.GetMatrix();
        return true;
    }
    return false;
}

// Author @p local_matrix as the LOCAL transform of @p prim relative to its
// parent, verbatim. Authored as a single matrix xformOp:transform so it
// round-trips through TransformDispatcher::ReadAuthoredLocalTransform. Returns
// false if the prim is not xformable (e.g. a reference to a non-xformable
// prim); callers treat that as non-fatal.
bool AuthorLocalTransform(const pxr::UsdPrim& prim,
                          const pxr::GfMatrix4d& local_matrix)
{
    pxr::UsdGeomXformable xformable(prim);
    if (!xformable) return false;

    // No SdfChangeBlock: this is Usd-level authoring (SetXformOpOrder /
    // AddTransformOp read the composed op order), which USD advises against
    // inside a change block. The caller's create_in_progress flag already
    // suppresses the notice, so there is nothing to coalesce.
    xformable.SetXformOpOrder({}, /*resetXformStack=*/false);
    pxr::UsdGeomXformOp op = xformable.AddTransformOp(pxr::UsdGeomXformOp::PrecisionDouble);
    if (!op) return false;
    op.Set(local_matrix);
    return true;
}

// True if @p prim's existence was authored locally — on the stage's root layer
// or this session's layer — so it can be masked with a session-layer
// `active=false` and later folded into the committed root file. A prim defined
// in a sublayer, or introduced across a reference/payload authored on an
// ancestor, is NOT locally deletable: its definition is owed to a foreign
// layer we cannot fold away.
//
// The subtlety is a *reference prim* we created this session: a typeless def on
// the local layer that carries a reference or payload arc. Its composed prim
// stack includes the target's specs on the referenced layer, so a naive
// "every spec is on a local layer" test wrongly rejects it. Those foreign
// specs are the prim's own reference content and must be ignored.
//
// So the check walks the prim's composition graph (Pcp) node by node instead
// of its flattened spec stack. A node whose opinions reach the prim through a
// reference or payload authored directly on the prim (PcpNodeRef::
// IsDueToAncestor() is false) — and every node beneath it — is the prim's own
// reference content and is accepted. Every other contributing node must draw
// its opinions only from the root or session layer: a spec on a sublayer, or
// any opinion pulled across an ancestral arc (reference/payload/inherit/
// specialize/variant), makes the prim non-locally-deletable.
bool IsLocallyDeletable(const pxr::UsdPrim& prim,
                        const pxr::SdfLayerHandle& root_layer,
                        const pxr::SdfLayerHandle& session_layer)
{
    const pxr::PcpPrimIndex& index = prim.GetPrimIndex();
    if (!index.IsValid()) return false;

    for (const pxr::PcpNodeRef& node : index.GetNodeRange())
    {
        if (!node.HasSpecs()) continue;

        // Is this node reached through a reference or payload authored directly
        // on the prim (not inherited from a namespace ancestor)? Walk up the
        // graph: if any node on the path to the root is such an arc, every spec
        // here is the prim's own reference content and is fine.
        bool own_reference_content = false;
        for (pxr::PcpNodeRef n = node; n; n = n.GetParentNode())
        {
            const pxr::PcpArcType arc = n.GetArcType();
            if ((arc == pxr::PcpArcTypeReference || arc == pxr::PcpArcTypePayload)
                && !n.IsDueToAncestor())
            {
                own_reference_content = true;
                break;
            }
        }
        if (own_reference_content) continue;

        // Otherwise this node contributes a local or ancestral opinion. It is
        // acceptable only when its spec lives on the root or session layer; a
        // spec on any other layer in its layer stack (a sublayer, or a layer
        // pulled across an ancestral arc) is foreign and cannot be folded away.
        for (const pxr::SdfLayerRefPtr& layer : node.GetLayerStack()->GetLayers())
        {
            const pxr::SdfLayerHandle handle(layer);
            if (handle == root_layer || handle == session_layer) continue;
            if (layer->HasSpec(node.GetPath())) return false;
        }
    }

    // A prim with no authored specs anywhere (e.g. a pure composition artifact)
    // is not something we can delete locally.
    return !prim.GetPrimStack().empty();
}

} // namespace

idtxcore::PrimType PrimDispatcher::TypeFromName(const std::string& type_name)
{
    if (type_name == "Cube")     return idtxcore::PRIM_TYPE_CUBE;
    if (type_name == "Sphere")   return idtxcore::PRIM_TYPE_SPHERE;
    if (type_name == "Cone")     return idtxcore::PRIM_TYPE_CONE;
    if (type_name == "Cylinder") return idtxcore::PRIM_TYPE_CYLINDER;
    if (type_name == "Capsule")  return idtxcore::PRIM_TYPE_CAPSULE;
    if (type_name == "Plane")    return idtxcore::PRIM_TYPE_PLANE;
    if (type_name == "Xform")    return idtxcore::PRIM_TYPE_XFORM;
    return idtxcore::PRIM_TYPE_UNSPECIFIED;
}

void PrimDispatcher::ReadAttributes(const pxr::UsdPrim& prim,
                                    idtxcore::PrimType type,
                                    CreatedPrim& out)
{
    if (!prim) return;

    pxr::TfToken axis;
    switch (type)
    {
        case idtxcore::PRIM_TYPE_CUBE:
            out.has_size = pxr::UsdGeomCube(prim).GetSizeAttr().Get(&out.size);
            break;
        case idtxcore::PRIM_TYPE_SPHERE:
            out.has_radius = pxr::UsdGeomSphere(prim).GetRadiusAttr().Get(&out.radius);
            break;
        case idtxcore::PRIM_TYPE_CONE:
        {
            pxr::UsdGeomCone cone(prim);
            out.has_height = cone.GetHeightAttr().Get(&out.height);
            out.has_radius = cone.GetRadiusAttr().Get(&out.radius);
            if ((out.has_axis = cone.GetAxisAttr().Get(&axis))) out.axis = axis.GetString();
            break;
        }
        case idtxcore::PRIM_TYPE_CYLINDER:
        {
            pxr::UsdGeomCylinder cyl(prim);
            out.has_height = cyl.GetHeightAttr().Get(&out.height);
            out.has_radius = cyl.GetRadiusAttr().Get(&out.radius);
            if ((out.has_axis = cyl.GetAxisAttr().Get(&axis))) out.axis = axis.GetString();
            break;
        }
        case idtxcore::PRIM_TYPE_CAPSULE:
        {
            pxr::UsdGeomCapsule cap(prim);
            out.has_height = cap.GetHeightAttr().Get(&out.height);
            out.has_radius = cap.GetRadiusAttr().Get(&out.radius);
            if ((out.has_axis = cap.GetAxisAttr().Get(&axis))) out.axis = axis.GetString();
            break;
        }
        case idtxcore::PRIM_TYPE_PLANE:
        {
            pxr::UsdGeomPlane plane(prim);
            out.has_width  = plane.GetWidthAttr().Get(&out.width);
            out.has_length = plane.GetLengthAttr().Get(&out.length);
            if ((out.has_axis = plane.GetAxisAttr().Get(&axis))) out.axis = axis.GetString();
            break;
        }
        case idtxcore::PRIM_TYPE_XFORM:        // a transform group has no shape attributes
        case idtxcore::PRIM_TYPE_UNSPECIFIED:
        default:
            break;
    }
}

bool PrimDispatcher::Create(const pxr::UsdStageRefPtr& stage,
                            const idtxcore::CreatePrimRequest& req,
                            const std::string& uploads_root,
                            CreatedPrim& out)
{
    if (!stage)
    {
        IDTX_LOG(IDTX_ERROR, "PrimDispatcher::Create called with null stage.");
        return false;
    }

    const std::string& parent_path = req.parent_path();
    const idtxcore::PrimType type   = req.type();
    const bool is_reference = (type == idtxcore::PRIM_TYPE_REF);

    // Resolve the prim's base name and, for a reference prim, validate and
    // re-anchor the reference path — all before anything is authored.
    pxr::TfToken base_name;
    ReferencePathResolver::Resolved resolved;
    if (is_reference)
    {
        if (req.reference_path().empty())
        {
            IDTX_LOG(IDTX_WARN, "CreatePrimRequest of reference type without reference_path; ignoring.");
            return false;
        }

        const pxr::SdfLayerHandle root_layer = stage->GetRootLayer();
        const pxr::SdfLayerHandle edit_layer = stage->GetEditTarget().GetLayer();
        if (!root_layer || !edit_layer)
        {
            IDTX_LOG(IDTX_ERROR, "CreatePrimRequest: stage has no root/edit layer; ignoring.");
            return false;
        }
        const std::filesystem::path root_file(root_layer->GetRealPath());
        const std::filesystem::path root_dir    = root_file.parent_path();
        const std::filesystem::path sidecar_dir =
            std::filesystem::path(edit_layer->GetRealPath()).parent_path();

        const ReferencePathResolver::Status rs = ReferencePathResolver::Resolve(
            req.reference_path(), std::filesystem::path(uploads_root),
            root_dir, sidecar_dir, root_file, resolved);
        if (rs != ReferencePathResolver::Status::Ok)
        {
            IDTX_LOG(IDTX_WARN, "CreatePrimRequest reference_path '{}' rejected: {}.",
                     req.reference_path(), ReferencePathResolver::StatusToString(rs));
            return false;
        }

        const std::string stem = std::filesystem::path(resolved.authored_asset).stem().string();
        const std::string ident = pxr::TfMakeValidIdentifier(stem);
        base_name = pxr::TfToken(ident.empty() ? "Ref" : ident);
    }
    else
    {
        base_name = BaseNameFor(type);
        if (base_name.IsEmpty())
        {
            IDTX_LOG(IDTX_WARN, "CreatePrimRequest with unknown prim type {}; ignoring.",
                     static_cast<int>(type));
            return false;
        }
    }

    if (parent_path.empty() || !pxr::SdfPath::IsValidPathString(parent_path))
    {
        IDTX_LOG(IDTX_WARN, "CreatePrimRequest with invalid parent_path '{}'.", parent_path);
        return false;
    }

    const pxr::SdfPath parent(parent_path);
    if (!parent.IsAbsolutePath() || !(parent.IsPrimPath() || parent == pxr::SdfPath::AbsoluteRootPath()))
    {
        IDTX_LOG(IDTX_WARN, "CreatePrimRequest parent_path '{}' is not a prim path.", parent_path);
        return false;
    }

    const pxr::UsdPrim parent_prim = stage->GetPrimAtPath(parent);
    if (!parent_prim)
    {
        IDTX_LOG(IDTX_WARN, "CreatePrimRequest parent '{}' not found on stage; ignoring.",
                 parent_path);
        return false;
    }

    // GetPrimAtPath returns inactive prims too, so an explicit check is needed:
    // a prim that was soft-deleted (deactivated) must not accept new children,
    // as the whole subtree is pruned from composition. The pseudo-root is
    // always active, so parenting under "/" is unaffected.
    if (!parent_prim.IsActive())
    {
        IDTX_LOG(IDTX_WARN,
                 "CreatePrimRequest parent '{}' is inactive (deleted); ignoring.",
                 parent_path);
        return false;
    }

    // Gprims may not be nested under another Gprim. As references are liekly to contain GPrims
    // they are shall also never be nested under another Gprim. Xform has no such rule and
    // may be parented under any valid prim, so it skips this check.
    if ((IsGprimType(type) || is_reference)&& HasGprimAncestor(parent_prim))
    {
        IDTX_LOG(IDTX_WARN,
                 "CreatePrimRequest parent '{}' is or descends from a gprim; "
                 "USD forbids nesting gprims. Ignoring.",
                 parent_path);
        return false;
    }

    try
    {
        const pxr::SdfPath child = UniqueChildPath(stage, parent, base_name.GetString(), out.name);
        out.prim_path = child.GetString();

        // No SdfChangeBlock around the define/arc authoring: defining a prim and
        // adding a composition arc are Usd-level operations USD advises against
        // performing inside a change block. The resync notices they fire are
        // suppressed by the caller's create_in_progress flag.
        if (is_reference)
        {
            const pxr::UsdPrim prim = stage->DefinePrim(child); // typeless def
            if (!prim)
            {
                IDTX_LOG(IDTX_ERROR, "Failed to author reference prim at '{}'.", out.prim_path);
                return false;
            }

            const bool payload =
                req.has_ref_type() && req.ref_type() == idtxcore::PRIM_REF_PAYLOAD;
            bool arc_ok = false;
            if (payload)
                arc_ok = prim.GetPayloads().AddPayload(
                    pxr::SdfPayload(resolved.authored_asset, resolved.prim_path));
            else
                arc_ok = prim.GetReferences().AddReference(
                    pxr::SdfReference(resolved.authored_asset, resolved.prim_path));

            if (!arc_ok)
            {
                IDTX_LOG(IDTX_ERROR, "Failed to author {} arc on '{}'.",
                         payload ? "payload" : "reference", out.prim_path);
                return false;
            }

            out.has_ref        = true;
            out.ref_type       = payload ? idtxcore::PRIM_REF_PAYLOAD : idtxcore::PRIM_REF_REFERENCE;
            out.reference_path = resolved.broadcast_path;
        }
        else
        {
            if (!AuthorPrim(stage, child, type))
            {
                IDTX_LOG(IDTX_ERROR, "Failed to author {} at '{}'.", base_name.GetString(), out.prim_path);
                return false;
            }
            // Read the (fallback) default attributes authored for the type.
            ReadAttributes(stage->GetPrimAtPath(child), type, out);
        }

        // Optional local transform: author it verbatim as the prim's local
        // transform relative to its parent. Applies to every prim type.
        pxr::GfMatrix4d local;
        if (LocalMatrixFromRequest(req, local))
        {
            const pxr::UsdPrim prim = stage->GetPrimAtPath(child);
            if (!AuthorLocalTransform(prim, local))
            {
                IDTX_LOG(IDTX_WARN,
                         "CreatePrimRequest carried a transform but '{}' is not xformable; "
                         "prim created without it.",
                         out.prim_path);
            }
        }

        return true;
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "Exception while creating prim under '{}': {}",
                 parent_path, e.what());
        return false;
    }
}

PrimDispatcher::SetActiveStatus PrimDispatcher::SetActive(const pxr::UsdStageRefPtr& stage,
                                                          const std::string& prim_path,
                                                          bool active)
{
    if (!stage)
    {
        IDTX_LOG(IDTX_ERROR, "PrimDispatcher::SetActive called with null stage.");
        return SetActiveStatus::Failed;
    }

    if (prim_path.empty() || !pxr::SdfPath::IsValidPathString(prim_path))
    {
        IDTX_LOG(IDTX_WARN, "SetPrimActiveRequest with invalid prim_path '{}'.", prim_path);
        return SetActiveStatus::InvalidPath;
    }

    const pxr::SdfPath path(prim_path);
    if (!path.IsAbsolutePath() || !path.IsPrimPath())
    {
        IDTX_LOG(IDTX_WARN, "SetPrimActiveRequest prim_path '{}' is not a prim path.", prim_path);
        return SetActiveStatus::InvalidPath;
    }

    // GetPrimAtPath returns inactive prims too, so this also finds a prim that
    // was already soft-deleted (which is how undo and the idempotency check
    // below work).
    const pxr::UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim || !prim.IsDefined())
    {
        IDTX_LOG(IDTX_WARN, "SetPrimActiveRequest prim '{}' not found on stage; ignoring.",
                 prim_path);
        return SetActiveStatus::NotFound;
    }

    if (prim.IsPseudoRoot())
    {
        IDTX_LOG(IDTX_WARN, "SetPrimActiveRequest targets the pseudo-root; ignoring.");
        return SetActiveStatus::NotDeletable;
    }

    if (prim.IsActive() == active)
    {
        IDTX_LOG(IDTX_DEBUG, "SetPrimActiveRequest for '{}' is a no-op (already active={}).",
                 prim_path, active);
        return SetActiveStatus::AlreadyInState;
    }

    // Deleting (active=false) requires the prim to be locally deletable;
    // reactivating (undo) only requires that the prim exists, which it does.
    if (!active)
    {
        // The stage's default prim (named in the root layer's defaultPrim
        // metadata) is the composition entry point for anyone referencing this
        // stage; deleting/deactivating it would break those references. Refuse.
        const pxr::SdfLayerHandle root_layer = stage->GetRootLayer();
        if (root_layer && root_layer->HasDefaultPrim() &&
            path == pxr::SdfPath::AbsoluteRootPath().AppendChild(root_layer->GetDefaultPrim()))
        {
            IDTX_LOG(IDTX_WARN,
                     "SetPrimActiveRequest cannot delete '{}': it is the stage's default prim. "
                     "Ignoring.",
                     prim_path);
            return SetActiveStatus::NotDeletable;
        }

        if (!IsLocallyDeletable(prim, stage->GetRootLayer(), stage->GetSessionLayer()))
        {
            IDTX_LOG(IDTX_WARN,
                     "SetPrimActiveRequest cannot delete '{}': it is defined in a sublayer or "
                     "introduced across a reference/payload. Ignoring.",
                     prim_path);
            return SetActiveStatus::NotDeletable;
        }
    }

    try
    {
        if (!prim.SetActive(active))
        {
            IDTX_LOG(IDTX_ERROR, "Failed to author active={} on '{}'.", active, prim_path);
            return SetActiveStatus::Failed;
        }
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "Exception while setting active on '{}': {}", prim_path, e.what());
        return SetActiveStatus::Failed;
    }

    return SetActiveStatus::Ok;
}

void PrimDispatcher::ReadSchemaDefaults(idtxcore::PrimType type, CreatedPrim& out)
{
    const pxr::TfType t = UsdSchemaType(type);
    if (t.IsUnknown()) return;

    const pxr::TfToken type_name = pxr::UsdSchemaRegistry::GetSchemaTypeName(t);
    const pxr::UsdPrimDefinition* def =
        pxr::UsdSchemaRegistry::GetInstance().FindConcretePrimDefinition(type_name);
    if (!def) return;

    const auto get_double = [&](const pxr::TfToken& name, double& value) -> bool
    {
        pxr::VtValue v;
        if (def->GetAttributeFallbackValue(name, &v) && v.IsHolding<double>())
        {
            value = v.UncheckedGet<double>();
            return true;
        }
        return false;
    };

    switch (type)
    {
        case idtxcore::PRIM_TYPE_CUBE:
            out.has_size = get_double(pxr::UsdGeomTokens->size, out.size);
            break;
        case idtxcore::PRIM_TYPE_SPHERE:
            out.has_radius = get_double(pxr::UsdGeomTokens->radius, out.radius);
            break;
        case idtxcore::PRIM_TYPE_CONE:
        case idtxcore::PRIM_TYPE_CYLINDER:
        case idtxcore::PRIM_TYPE_CAPSULE:
        {
            out.has_height = get_double(pxr::UsdGeomTokens->height, out.height);
            out.has_radius = get_double(pxr::UsdGeomTokens->radius, out.radius);
            pxr::VtValue axis;
            if (def->GetAttributeFallbackValue(pxr::UsdGeomTokens->axis, &axis)
                && axis.IsHolding<pxr::TfToken>())
            {
                out.axis = axis.UncheckedGet<pxr::TfToken>().GetString();
                out.has_axis = true;
            }
            break;
        }
        case idtxcore::PRIM_TYPE_PLANE:
        {
            out.has_width  = get_double(pxr::UsdGeomTokens->width, out.width);
            out.has_length = get_double(pxr::UsdGeomTokens->length, out.length);
            pxr::VtValue axis;
            if (def->GetAttributeFallbackValue(pxr::UsdGeomTokens->axis, &axis)
                && axis.IsHolding<pxr::TfToken>())
            {
                out.axis = axis.UncheckedGet<pxr::TfToken>().GetString();
                out.has_axis = true;
            }
            break;
        }
        case idtxcore::PRIM_TYPE_XFORM:
        case idtxcore::PRIM_TYPE_UNSPECIFIED:
        default:
            break;
    }
}

} // namespace session
} // namespace idtx
