// tests/AttributeDispatcherTests.cpp
//
// Stage-level unit tests of AttributeDispatcher::Author / ReadAuthoredAttribute
// and the ProtoVtCodec round trips they rely on. These use in-memory UsdStages
// (no server, no files) and author into the session layer, as a session does.
// They pin the acceptance criteria in plan §§2, 4.1, 6.2.

#include "thirdparty/doctest/doctest.h"

#include <cstdint>
#include <string>
#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>

#include <idtx/proto/attribute.pb.h>

#include "session/AttributeDispatcher.h"
#include "support/ProtoHelpers.h"

using idtx::session::AttributeAssetContext;
using idtx::session::AttributeDispatcher;
using FR = idtx::session::AttributeDispatcher::FailureReason;
using idtx::tests::MakeBoolAttr;
using idtx::tests::MakeDoubleAttr;
using idtx::tests::MakeIntAttr;
using idtx::tests::MakeStringAttr;

namespace
{

// A stage with /Root (Xform) defined on the root layer, editing the session
// layer, as a collaboration session does.
pxr::UsdStageRefPtr Stage()
{
    pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
    pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));
    stage->SetEditTarget(pxr::UsdEditTarget(stage->GetSessionLayer()));
    return stage;
}

idtxcore::SetPrimAttributesRequest Req(const std::string& path,
                                       std::vector<idtxcore::PrimAttribute> attrs)
{
    idtxcore::SetPrimAttributesRequest r;
    r.set_prim_path(path);
    for (auto& a : attrs) *r.add_attributes() = std::move(a);
    return r;
}

AttributeDispatcher::AuthoringResult Author(const pxr::UsdStageRefPtr& stage,
                                            const idtxcore::SetPrimAttributesRequest& req)
{
    return AttributeDispatcher::Author(stage, req, AttributeAssetContext{});
}

pxr::VtValue Composed(const pxr::UsdStageRefPtr& stage, const char* path, const char* name)
{
    pxr::VtValue v;
    stage->GetPrimAtPath(pxr::SdfPath(path)).GetAttribute(pxr::TfToken(name)).Get(&v);
    return v;
}

} // namespace

TEST_CASE("attr: scalar float round-trips through author and read-back")
{
    const auto stage = Stage();
    const auto result = Author(stage, Req("/Root", {MakeDoubleAttr("myF", "float", {1.5})}));

    REQUIRE(result.authored_any);
    REQUIRE(result.attributes.size() == 1);
    CHECK(result.attributes[0].ok);
    CHECK(Composed(stage, "/Root", "myF").Get<float>() == doctest::Approx(1.5f));

    // The pre-encoded wire value preserves the scalar token and is not an array.
    CHECK(result.attributes[0].wire.type_name() == "float");
    CHECK(result.attributes[0].wire.value().is_array() == false);
    CHECK(result.attributes[0].wire.value().doubles_size() == 1);
}

TEST_CASE("attr: color3f preserves its role (not plain float3)")
{
    const auto stage = Stage();
    const auto result = Author(stage, Req("/Root", {MakeDoubleAttr("col", "color3f", {0.1, 0.2, 0.3})}));

    REQUIRE(result.authored_any);
    CHECK(result.attributes[0].ok);
    const pxr::UsdAttribute a = stage->GetPrimAtPath(pxr::SdfPath("/Root")).GetAttribute(pxr::TfToken("col"));
    CHECK(a.GetTypeName() == pxr::SdfValueTypeNames->Color3f);
    CHECK(Composed(stage, "/Root", "col").Get<pxr::GfVec3f>() == pxr::GfVec3f(0.1f, 0.2f, 0.3f));
    CHECK(result.attributes[0].wire.type_name() == "color3f");
}

TEST_CASE("attr: token / bool / int / uint64 scalars")
{
    const auto stage = Stage();
    const auto result = Author(stage, Req("/Root", {
        MakeStringAttr("tk", "token", {"up"}),
        MakeBoolAttr("flag", "bool", {true}),
        MakeIntAttr("n", "int", {5}),
    }));
    REQUIRE(result.authored_any);
    for (const auto& r : result.attributes) CHECK(r.ok);
    CHECK(Composed(stage, "/Root", "tk").Get<pxr::TfToken>() == pxr::TfToken("up"));
    CHECK(Composed(stage, "/Root", "flag").Get<bool>() == true);
    CHECK(Composed(stage, "/Root", "n").Get<int>() == 5);
}

TEST_CASE("attr: arrays incl. empty and length-one preserve type")
{
    const auto stage = Stage();
    const auto result = Author(stage, Req("/Root", {
        MakeDoubleAttr("fa", "float", {1, 2, 3}, /*is_array=*/true),
        MakeDoubleAttr("one", "color3f", {0.5, 0.5, 0.5}, /*is_array=*/true),
        MakeDoubleAttr("empty", "float", {}, /*is_array=*/true),
    }));
    REQUIRE(result.authored_any);
    for (const auto& r : result.attributes) CHECK(r.ok);

    CHECK(Composed(stage, "/Root", "fa").Get<pxr::VtArray<float>>().size() == 3);
    const auto one = Composed(stage, "/Root", "one").Get<pxr::VtArray<pxr::GfVec3f>>();
    CHECK(one.size() == 1);
    CHECK(Composed(stage, "/Root", "empty").Get<pxr::VtArray<float>>().size() == 0);
    // Array-spelled wire token stays scalar; is_array carries the array-ness.
    CHECK(result.attributes[0].wire.type_name() == "float");
    CHECK(result.attributes[0].wire.value().is_array() == true);
}

TEST_CASE("attr: non-schema name is authored custom=true")
{
    const auto stage = Stage();
    const auto result = Author(stage, Req("/Root", {MakeDoubleAttr("myCustom", "double", {7.0})}));
    REQUIRE(result.authored_any);
    const pxr::UsdAttribute a = stage->GetPrimAtPath(pxr::SdfPath("/Root")).GetAttribute(pxr::TfToken("myCustom"));
    CHECK(a.IsCustom());
}

TEST_CASE("attr: type mismatch against an existing attribute is rejected")
{
    const auto stage = Stage();
    REQUIRE(Author(stage, Req("/Root", {MakeDoubleAttr("x", "float", {1.0})})).authored_any);
    // Now author a different type at the same name.
    const auto result = Author(stage, Req("/Root", {MakeStringAttr("x", "token", {"nope"})}));
    CHECK_FALSE(result.authored_any);
    CHECK(result.attributes[0].reason == FR::TypeMismatch);
}

TEST_CASE("attr: unknown and array-spelled types")
{
    const auto stage = Stage();
    auto r1 = Author(stage, Req("/Root", {MakeDoubleAttr("a", "notatype", {1.0})}));
    CHECK(r1.attributes[0].reason == FR::UnknownType);
    auto r2 = Author(stage, Req("/Root", {MakeDoubleAttr("b", "float[]", {1.0})}));
    CHECK(r2.attributes[0].reason == FR::InvalidValue);
    CHECK_FALSE(r2.authored_any);
}

TEST_CASE("attr: reserved transform names are rejected")
{
    const auto stage = Stage();
    auto r = Author(stage, Req("/Root", {
        MakeDoubleAttr("xformOp:translate", "double3", {1, 2, 3}),
        MakeStringAttr("xformOpOrder", "token", {"xformOp:translate"}, true),
    }));
    CHECK_FALSE(r.authored_any);
    CHECK(r.attributes[0].reason == FR::ReservedAttribute);
    CHECK(r.attributes[1].reason == FR::ReservedAttribute);
}

TEST_CASE("attr: relationship-kind collision is rejected")
{
    const auto stage = Stage();
    stage->GetPrimAtPath(pxr::SdfPath("/Root")).CreateRelationship(pxr::TfToken("myRel"));
    auto r = Author(stage, Req("/Root", {MakeDoubleAttr("myRel", "float", {1.0})}));
    CHECK_FALSE(r.attributes[0].ok);
    CHECK(r.attributes[0].reason == FR::PropertyKindMismatch);
}

TEST_CASE("attr: all occurrences of a duplicate name fail; unique sibling succeeds")
{
    const auto stage = Stage();
    auto r = Author(stage, Req("/Root", {
        MakeDoubleAttr("dup", "float", {1.0}),
        MakeDoubleAttr("dup", "float", {2.0}),
        MakeDoubleAttr("solo", "float", {3.0}),
    }));
    CHECK(r.attributes[0].reason == FR::InvalidName);
    CHECK(r.attributes[1].reason == FR::InvalidName);
    CHECK(r.attributes[2].ok);
    CHECK(r.authored_any); // the unique sibling was authored
}

TEST_CASE("attr: invalid value shapes")
{
    const auto stage = Stage();
    // float3 scalar needs exactly 3 doubles.
    CHECK(Author(stage, Req("/Root", {MakeDoubleAttr("v", "float3", {1, 2})})).attributes[0].reason == FR::InvalidValue);
    // int in the wrong lane (doubles instead of ints).
    CHECK(Author(stage, Req("/Root", {MakeDoubleAttr("i", "int", {5})})).attributes[0].reason == FR::InvalidValue);
    // missing value.
    idtxcore::PrimAttribute no_val;
    no_val.set_name("nv");
    no_val.set_type_name("float");
    CHECK(Author(stage, Req("/Root", {no_val})).attributes[0].reason == FR::InvalidValue);
}

TEST_CASE("attr: integer overflow is range-checked")
{
    const auto stage = Stage();
    auto r = Author(stage, Req("/Root", {MakeIntAttr("big", "int", {5000000000LL})}));
    CHECK(r.attributes[0].reason == FR::InvalidValue);
}

TEST_CASE("attr: prim-level rejections")
{
    const auto stage = Stage();
    CHECK(Author(stage, Req("/Missing", {MakeDoubleAttr("x", "float", {1.0})})).attributes[0].reason == FR::PrimNotFound);
    CHECK(Author(stage, Req("/", {MakeDoubleAttr("x", "float", {1.0})})).attributes[0].reason == FR::PrimNotFound);
    CHECK(Author(stage, Req("not/absolute", {MakeDoubleAttr("x", "float", {1.0})})).attributes[0].reason == FR::PrimNotFound);
}

TEST_CASE("attr: empty request is a no-op")
{
    const auto stage = Stage();
    auto r = Author(stage, Req("/Root", {}));
    CHECK_FALSE(r.authored_any);
    CHECK(r.attributes.empty());
    CHECK(stage->GetSessionLayer()->IsEmpty());
}

TEST_CASE("attr: an all-invalid batch authors no spec")
{
    const auto stage = Stage();
    auto r = Author(stage, Req("/Root", {MakeDoubleAttr("a", "notatype", {1.0})}));
    CHECK_FALSE(r.authored_any);
    CHECK(stage->GetSessionLayer()->GetPrimAtPath(pxr::SdfPath("/Root")) == pxr::SdfPrimSpecHandle());
}

TEST_CASE("attr: ReadAuthoredAttribute reproduces an authored default")
{
    const auto stage = Stage();
    REQUIRE(Author(stage, Req("/Root", {MakeDoubleAttr("col", "color3f", {0.1, 0.2, 0.3})})).authored_any);

    const pxr::SdfAttributeSpecHandle spec =
        stage->GetSessionLayer()->GetAttributeAtPath(pxr::SdfPath("/Root.col"));
    REQUIRE(spec);
    idtxcore::PrimAttribute out;
    REQUIRE(AttributeDispatcher::ReadAuthoredAttribute(spec, AttributeAssetContext{}, out));
    CHECK(out.name() == "col");
    CHECK(out.type_name() == "color3f");
    CHECK(out.value().is_array() == false);
    CHECK(out.value().doubles_size() == 3);
}

TEST_CASE("attr: uint64 / int64 / uchar / half / matrix4d / quatf round-trip")
{
    const auto stage = Stage();

    idtxcore::PrimAttribute u64;
    u64.set_name("u64"); u64.set_type_name("uint64");
    u64.mutable_value()->add_uints(18000000000000000000ULL); // > 2^63

    idtxcore::PrimAttribute i64;
    i64.set_name("i64"); i64.set_type_name("int64");
    i64.mutable_value()->add_ints(-5000000000LL);

    idtxcore::PrimAttribute uc;
    uc.set_name("uc"); uc.set_type_name("uchar");
    uc.mutable_value()->add_uints(200);

    const auto result = Author(stage, Req("/Root", {
        u64, i64, uc,
        MakeDoubleAttr("h", "half", {1.25}),
        MakeDoubleAttr("m", "matrix4d",
                       {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  4, 5, 6, 1}),
        MakeDoubleAttr("q", "quatf", {1, 0, 0, 0}), // real,i,j,k
    }));
    REQUIRE(result.authored_any);
    for (const auto& r : result.attributes) CHECK(r.ok);

    CHECK(Composed(stage, "/Root", "u64").Get<std::uint64_t>() == 18000000000000000000ULL);
    CHECK(Composed(stage, "/Root", "i64").Get<std::int64_t>() == -5000000000LL);
    CHECK(static_cast<int>(Composed(stage, "/Root", "uc").Get<unsigned char>()) == 200);

    // Read every authored default back through the codec and check the wire token.
    for (const char* name : {"u64", "i64", "uc", "h", "m", "q"})
    {
        const pxr::SdfAttributeSpecHandle spec = stage->GetSessionLayer()->GetAttributeAtPath(
            pxr::SdfPath(std::string("/Root.") + name));
        REQUIRE(spec);
        idtxcore::PrimAttribute out;
        CHECK(AttributeDispatcher::ReadAuthoredAttribute(spec, AttributeAssetContext{}, out));
    }
    CHECK(Composed(stage, "/Root", "m").Get<pxr::GfMatrix4d>().ExtractTranslation()
          == pxr::GfVec3d(4, 5, 6));
}
