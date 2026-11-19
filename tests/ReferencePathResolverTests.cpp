// tests/ReferencePathResolverTests.cpp
//
// Unit tests of ReferencePathResolver: classification (absolute / stage-relative
// / search-path), the uploads-root escape guard, existence + USD checks, the
// <prim> suffix split, and the authored-vs-broadcast re-anchoring (authored is
// the absolute on-disk target; broadcast is root-layer-relative). No server or
// stage is involved; real temp files stand in for the uploads tree.

#include "thirdparty/doctest/doctest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <pxr/usd/sdf/layer.h>

#include "session/ReferencePathResolver.h"

using idtx::session::ReferencePathResolver;
namespace fs = std::filesystem;

namespace
{

using Status = ReferencePathResolver::Status;

// A disposable uploads tree:
//   <root>/uploads/              <- uploads root (root download folder)
//   <root>/uploads/scene.usda    <- the open stage's root layer
//   <root>/uploads/kit/chair.usda<- a reference target (defaultPrim + /Geom/Seat)
//   <root>/sessions/             <- sibling sidecar dir (edit target lives here)
struct Tree
{
    fs::path root;
    fs::path uploads;
    fs::path root_file;    // uploads/scene.usda
    fs::path root_dir;     // uploads
    fs::path sidecar_dir;  // sessions

    Tree()
    {
        root = fs::temp_directory_path() /
               ("idtx_refres_" + std::to_string(
                   std::hash<std::string>{}(__FILE__) ^
                   reinterpret_cast<std::uintptr_t>(this)));
        uploads     = root / "uploads";
        root_dir    = uploads;
        root_file   = uploads / "scene.usda";
        sidecar_dir = root / "sessions";

        fs::create_directories(uploads / "kit");
        fs::create_directories(sidecar_dir);
        Touch(root_file);
        // A real USD layer: defaultPrim "Geom" with a child "Seat", so the
        // resolver's defaultPrim and <prim> existence checks have something to
        // find.
        Write(uploads / "kit" / "chair.usda",
              "#usda 1.0\n"
              "(\n"
              "    defaultPrim = \"Geom\"\n"
              ")\n"
              "def Xform \"Geom\"\n"
              "{\n"
              "    def \"Seat\"\n"
              "    {\n"
              "    }\n"
              "}\n");
    }

    ~Tree()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static void Touch(const fs::path& p)
    {
        std::ofstream(p) << "#usda 1.0\n";
    }

    static void Write(const fs::path& p, const char* content)
    {
        std::ofstream(p) << content;
    }

    // The resolver normalises the target with weakly_canonical; mirror that here
    // so authored-path assertions match regardless of temp-dir symlinks.
    static std::string Canonical(const fs::path& p)
    {
        std::error_code ec;
        const fs::path c = fs::weakly_canonical(p, ec);
        return (ec ? p.lexically_normal() : c).generic_string();
    }

    Status Resolve(const std::string& ref, ReferencePathResolver::Resolved& out) const
    {
        const pxr::SdfLayerRefPtr root_layer = pxr::SdfLayer::FindOrOpen(root_file.generic_string());
        REQUIRE(root_layer);
        return ReferencePathResolver::Resolve(ref, uploads, pxr::SdfLayerHandle(root_layer),
                                              out);
    }
};

} // namespace

TEST_CASE("absolute path resolves against the uploads root")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/chair.usda", r) == Status::Ok);
    // Broadcast is relative to the root layer dir (uploads/).
    CHECK(r.broadcast_path == "./kit/chair.usda");
    // Authored is the absolute on-disk target under the uploads root.
    CHECK(r.authored_asset == Tree::Canonical(t.uploads / "kit" / "chair.usda"));
    CHECK(r.prim_path.IsEmpty());
}

TEST_CASE("stage-relative ./ path resolves against the root layer dir")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("./kit/chair.usda", r) == Status::Ok);
    CHECK(r.broadcast_path == "./kit/chair.usda");
    CHECK(r.authored_asset == Tree::Canonical(t.uploads / "kit" / "chair.usda"));
}

TEST_CASE("bare search-path is rejected")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("kit/chair.usda", r) == Status::SearchPathRejected);
}

TEST_CASE("a ../ path that escapes the uploads root is rejected")
{
    Tree t;
    // Create a file OUTSIDE uploads and try to reach it from the root layer dir.
    Tree::Touch(t.root / "secret.usda");
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("../secret.usda", r) == Status::OutsideRoot);
}

TEST_CASE("a missing target is rejected")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/missing.usda", r) == Status::LayerUnreadable);
}

TEST_CASE("a non-USD target is rejected")
{
    Tree t;
    Tree::Touch(t.uploads / "kit" / "note.txt");
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/note.txt", r) == Status::LayerUnreadable);
}

TEST_CASE("referencing the session's own root layer is rejected")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/scene.usda", r) == Status::SelfReference);
}

TEST_CASE("a <prim> suffix is parsed and preserved on the broadcast path")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/chair.usda</Geom/Seat>", r) == Status::Ok);
    CHECK(r.prim_path.GetString() == "/Geom/Seat");
    CHECK(r.authored_asset == Tree::Canonical(t.uploads / "kit" / "chair.usda")); // asset only, no suffix
    CHECK(r.broadcast_path == "./kit/chair.usda</Geom/Seat>");
}

TEST_CASE("a malformed <prim> suffix is rejected")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/chair.usda<not-a-path>", r) == Status::InvalidPrimPath);
}

TEST_CASE("a <prim> suffix targeting a non-existent prim is rejected")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/chair.usda</Geom/Nope>", r) == Status::PrimNotFound);
}

TEST_CASE("a layer without a defaultPrim is rejected when no <prim> is given")
{
    Tree t;
    // A valid USD layer that declares no defaultPrim.
    Tree::Touch(t.uploads / "kit" / "bare.usda");
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/bare.usda", r) == Status::NoDefaultPrim);
}

TEST_CASE("AuthoredToBroadcast re-anchors an absolute authored path")
{
    // Resolve() authors the absolute on-disk target into the sidecar; the join
    // snapshot must re-express it root-relative.
    Tree t;
    const std::string b = ReferencePathResolver::AuthoredToBroadcast(
        Tree::Canonical(t.uploads / "kit" / "chair.usda"), t.root_dir);
    CHECK(b == "./kit/chair.usda");
}
