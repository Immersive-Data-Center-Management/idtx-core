#include "TransformDispatcher.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>

namespace idtx
{
namespace session
{

namespace
{

bool ApplySeparate(pxr::UsdGeomXformable& xformable,
                   const idtxcore::SeparateTransform& sep)
{
    // this ChangeBlock ensures the TfNotice triggers only once after it goes out of scope reducing the noice
    // of the different set operations below
    pxr::SdfChangeBlock block;
    
    // Reset the xform op stack so we author a clean t-r-s sequence.
    bool reset_xform_stack = false;
    xformable.SetXformOpOrder({}, reset_xform_stack);

    auto translateOp = xformable.AddTranslateOp(pxr::UsdGeomXformOp::PrecisionDouble);
    auto rotateOp    = xformable.AddRotateXYZOp(pxr::UsdGeomXformOp::PrecisionFloat);
    auto scaleOp     = xformable.AddScaleOp(pxr::UsdGeomXformOp::PrecisionFloat);

    if (!translateOp || !rotateOp || !scaleOp) return false;

    const auto& t = sep.translation();
    const auto& r = sep.rotation();
    const auto& s = sep.scale();

    translateOp.Set(pxr::GfVec3d(t.x(), t.y(), t.z()));
    rotateOp.Set(pxr::GfVec3f(static_cast<float>(r.x()),
                              static_cast<float>(r.y()),
                              static_cast<float>(r.z())));
    scaleOp.Set(pxr::GfVec3f(static_cast<float>(s.x()),
                             static_cast<float>(s.y()),
                             static_cast<float>(s.z())));
    return true;
}

bool ApplyMatrix(pxr::UsdGeomXformable& xformable,
                 const idtxcore::Matrix4dTransform& m)
{
    // this ChangeBlock ensures the TfNotice triggers only once after it goes out of scope reducing the noise
    // of the different set operations below
    pxr::SdfChangeBlock block;
    
    bool reset_xform_stack = false;
    xformable.SetXformOpOrder({}, reset_xform_stack);

    auto op = xformable.AddTransformOp(pxr::UsdGeomXformOp::PrecisionDouble);
    if (!op) return false;

    pxr::GfMatrix4d gfm(
        m.m00(), m.m01(), m.m02(), m.m03(),
        m.m10(), m.m11(), m.m12(), m.m13(),
        m.m20(), m.m21(), m.m22(), m.m23(),
        m.m30(), m.m31(), m.m32(), m.m33());

    op.Set(gfm);
    return true;
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

    pxr::UsdGeomXformable xformable(prim);
    if (!xformable)
    {
        IDTX_LOG(IDTX_WARN, "Prim '{}' is not Xformable; ignoring TransformUpdate.",
                 prim_path_str);
        return false;
    }

    try
    {
        switch (upd.transform_case())
        {
            case idtxcore::TransformUpdate::kSeperate:
                return ApplySeparate(xformable, upd.seperate());
            case idtxcore::TransformUpdate::kMatrix:
                return ApplyMatrix(xformable, upd.matrix());
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

} // namespace session
} // namespace idtx