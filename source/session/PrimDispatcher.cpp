#include "PrimDispatcher.h"

#include <cstdint>
#include <string>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>

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

// True if every opinion contributing to @p prim lives on the stage's local
// layers (its root layer or this session's layer) and nowhere else. A prim
// introduced across a reference or payload, or defined in a sublayer, has at
// least one contributing spec on a foreign layer and so is NOT locally
// deletable: its definition cannot be masked by a session-layer `active=false`
// in a way we can later fold into the committed root file. Covers both a
// root-authored prim (spec on the root layer, plus our session overrides) and
// a prim created earlier this session (spec only on the session layer).
bool IsLocallyDeletable(const pxr::UsdPrim& prim,
                        const pxr::SdfLayerHandle& root_layer,
                        const pxr::SdfLayerHandle& session_layer)
{
    const pxr::SdfPrimSpecHandleVector stack = prim.GetPrimStack();
    if (stack.empty()) return false;
    for (const pxr::SdfPrimSpecHandle& spec : stack)
    {
        if (!spec) continue;
        const pxr::SdfLayerHandle layer = spec->GetLayer();
        if (layer != root_layer && layer != session_layer) return false;
    }
    return true;
}

} // namespace

idtxcore::PrimType PrimDispatcher::TypeFromName(const std::string& type_name)
{
    if (type_name == "Cube")     return idtxcore::PRIM_TYPE_CUBE;
    if (type_name == "Sphere")   return idtxcore::PRIM_TYPE_SPHERE;
    if (type_name == "Cone")     return idtxcore::PRIM_TYPE_CONE;
    if (type_name == "Cylinder") return idtxcore::PRIM_TYPE_CYLINDER;
    if (type_name == "Capsule")  return idtxcore::PRIM_TYPE_CAPSULE;
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
        case idtxcore::PRIM_TYPE_XFORM:        // a transform group has no shape attributes
        case idtxcore::PRIM_TYPE_UNSPECIFIED:
        default:
            break;
    }
}

bool PrimDispatcher::Create(const pxr::UsdStageRefPtr& stage,
                            const std::string& parent_path,
                            idtxcore::PrimType type,
                            CreatedPrim& out)
{
    if (!stage)
    {
        IDTX_LOG(IDTX_ERROR, "PrimDispatcher::Create called with null stage.");
        return false;
    }

    const pxr::TfToken base_name = BaseNameFor(type);
    if (base_name.IsEmpty())
    {
        IDTX_LOG(IDTX_WARN, "CreatePrimRequest with unknown prim type {}; ignoring.",
                 static_cast<int>(type));
        return false;
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

    // Gprims may not be nested under another gprim. Xform has no such rule and
    // may be parented under any valid prim, so it skips this check.
    if (IsGprimType(type) && HasGprimAncestor(parent_prim))
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

        // No SdfChangeBlock here: defining a prim is Usd-level authoring, which
        // USD advises against performing inside a change block. The resync
        // notice this fires is suppressed by the caller's create_in_progress
        // flag, so coalescing notices buys nothing.
        if (!AuthorPrim(stage, child, type))
        {
            IDTX_LOG(IDTX_ERROR, "Failed to author {} at '{}'.", base_name.GetString(), out.prim_path);
            return false;
        }

        // Read the (fallback) default attributes authored for the type.
        ReadAttributes(stage->GetPrimAtPath(child), type, out);
        return true;
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "Exception while creating {} under '{}': {}",
                 base_name.GetString(), parent_path, e.what());
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
        case idtxcore::PRIM_TYPE_XFORM:
        case idtxcore::PRIM_TYPE_UNSPECIFIED:
        default:
            break;
    }
}

} // namespace session
} // namespace idtx
