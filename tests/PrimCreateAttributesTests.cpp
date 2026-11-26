// tests/PrimCreateAttributesTests.cpp
//
// Stage-level unit tests of PrimDispatcher::Create's dimensions and request
// attributes: built-in prims get 1 m based dimensions in the stage's units,
// attributes carried by the request override them and are authored with the
// creation, and a create whose attributes cannot all be authored creates
// nothing. In-memory stages, no server.
#include "thirdparty/doctest/doctest.h"

#include <string>
#include <vector>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>

#include <idtx/proto/prim.pb.h>

#include "session/PrimDispatcher.h"
#include "support/ProtoHelpers.h"

using idtx::session::CreatedPrim;
using idtx::session::PrimDispatcher;
using Status = PrimDispatcher::CreatePrimStatus;

namespace
{
pxr::UsdStageRefPtr MakeStage(double meters_per_unit)
{
    auto stage = pxr::UsdStage::CreateInMemory();
    pxr::UsdGeomSetStageMetersPerUnit(stage, meters_per_unit);
    pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));
    return stage;
}

idtxcore::CreatePrimRequest Request(idtxcore::PrimType type)
{
    idtxcore::CreatePrimRequest req;
    req.set_parent_path("/Root");
    req.set_type(type);
    return req;
}

pxr::UsdPrim CreateOk(const pxr::UsdStageRefPtr& stage, const idtxcore::CreatePrimRequest& req, CreatedPrim& out)
{
    REQUIRE(PrimDispatcher::Create(stage, req, /*uploads_root=*/"", out) == Status::Ok);
    return stage->GetPrimAtPath(pxr::SdfPath(out.prim_path));
}

double Get(const pxr::UsdAttribute& attr)
{
    double value = -1.0;
    REQUIRE(attr.HasAuthoredValue());
    REQUIRE(attr.Get(&value));
    return value;
}
} // namespace

TEST_CASE("Create authors 1 m based dimensions in a metre stage")
{
    const auto stage = MakeStage(1.0);
    CreatedPrim out;

    const auto cube = CreateOk(stage, Request(idtxcore::PRIM_TYPE_CUBE), out);
    CHECK(Get(pxr::UsdGeomCube(cube).GetSizeAttr()) == doctest::Approx(1.0));
    CHECK(out.size == doctest::Approx(1.0)); // broadcast reports the authored value

    out = {};
    const auto sphere = CreateOk(stage, Request(idtxcore::PRIM_TYPE_SPHERE), out);
    CHECK(Get(pxr::UsdGeomSphere(sphere).GetRadiusAttr()) == doctest::Approx(0.5));

    out = {};
    const auto cylinder = CreateOk(stage, Request(idtxcore::PRIM_TYPE_CYLINDER), out);
    CHECK(Get(pxr::UsdGeomCylinder(cylinder).GetRadiusAttr()) == doctest::Approx(0.5));
    CHECK(Get(pxr::UsdGeomCylinder(cylinder).GetHeightAttr()) == doctest::Approx(1.0));

    out = {};
    const auto cone = CreateOk(stage, Request(idtxcore::PRIM_TYPE_CONE), out);
    CHECK(Get(pxr::UsdGeomCone(cone).GetRadiusAttr()) == doctest::Approx(0.5));
    CHECK(Get(pxr::UsdGeomCone(cone).GetHeightAttr()) == doctest::Approx(1.0));

    out = {};
    const auto capsule = CreateOk(stage, Request(idtxcore::PRIM_TYPE_CAPSULE), out);
    CHECK(Get(pxr::UsdGeomCapsule(capsule).GetRadiusAttr()) == doctest::Approx(0.25));
    CHECK(Get(pxr::UsdGeomCapsule(capsule).GetHeightAttr()) == doctest::Approx(0.5)); // + caps = 1 m

    out = {};
    const auto plane = CreateOk(stage, Request(idtxcore::PRIM_TYPE_PLANE), out);
    CHECK(Get(pxr::UsdGeomPlane(plane).GetWidthAttr()) == doctest::Approx(1.0));
    CHECK(Get(pxr::UsdGeomPlane(plane).GetLengthAttr()) == doctest::Approx(1.0));

    out = {};
    const auto xform = CreateOk(stage, Request(idtxcore::PRIM_TYPE_XFORM), out);
    CHECK(xform.GetAuthoredAttributes().empty());
}

TEST_CASE("Create converts the 1 m dimensions to the stage's units")
{
    const auto stage = MakeStage(0.01); // centimetres
    CreatedPrim out;
    const auto cube = CreateOk(stage, Request(idtxcore::PRIM_TYPE_CUBE), out);
    CHECK(Get(pxr::UsdGeomCube(cube).GetSizeAttr()) == doctest::Approx(100.0));
    CHECK(out.size == doctest::Approx(100.0));
}

TEST_CASE("Create authors an extent matching the dimensions")
{
    const auto stage = MakeStage(1.0);
    CreatedPrim out;
    const auto cube = CreateOk(stage, Request(idtxcore::PRIM_TYPE_CUBE), out);
    pxr::VtVec3fArray extent;
    REQUIRE(pxr::UsdGeomCube(cube).GetExtentAttr().Get(&extent));
    REQUIRE(extent.size() == 2);
    CHECK(extent[0][0] == doctest::Approx(-0.5f));
    CHECK(extent[1][2] == doctest::Approx(0.5f));
}

TEST_CASE("Create authors the request's attributes, which override the defaults")
{
    const auto stage = MakeStage(1.0);
    auto req = Request(idtxcore::PRIM_TYPE_CUBE);
    *req.add_attributes() = idtx::tests::MakeDoubleAttr("size", "double", {3.0});
    *req.add_attributes() =
        idtx::tests::MakeDoubleAttr("primvars:displayColor", "color3f", {1.0, 0.0, 0.0}, /*is_array=*/true);

    CreatedPrim out;
    const auto cube = CreateOk(stage, req, out);
    CHECK(Get(pxr::UsdGeomCube(cube).GetSizeAttr()) == doctest::Approx(3.0));
    CHECK(out.size == doctest::Approx(3.0));

    pxr::VtVec3fArray color;
    REQUIRE(pxr::UsdGeomCube(cube).GetDisplayColorAttr().Get(&color));
    REQUIRE(color.size() == 1);
    CHECK(color[0][0] == doctest::Approx(1.0f));

    // The extent follows the overridden size.
    pxr::VtVec3fArray extent;
    REQUIRE(pxr::UsdGeomCube(cube).GetExtentAttr().Get(&extent));
    CHECK(extent[1][0] == doctest::Approx(1.5f));

    // Both attributes are reported for the broadcast, in request order.
    REQUIRE(out.attributes.size() == 2);
    CHECK(out.attributes[0].name() == "size");
    CHECK(out.attributes[1].name() == "primvars:displayColor");
    CHECK(out.attribute_failures.empty());
}

TEST_CASE("Create with an attribute it cannot author creates nothing")
{
    const auto stage = MakeStage(1.0);
    auto req = Request(idtxcore::PRIM_TYPE_CUBE);
    *req.add_attributes() = idtx::tests::MakeDoubleAttr("size", "double", {3.0});
    // Transforms belong in the request's transform, not in its attributes.
    *req.add_attributes() = idtx::tests::MakeDoubleAttr("xformOp:translate", "double3", {1.0, 2.0, 3.0});

    CreatedPrim out;
    CHECK(PrimDispatcher::Create(stage, req, "", out) == Status::InvalidAttribute);
    CHECK(PrimDispatcher::CreatePrimStatusToString(Status::InvalidAttribute) == std::string("invalid_attribute"));
    CHECK_FALSE(stage->GetPrimAtPath(pxr::SdfPath(out.prim_path)));
    CHECK_FALSE(stage->GetEditTarget().GetLayer()->GetPrimAtPath(pxr::SdfPath(out.prim_path)));
    REQUIRE(out.attribute_failures.size() == 1);
    CHECK(out.attribute_failures[0].name() == "xformOp:translate");
    CHECK(out.attribute_failures[0].reason() == idtxcore::ATTRIBUTE_FAILURE_RESERVED_ATTRIBUTE);
}

TEST_CASE("Create rejects an attribute whose value does not match the schema type")
{
    const auto stage = MakeStage(1.0);
    auto req = Request(idtxcore::PRIM_TYPE_CUBE);
    *req.add_attributes() = idtx::tests::MakeStringAttr("size", "string", {"big"});

    CreatedPrim out;
    CHECK(PrimDispatcher::Create(stage, req, "", out) == Status::InvalidAttribute);
    CHECK_FALSE(stage->GetPrimAtPath(pxr::SdfPath(out.prim_path)));
    REQUIRE(out.attribute_failures.size() == 1);
    CHECK(out.attribute_failures[0].reason() == idtxcore::ATTRIBUTE_FAILURE_TYPE_MISMATCH);
}

TEST_CASE("ReadAuthoredDimensions reads the spec, falling back to the schema defaults")
{
    const auto stage = MakeStage(1.0);
    auto req = Request(idtxcore::PRIM_TYPE_CONE);
    *req.add_attributes() = idtx::tests::MakeDoubleAttr("height", "double", {4.0});
    CreatedPrim created;
    CreateOk(stage, req, created);

    const auto spec = stage->GetEditTarget().GetLayer()->GetPrimAtPath(pxr::SdfPath(created.prim_path));
    REQUIRE(spec);
    CreatedPrim snapshot;
    PrimDispatcher::ReadAuthoredDimensions(spec, idtxcore::PRIM_TYPE_CONE, snapshot);
    CHECK(snapshot.height == doctest::Approx(4.0)); // from the request
    CHECK(snapshot.radius == doctest::Approx(0.5)); // the 1 m default
    CHECK(snapshot.axis == "Z");                    // schema fallback, not authored

    CreatedPrim none;
    PrimDispatcher::ReadAuthoredDimensions(pxr::SdfPrimSpecHandle(), idtxcore::PRIM_TYPE_CUBE, none);
    CHECK(none.size == doctest::Approx(2.0)); // no spec: the schema fallback
}
