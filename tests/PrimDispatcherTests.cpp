// tests/PrimDispatcherTests.cpp
//
// Stage-level unit tests of PrimDispatcher::Create's local transform
// handling. These use an in-memory UsdStage (no server, no files) and assert
// the created prim's authored LOCAL transform equals the requested transform,
// which is the contract: the client sends a local transform and the server
// authors it verbatim relative to the parent. The reference-path behaviour is
// covered by ReferencePathResolverTests and WebSocketTests.

#include "thirdparty/doctest/doctest.h"

#include <cmath>
#include <string>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#include <idtx/proto/prim.pb.h>

#include "session/PrimDispatcher.h"

using idtx::session::CreatedPrim;
using idtx::session::PrimDispatcher;

namespace
{

pxr::GfMatrix4d WorldOf(const pxr::UsdStageRefPtr& stage, const std::string& path)
{
    pxr::UsdGeomXformCache cache;
    return cache.GetLocalToWorldTransform(stage->GetPrimAtPath(pxr::SdfPath(path)));
}

bool MatrixApprox(const pxr::GfMatrix4d& a, const pxr::GfMatrix4d& b, double eps = 1e-6)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (std::abs(a[i][j] - b[i][j]) > eps) return false;
    return true;
}

} // namespace

TEST_CASE("Create authors the request transform as the prim's local transform (matrix, non-identity parent)")
{
    auto stage = pxr::UsdStage::CreateInMemory();
    auto parent = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Parent"));
    REQUIRE(parent);
    // Move the parent so parentWorld is non-identity.
    pxr::UsdGeomXformCommonAPI(parent.GetPrim()).SetTranslate(pxr::GfVec3d(10.0, 20.0, 30.0));

    // Requested LOCAL transform: translate(1,2,3) as a matrix (translation in
    // the last row, row-vector convention).
    idtxcore::CreatePrimRequest req;
    req.set_parent_path("/Parent");
    req.set_type(idtxcore::PRIM_TYPE_XFORM);
    auto* m = req.mutable_matrix();
    m->set_m00(1); m->set_m01(0); m->set_m02(0); m->set_m03(0);
    m->set_m10(0); m->set_m11(1); m->set_m12(0); m->set_m13(0);
    m->set_m20(0); m->set_m21(0); m->set_m22(1); m->set_m23(0);
    m->set_m30(1); m->set_m31(2); m->set_m32(3); m->set_m33(1);

    CreatedPrim out;
    REQUIRE(PrimDispatcher::Create(stage, req, /*uploads_root=*/"", out));

    // The transform is authored verbatim as the prim's LOCAL transform, so the
    // composed world is the parent's world composed with it: (10,20,30)+(1,2,3).
    pxr::GfMatrix4d expected(1.0);
    expected.SetTranslateOnly(pxr::GfVec3d(11.0, 22.0, 33.0));
    CHECK(MatrixApprox(WorldOf(stage, out.prim_path), expected));
}

TEST_CASE("Create separate-transform rotation matches UsdGeomXformCommonAPI RotationOrderXYZ")
{
    auto stage = pxr::UsdStage::CreateInMemory();
    auto parent = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));
    REQUIRE(parent); // identity parent, so local == world

    const pxr::GfVec3d t(1.0, 2.0, 3.0);
    const pxr::GfVec3f r(10.0f, 20.0f, 30.0f); // degrees
    const pxr::GfVec3f s(2.0f, 3.0f, 4.0f);

    idtxcore::CreatePrimRequest req;
    req.set_parent_path("/Root");
    req.set_type(idtxcore::PRIM_TYPE_XFORM);
    auto* sep = req.mutable_seperate();
    sep->mutable_translation()->set_x(t[0]); sep->mutable_translation()->set_y(t[1]); sep->mutable_translation()->set_z(t[2]);
    sep->mutable_rotation()->set_x(r[0]);    sep->mutable_rotation()->set_y(r[1]);    sep->mutable_rotation()->set_z(r[2]);
    sep->mutable_scale()->set_x(s[0]);       sep->mutable_scale()->set_y(s[1]);       sep->mutable_scale()->set_z(s[2]);

    CreatedPrim out;
    REQUIRE(PrimDispatcher::Create(stage, req, /*uploads_root=*/"", out));

    // Independent expectation: author the same vectors through the official
    // XformCommonAPI with RotationOrderXYZ and read the composed matrix.
    auto ref = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Ref"));
    pxr::UsdGeomXformCommonAPI(ref.GetPrim()).SetXformVectors(
        t, r, s, pxr::GfVec3f(0.0f),
        pxr::UsdGeomXformCommonAPI::RotationOrderXYZ,
        pxr::UsdTimeCode::Default());

    CHECK(MatrixApprox(WorldOf(stage, out.prim_path), WorldOf(stage, "/Ref")));
}
