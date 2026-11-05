// tests/ReferencePathResolverTests.cpp
//
// Unit tests of ReferencePathResolver: classification (absolute / stage-relative
// / search-path), the uploads-root escape guard, existence + USD checks, the
// <prim> suffix split, and the sidecar-vs-broadcast re-anchoring. No server or
// stage is involved; real temp files stand in for the uploads tree.

#include "thirdparty/doctest/doctest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "session/ReferencePathResolver.h"

using idtx::session::ReferencePathResolver;
namespace fs = std::filesystem;

namespace
{

using Status = ReferencePathResolver::Status;

// A disposable uploads tree:
//   <root>/uploads/              <- uploads root (root download folder)
//   <root>/uploads/scene.usda    <- the open stage's root layer
//   <root>/uploads/kit/chair.usda<- a reference target
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
        Touch(uploads / "kit" / "chair.usda");
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

    Status Resolve(const std::string& ref, ReferencePathResolver::Resolved& out) const
    {
        return ReferencePathResolver::Resolve(ref, uploads, root_dir, sidecar_dir,
                                              root_file, out);
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
    // Authored is relative to the sidecar dir (sessions/), climbing out with '..'.
    CHECK(r.authored_asset == "../uploads/kit/chair.usda");
    CHECK(r.prim_path.IsEmpty());
}

TEST_CASE("stage-relative ./ path resolves against the root layer dir")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("./kit/chair.usda", r) == Status::Ok);
    CHECK(r.broadcast_path == "./kit/chair.usda");
    CHECK(r.authored_asset == "../uploads/kit/chair.usda");
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
    CHECK(t.Resolve("/kit/missing.usda", r) == Status::NotFound);
}

TEST_CASE("a non-USD target is rejected")
{
    Tree t;
    Tree::Touch(t.uploads / "kit" / "note.txt");
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/note.txt", r) == Status::NotAUsdFile);
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
    CHECK(r.authored_asset == "../uploads/kit/chair.usda"); // asset only, no suffix
    CHECK(r.broadcast_path == "./kit/chair.usda</Geom/Seat>");
}

TEST_CASE("a malformed <prim> suffix is rejected")
{
    Tree t;
    ReferencePathResolver::Resolved r;
    CHECK(t.Resolve("/kit/chair.usda<not-a-path>", r) == Status::InvalidPrimPath);
}

TEST_CASE("AuthoredToBroadcast reverses the sidecar anchoring")
{
    Tree t;
    const std::string b = ReferencePathResolver::AuthoredToBroadcast(
        "../uploads/kit/chair.usda", t.sidecar_dir, t.root_dir);
    CHECK(b == "./kit/chair.usda");
}
