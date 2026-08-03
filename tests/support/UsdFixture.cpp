#include "UsdFixture.h"

#include <fstream>
#include <stdexcept>
#include <string>

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdGeom/xformable.h>

namespace idtx::tests
{

void WriteCubeUsda(const std::filesystem::path& dst)
{
    // Make sure the parent directory exists.
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    // If the file already exists, remove it so UsdStage::CreateNew succeeds.
    if (std::filesystem::exists(dst, ec))
    {
        std::filesystem::remove(dst, ec);
    }

    // Author the stage programmatically. Using UsdStage::CreateNew with a .usda
    // extension makes the resulting layer text-based and easy to diff.
    auto stage = pxr::UsdStage::CreateNew(dst.string());
    if (!stage)
    {
        throw std::runtime_error("Failed to create USD stage at " + dst.string());
    }

    // /Root
    auto root_xform = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));
    if (!root_xform) throw std::runtime_error("Failed to define /Root Xform.");

    // /Root/Cube  (we keep it an Xform so we don't need UsdGeom's Cube schema)
    auto cube_xform = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root/Cube"));
    if (!cube_xform) throw std::runtime_error("Failed to define /Root/Cube Xform.");

    // Seed the translate op with (0,0,0) via the common API so subsequent
    // reads through the same API are guaranteed to work.
    pxr::UsdGeomXformCommonAPI api(cube_xform.GetPrim());
    api.SetTranslate(pxr::GfVec3d(0.0, 0.0, 0.0));

    stage->SetDefaultPrim(root_xform.GetPrim());
    stage->GetRootLayer()->Save();
}

std::string BuildCubeUsdaBytes(double x, double y, double z)
{
    // Author into a temp .usda so we can round-trip through USD's official
    // serialiser (matches what WriteCubeUsda produces byte-for-byte modulo
    // the translate value). We then slurp the file contents back and delete
    // the temp file.
    namespace fs = std::filesystem;

    const fs::path tmp = fs::temp_directory_path() /
        ("idtx_cube_usda_" + std::to_string(std::hash<std::string>{}(
            std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z))) +
         ".usda");

    // Clean any stale leftover.
    std::error_code ec;
    fs::remove(tmp, ec);

    auto stage = pxr::UsdStage::CreateNew(tmp.string());
    if (!stage)
        throw std::runtime_error("Failed to create USD stage at " + tmp.string());

    auto root_xform = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));
    if (!root_xform) throw std::runtime_error("Failed to define /Root Xform.");

    auto cube_xform = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root/Cube"));
    if (!cube_xform) throw std::runtime_error("Failed to define /Root/Cube Xform.");

    pxr::UsdGeomXformCommonAPI api(cube_xform.GetPrim());
    api.SetTranslate(pxr::GfVec3d(x, y, z));

    stage->SetDefaultPrim(root_xform.GetPrim());
    stage->GetRootLayer()->Save();

    std::ifstream in(tmp, std::ios::binary);
    if (!in) throw std::runtime_error("Failed to read back " + tmp.string());
    std::string bytes((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    in.close();

    fs::remove(tmp, ec);
    return bytes;
}

bool ReadTranslate(const std::filesystem::path& usd_file,
                   const std::string& prim_path,
                   double& out_x, double& out_y, double& out_z)
{
    auto stage = pxr::UsdStage::Open(usd_file.string());
    if (!stage) return false;

    if (!pxr::SdfPath::IsValidPathString(prim_path)) return false;
    auto prim = stage->GetPrimAtPath(pxr::SdfPath(prim_path));
    if (!prim) return false;

    pxr::UsdGeomXformCommonAPI api(prim);
    if (!api) return false;

    pxr::GfVec3d t;
    pxr::GfVec3f r, s, pivot;
    pxr::UsdGeomXformCommonAPI::RotationOrder order;
    if (!api.GetXformVectorsByAccumulation(&t, &r, &s, &pivot, &order,
                                           pxr::UsdTimeCode::Default()))
    {
        return false;
    }
    out_x = t[0];
    out_y = t[1];
    out_z = t[2];
    return true;
}

} // namespace idtx::tests