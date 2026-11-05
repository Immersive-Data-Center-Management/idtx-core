#include "TransformDispatcher.h"

#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>

namespace idtx
{
namespace session
{

namespace
{

// Precision of the ops the dispatcher authors.
constexpr auto kTranslatePrecision = pxr::UsdGeomXformOp::PrecisionDouble;
constexpr auto kRotatePrecision    = pxr::UsdGeomXformOp::PrecisionFloat;
constexpr auto kScalePrecision     = pxr::UsdGeomXformOp::PrecisionFloat;
constexpr auto kMatrixPrecision    = pxr::UsdGeomXformOp::PrecisionDouble;

// The op's attribute must keep the type it already has in the composed
// prim (for example a float translate): AuthorOps() would otherwise author a
// conflicting type over a weaker layer's opinion.
bool CanAddOp(const pxr::UsdPrim&                  prim,
              const pxr::UsdGeomXformOp::Type      type,
              const pxr::UsdGeomXformOp::Precision precision)
{
    const pxr::UsdAttribute attr = prim.GetAttribute(pxr::UsdGeomXformOp::GetOpName(type));
    return !attr || attr.GetTypeName() == pxr::UsdGeomXformOp::GetValueTypeName(type, precision);
}

// True if every op the update authors can be added. Checked before anything
// is changed, so a type mismatch never leaves the prim half modified.
bool CanAuthorOps(const pxr::UsdPrim& prim, const idtxcore::TransformUpdate& upd)
{
    switch (upd.transform_case())
    {
        case idtxcore::TransformUpdate::kSeperate:
            return CanAddOp(prim, pxr::UsdGeomXformOp::TypeTranslate, kTranslatePrecision)
                && CanAddOp(prim, pxr::UsdGeomXformOp::TypeRotateXYZ, kRotatePrecision)
                && CanAddOp(prim, pxr::UsdGeomXformOp::TypeScale,     kScalePrecision);
        case idtxcore::TransformUpdate::kMatrix:
            return CanAddOp(prim, pxr::UsdGeomXformOp::TypeTransform, kMatrixPrecision);
        case idtxcore::TransformUpdate::TRANSFORM_NOT_SET:
        default:
            return true;
    }
}

// One xform op and the value to author for it.
struct OpValue
{
    pxr::UsdGeomXformOp::Type      type;
    pxr::UsdGeomXformOp::Precision precision;
    pxr::VtValue                   value;
};

// Author the attribute's default value, creating the attribute spec if the
// prim spec has none for it yet.
bool SetDefault(const pxr::SdfPrimSpecHandle& prim_spec,
                const pxr::TfToken&           name,
                const pxr::SdfValueTypeName&  type,
                const pxr::SdfVariability     variability,
                const pxr::VtValue&           value)
{
    pxr::SdfAttributeSpecHandle attr = prim_spec->GetAttributeAtPath(
        prim_spec->GetPath().AppendProperty(name));
    if (!attr) attr = pxr::SdfAttributeSpec::New(prim_spec, name.GetString(), type, variability);
    return attr && attr->SetDefaultValue(value);
}

// Replace the prim's op stack in the edit target with @p ops, in order.
// Only Sdf is used inside the change block: the stage does not recompose
// until the block closes, so Usd API would read a stale view of the prim
// (for a prim from a reference or payload it would not even see what was
// just authored). Everything that reads the stage runs before, in Apply().
bool AuthorOps(const pxr::UsdStageRefPtr&  stage,
               const pxr::UsdPrim&         prim,
               const std::vector<OpValue>& ops)
{
    const pxr::UsdEditTarget& target = stage->GetEditTarget();
    const pxr::SdfLayerHandle& layer = target.GetLayer();
    const pxr::SdfPath path = target.MapToSpecPath(prim.GetPath());
    if (!layer || path.IsEmpty()) return false;

    // Coalesces the writes below into a single notice.
    pxr::SdfChangeBlock block;

    // Returns the existing spec, or creates an over for a prim the layer has
    // none for yet.
    const pxr::SdfPrimSpecHandle prim_spec = pxr::SdfCreatePrimInLayer(layer, path);
    if (!prim_spec) return false;

    pxr::VtTokenArray order;
    order.reserve(ops.size());
    for (const OpValue& op : ops)
    {
        const pxr::TfToken name = pxr::UsdGeomXformOp::GetOpName(op.type);
        if (!SetDefault(prim_spec, name,
                        pxr::UsdGeomXformOp::GetValueTypeName(op.type, op.precision),
                        pxr::SdfVariabilityVarying, op.value))
        {
            return false;
        }
        order.push_back(name);
    }
    return SetDefault(prim_spec, pxr::UsdGeomTokens->xformOpOrder,
                      pxr::SdfValueTypeNames->TokenArray, pxr::SdfVariabilityUniform,
                      pxr::VtValue(order));
}

bool ApplySeparate(const pxr::UsdStageRefPtr&         stage,
                   const pxr::UsdPrim&                prim,
                   const idtxcore::SeparateTransform& sep)
{
    const auto& t = sep.translation();
    const auto& r = sep.rotation();
    const auto& s = sep.scale();
    return AuthorOps(stage, prim, {
        {pxr::UsdGeomXformOp::TypeTranslate, kTranslatePrecision,
         pxr::VtValue(pxr::GfVec3d(t.x(), t.y(), t.z()))},
        {pxr::UsdGeomXformOp::TypeRotateXYZ, kRotatePrecision,
         pxr::VtValue(pxr::GfVec3f(static_cast<float>(r.x()),
                                   static_cast<float>(r.y()),
                                   static_cast<float>(r.z())))},
        {pxr::UsdGeomXformOp::TypeScale, kScalePrecision,
         pxr::VtValue(pxr::GfVec3f(static_cast<float>(s.x()),
                                   static_cast<float>(s.y()),
                                   static_cast<float>(s.z())))},
    });
}

bool ApplyMatrix(const pxr::UsdStageRefPtr&         stage,
                 const pxr::UsdPrim&                prim,
                 const idtxcore::Matrix4dTransform& m)
{
    const pxr::GfMatrix4d gfm(
        m.m00(), m.m01(), m.m02(), m.m03(),
        m.m10(), m.m11(), m.m12(), m.m13(),
        m.m20(), m.m21(), m.m22(), m.m23(),
        m.m30(), m.m31(), m.m32(), m.m33());
    return AuthorOps(stage, prim,
                     {{pxr::UsdGeomXformOp::TypeTransform, kMatrixPrecision, pxr::VtValue(gfm)}});
}

} // namespace

bool TransformDispatcher::Apply(const pxr::UsdStageRefPtr& stage,
                                const idtxcore::TransformUpdate& upd)
{
    if (!stage)
    {
        IDTX_LOG(IDTX_ERROR, "TransformDispatcher::Apply called with null stage.");
        return false;
    }

    const std::string& prim_path_str = upd.prim_path();
    if (prim_path_str.empty())
    {
        IDTX_LOG(IDTX_WARN, "TransformUpdate without prim_path; ignoring.");
        return false;
    }

    if (!pxr::SdfPath::IsValidPathString(prim_path_str))
    {
        IDTX_LOG(IDTX_WARN, "TransformUpdate with invalid prim_path '{}'.", prim_path_str);
        return false;
    }

    pxr::SdfPath prim_path(prim_path_str);
    pxr::UsdPrim prim = stage->GetPrimAtPath(prim_path);
    if (!prim)
    {
        IDTX_LOG(IDTX_WARN, "Prim '{}' not found on stage; ignoring TransformUpdate.",
                 prim_path_str);
        return false;
    }

    if (!pxr::UsdGeomXformable(prim))
    {
        IDTX_LOG(IDTX_WARN, "Prim '{}' is not Xformable; ignoring TransformUpdate.",
                 prim_path_str);
        return false;
    }

    // AuthorOps() writes through Sdf and so bypasses the stage's own check:
    // an opinion on an instance proxy or prototype would have no effect.
    if (prim.IsInstanceProxy() || prim.IsInPrototype())
    {
        IDTX_LOG(IDTX_WARN, "Prim '{}' is part of an instance; ignoring TransformUpdate.",
                 prim_path_str);
        return false;
    }

    try
    {
        if (!CanAuthorOps(prim, upd))
        {
            IDTX_LOG(IDTX_WARN,
                     "Prim '{}' has an xform op attribute with a different precision; "
                     "ignoring TransformUpdate.",
                     prim_path_str);
            return false;
        }

        switch (upd.transform_case())
        {
            case idtxcore::TransformUpdate::kSeperate:
                return ApplySeparate(stage, prim, upd.seperate());
            case idtxcore::TransformUpdate::kMatrix:
                return ApplyMatrix(stage, prim, upd.matrix());
            case idtxcore::TransformUpdate::TRANSFORM_NOT_SET:
            default:
                IDTX_LOG(IDTX_WARN, "TransformUpdate for '{}' has no transform set.",
                         prim_path_str);
                return false;
        }
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "Exception while authoring transform on '{}': {}",
                 prim_path_str, e.what());
        return false;
    }
}

bool TransformDispatcher::ReadAuthoredLocalTransform(
    const pxr::SdfPrimSpecHandle& prim_spec,
    idtxcore::SeparateTransform& out_sep,
    idtxcore::Matrix4dTransform& out_mat,
    bool& out_use_matrix)
{
    if (!prim_spec) return false;
    const pxr::SdfLayerHandle layer = prim_spec->GetLayer();
    if (!layer) return false;
    const pxr::SdfPath base = prim_spec->GetPath();

    // Op attribute names must match what Apply() authors (see AddTranslateOp /
    // AddRotateXYZOp / AddScaleOp / AddTransformOp, all with no op suffix).
    const auto attr_default = [&](const char* op_name) -> pxr::VtValue
    {
        const pxr::SdfAttributeSpecHandle attr =
            layer->GetAttributeAtPath(base.AppendProperty(pxr::TfToken(op_name)));
        return attr ? attr->GetDefaultValue() : pxr::VtValue();
    };

    // Matrix shape: a single xformOp:transform.
    const pxr::VtValue xf = attr_default("xformOp:transform");
    if (xf.IsHolding<pxr::GfMatrix4d>())
    {
        const pxr::GfMatrix4d& m = xf.UncheckedGet<pxr::GfMatrix4d>();
        out_mat.set_m00(m[0][0]); out_mat.set_m01(m[0][1]); out_mat.set_m02(m[0][2]); out_mat.set_m03(m[0][3]);
        out_mat.set_m10(m[1][0]); out_mat.set_m11(m[1][1]); out_mat.set_m12(m[1][2]); out_mat.set_m13(m[1][3]);
        out_mat.set_m20(m[2][0]); out_mat.set_m21(m[2][1]); out_mat.set_m22(m[2][2]); out_mat.set_m23(m[2][3]);
        out_mat.set_m30(m[3][0]); out_mat.set_m31(m[3][1]); out_mat.set_m32(m[3][2]); out_mat.set_m33(m[3][3]);
        out_use_matrix = true;
        return true;
    }

    // Separate shape: translate + rotateXYZ + scale. Any op the dispatcher
    // omitted defaults to the identity component.
    const pxr::VtValue t = attr_default("xformOp:translate");
    const pxr::VtValue r = attr_default("xformOp:rotateXYZ");
    const pxr::VtValue s = attr_default("xformOp:scale");
    if (t.IsEmpty() && r.IsEmpty() && s.IsEmpty()) return false;

    pxr::GfVec3d tv(0.0);
    pxr::GfVec3f rv(0.0f);
    pxr::GfVec3f sv(1.0f);
    if (t.IsHolding<pxr::GfVec3d>()) tv = t.UncheckedGet<pxr::GfVec3d>();
    if (r.IsHolding<pxr::GfVec3f>()) rv = r.UncheckedGet<pxr::GfVec3f>();
    if (s.IsHolding<pxr::GfVec3f>()) sv = s.UncheckedGet<pxr::GfVec3f>();

    out_sep.mutable_translation()->set_x(tv[0]);
    out_sep.mutable_translation()->set_y(tv[1]);
    out_sep.mutable_translation()->set_z(tv[2]);
    out_sep.mutable_rotation()->set_x(rv[0]);
    out_sep.mutable_rotation()->set_y(rv[1]);
    out_sep.mutable_rotation()->set_z(rv[2]);
    out_sep.mutable_scale()->set_x(sv[0]);
    out_sep.mutable_scale()->set_y(sv[1]);
    out_sep.mutable_scale()->set_z(sv[2]);
    out_use_matrix = false;
    return true;
}

} // namespace session
} // namespace idtx