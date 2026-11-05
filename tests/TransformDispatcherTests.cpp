// tests/TransformDispatcherTests.cpp
//
// Checks TransformDispatcher::Apply on prims the stage composes in different
// ways: defined locally, or brought in by a payload or a reference, each with
// and without xform ops of their own. Edits go to the session layer, as in a
// session. No server is involved; the layers are anonymous.

#include "thirdparty/doctest/doctest.h"

#include <string>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformable.h>

#include <idtx/proto/transform.pb.h>

#include "session/TransformDispatcher.h"

using idtx::session::TransformDispatcher;

namespace
{

// Body of an Xform that already authors a translate op.
constexpr const char* kWithOps =
    "        double3 xformOp:translate = (0, 0, 0)\n"
    "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n";

pxr::SdfLayerRefPtr Layer(const std::string& text)
{
    pxr::SdfLayerRefPtr layer = pxr::SdfLayer::CreateAnonymous(".usda");
    REQUIRE(layer->ImportFromString(text));
    return layer;
}

// An asset with /A/In, optionally with its own xform ops.
pxr::SdfLayerRefPtr Asset(bool with_ops)
{
    return Layer(std::string("#usda 1.0\ndef Xform \"A\"\n{\n    def Xform \"In\"\n    {\n")
                 + (with_ops ? kWithOps : "") + "    }\n}\n");
}

// A root layer with /R/In defined locally, optionally with its own ops.
std::string LocalRoot(bool with_ops)
{
    return std::string("#usda 1.0\ndef Xform \"R\"\n{\n    def Xform \"In\"\n    {\n")
           + (with_ops ? kWithOps : "") + "    }\n}\n";
}

// A root layer whose /R/G brings in /A of @p asset through @p arc
// ("payload" or "references"), so /A/In appears at /R/G/In.
std::string ArcRoot(const char* arc, const pxr::SdfLayerRefPtr& asset)
{
    return std::string("#usda 1.0\ndef Xform \"R\"\n{\n    def Xform \"G\" (\n        prepend ")
           + arc + " = @" + asset->GetIdentifier() + "@</A>\n    )\n    {\n    }\n}\n";
}

// A stage on @p root_text that edits its session layer, as a session does.
pxr::UsdStageRefPtr OpenStage(const std::string& root_text)
{
    pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(Layer(root_text));
    REQUIRE(stage);
    stage->SetEditTarget(pxr::UsdEditTarget(stage->GetSessionLayer()));
    return stage;
}

idtxcore::TransformUpdate SeparateUpdate(const std::string& prim_path, double x, double y, double z)
{
    idtxcore::TransformUpdate upd;
    upd.set_prim_path(prim_path);
    auto* sep = upd.mutable_seperate();
    sep->mutable_translation()->set_x(x);
    sep->mutable_translation()->set_y(y);
    sep->mutable_translation()->set_z(z);
    sep->mutable_scale()->set_x(1.0);
    sep->mutable_scale()->set_y(1.0);
    sep->mutable_scale()->set_z(1.0);
    return upd;
}

pxr::GfVec3d Translation(const pxr::UsdStageRefPtr& stage, const std::string& prim_path)
{
    const pxr::UsdGeomXformable xformable(stage->GetPrimAtPath(pxr::SdfPath(prim_path)));
    REQUIRE(xformable);
    pxr::GfMatrix4d local;
    bool resets = false;
    REQUIRE(xformable.GetLocalTransformation(&local, &resets));
    return local.ExtractTranslation();
}

// Applies two updates in a row and checks that both are authored without a
// USD error. The second runs on a prim the session layer already overrides.
void CheckApply(const std::string& root_text, const std::string& prim_path)
{
    const pxr::UsdStageRefPtr stage = OpenStage(root_text);

    pxr::TfErrorMark mark;
    CHECK(TransformDispatcher::Apply(stage, SeparateUpdate(prim_path, 5.0, 0.0, 0.0)));
    CHECK(Translation(stage, prim_path) == pxr::GfVec3d(5.0, 0.0, 0.0));
    CHECK(TransformDispatcher::Apply(stage, SeparateUpdate(prim_path, 1.0, 2.0, 3.0)));
    CHECK(Translation(stage, prim_path) == pxr::GfVec3d(1.0, 2.0, 3.0));
    CHECK(mark.IsClean());
    mark.Clear();

    const pxr::UsdGeomXformable xformable(stage->GetPrimAtPath(pxr::SdfPath(prim_path)));
    bool resets = false;
    CHECK(xformable.GetOrderedXformOps(&resets).size() == 3);
}

} // namespace

TEST_CASE("transform: a locally defined prim is authored")
{
    SUBCASE("without ops of its own") { CheckApply(LocalRoot(false), "/R/In"); }
    SUBCASE("with ops of its own")    { CheckApply(LocalRoot(true),  "/R/In"); }
}

TEST_CASE("transform: a prim from a payload is authored")
{
    SUBCASE("without ops of its own")
    {
        const pxr::SdfLayerRefPtr asset = Asset(false);
        CheckApply(ArcRoot("payload", asset), "/R/G/In");
    }
    SUBCASE("with ops of its own")
    {
        const pxr::SdfLayerRefPtr asset = Asset(true);
        CheckApply(ArcRoot("payload", asset), "/R/G/In");
    }
}

TEST_CASE("transform: a prim from a reference is authored")
{
    SUBCASE("without ops of its own")
    {
        const pxr::SdfLayerRefPtr asset = Asset(false);
        CheckApply(ArcRoot("references", asset), "/R/G/In");
    }
    SUBCASE("with ops of its own")
    {
        const pxr::SdfLayerRefPtr asset = Asset(true);
        CheckApply(ArcRoot("references", asset), "/R/G/In");
    }
}

TEST_CASE("transform: a matrix update on a prim from a payload is authored")
{
    const pxr::SdfLayerRefPtr asset = Asset(true);
    const pxr::UsdStageRefPtr stage = OpenStage(ArcRoot("payload", asset));

    idtxcore::TransformUpdate upd;
    upd.set_prim_path("/R/G/In");
    auto* m = upd.mutable_matrix();
    m->set_m00(1.0); m->set_m11(1.0); m->set_m22(1.0); m->set_m33(1.0);
    m->set_m30(4.0); m->set_m31(5.0); m->set_m32(6.0);

    pxr::TfErrorMark mark;
    CHECK(TransformDispatcher::Apply(stage, upd));
    CHECK(mark.IsClean());
    mark.Clear();
    CHECK(Translation(stage, "/R/G/In") == pxr::GfVec3d(4.0, 5.0, 6.0));
}

TEST_CASE("transform: a mismatching op type is rejected without changing the prim")
{
    const pxr::UsdStageRefPtr stage = OpenStage(
        "#usda 1.0\ndef Xform \"R\"\n{\n"
        "    float3 xformOp:translate = (7, 0, 0)\n"
        "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n");

    CHECK_FALSE(TransformDispatcher::Apply(stage, SeparateUpdate("/R", 1.0, 0.0, 0.0)));
    CHECK(stage->GetSessionLayer()->IsEmpty());
    CHECK(Translation(stage, "/R") == pxr::GfVec3d(7.0, 0.0, 0.0));
}
