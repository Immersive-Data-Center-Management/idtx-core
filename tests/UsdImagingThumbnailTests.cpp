// tests/UsdImagingThumbnailTests.cpp
//
// Exercises the Hydra-based UsdImagingThumbnailGenerator. The whole body is
// compiled out unless the project was built with imaging support
// (`scons tests=1 imaging=1`), which is the only configuration where the
// generator (and the USD imaging libraries it links against) exist.
//
// Note on semantics: the generator falls back to the metadata-only
// PlaceholderThumbnailGenerator on any render failure, and the placeholder
// also emits a valid PNG. This test therefore asserts "a non-trivial PNG was
// produced" rather than "the GPU-less Storm render specifically succeeded" -
// a real rendered preview vs. the gradient placeholder is verified by the
// container smoke test, not here.

#include "thirdparty/doctest/doctest.h"

#if defined(IDTX_ENABLE_IMAGING)

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/sphere.h>

#include "thumbnails/UsdImagingThumbnailGenerator.h"

namespace
{
namespace fs = std::filesystem;

// Author a stage that contains real, bounded geometry (a sphere) so the
// synthetic-camera framing has a non-empty bounding box to work with. The
// shared UsdFixture only authors empty Xforms, which produce an empty bbox.
fs::path WriteSphereStage()
{
    const fs::path dst = fs::temp_directory_path() /
        ("idtx_imaging_sphere_" +
         std::to_string(static_cast<unsigned long long>(
             std::chrono::steady_clock::now().time_since_epoch().count())) +
         ".usda");

    std::error_code ec;
    fs::remove(dst, ec);

    auto stage = pxr::UsdStage::CreateNew(dst.string());
    REQUIRE(stage);

    auto sphere = pxr::UsdGeomSphere::Define(stage, pxr::SdfPath("/Root/Sphere"));
    REQUIRE(sphere);
    sphere.GetRadiusAttr().Set(2.0);

    stage->SetDefaultPrim(stage->GetPrimAtPath(pxr::SdfPath("/Root/Sphere")));
    stage->GetRootLayer()->Save();
    return dst;
}

// PNG 8-byte signature check.
bool HasPngSignature(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::array<unsigned char, 8> sig{};
    in.read(reinterpret_cast<char*>(sig.data()),
            static_cast<std::streamsize>(sig.size()));
    if (in.gcount() != static_cast<std::streamsize>(sig.size())) return false;
    static constexpr std::array<unsigned char, 8> kPng = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return sig == kPng;
}

} // namespace

TEST_CASE("UsdImagingThumbnailGenerator emits a non-trivial PNG")
{
    const fs::path usd = WriteSphereStage();
    const fs::path out = fs::temp_directory_path() / "idtx_imaging_out.png";

    std::error_code ec;
    fs::remove(out, ec);

    idtx::thumbnails::UsdImagingThumbnailGenerator generator(64);

    std::string err;
    const bool ok = generator.Generate(usd, out, err);

    CHECK_MESSAGE(ok, "Generate failed: " << err);
    CHECK(fs::exists(out));
    CHECK(HasPngSignature(out));
    CHECK(fs::file_size(out, ec) > 8u);
    CHECK_FALSE(ec);

    CHECK(std::string(generator.Extension()) == ".png");

    fs::remove(usd, ec);
    fs::remove(out, ec);
}

#endif // IDTX_ENABLE_IMAGING
