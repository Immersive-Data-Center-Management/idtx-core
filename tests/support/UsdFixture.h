// tests/support/UsdFixture.h — programmatically author a minimal, valid USDA
// scene the whole test suite operates on.
//
// We deliberately don't check a USD binary into git. Every TestServer creates
// its own uploads/ directory and asks UsdFixture to (re)create the canonical
// cube.usda inside it, so tests never collide on file system state.

#pragma once

#include <filesystem>
#include <string>

namespace idtx::tests
{

/**
 * @brief Write a minimal `Xform "Root" { Xform "Cube" { xformOp:translate = (0,0,0) } }`
 *        scene to @p dst. Overwrites any existing file. Throws on failure.
 */
void WriteCubeUsda(const std::filesystem::path& dst);

/**
 * @brief Read back the resolved translate of @p prim_path from the USD file
 *        at @p usd_file. Returns true on success.
 *
 * Uses UsdGeomXformCommonAPI so the reader sees whatever the server
 * ultimately authored, regardless of the exact XformOp stack layout.
 */
bool ReadTranslate(const std::filesystem::path& usd_file,
                   const std::string& prim_path,
                   double& out_x, double& out_y, double& out_z);

/**
 * @brief Author a fresh `Xform "Root" { Xform "Cube" { xformOp:translate = (x,y,z) } }`
 *        scene as a raw USDA byte buffer.
 *
 * The result is suitable as the body of a multipart upload request in
 * @c StageReloadTests. Nothing is written to disk — the caller decides
 * what to do with the bytes.
 *
 * @param x  Translate X component authored on `/Root/Cube`.
 * @param y  Translate Y component authored on `/Root/Cube`.
 * @param z  Translate Z component authored on `/Root/Cube`.
 * @return   A UTF-8 encoded USDA blob.
 */
std::string BuildCubeUsdaBytes(double x, double y, double z);

} // namespace idtx::tests
