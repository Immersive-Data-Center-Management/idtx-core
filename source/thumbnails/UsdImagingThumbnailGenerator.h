/**
 * @file UsdImagingThumbnailGenerator.h
 * @brief ThumbnailGenerator that renders the USD stage into a real preview
 *        image using Hydra (Storm) in a headless container.
 *
 * Unlike @c PlaceholderThumbnailGenerator (which only paints file metadata),
 * this generator opens the stage and renders it with @c UsdImagingGL via
 * @c UsdAppUtilsFrameRecorder. It picks the first authored @c UsdGeomCamera in
 * the stage if one exists; otherwise it authors a synthetic preview camera into
 * the stage's *session layer*, positioned 45 degrees above the content and
 * framed to the outermost extent of the stage's prims.
 *
 * Because the containers this runs in are headless and GPU-less, rendering goes
 * through software OpenGL (Mesa llvmpipe, optionally via OSMesa). See the
 * SConstruct / Dockerfile changes gated behind @c IDTX_ENABLE_IMAGING.
 *
 * The whole class is compiled only when the project is built with imaging
 * support (@c scons imaging=1, which defines @c IDTX_ENABLE_IMAGING). When
 * imaging is not compiled in, this header declares nothing and
 * @c ApplicationContext falls back to the placeholder generator.
 *
 * Runtime robustness: any failure (stage cannot open, no GL context, empty
 * scene, render error) transparently falls back to an owned
 * @c PlaceholderThumbnailGenerator so a valid thumbnail is always produced.
 */
#pragma once

#if defined(IDTX_ENABLE_IMAGING)

#include <cstdint>
#include <filesystem>
#include <string>

#include <idtx/utils/Logger.h>

#include "thumbnails/PlaceholderThumbnailGenerator.h"
#include "thumbnails/ThumbnailGenerator.h"

namespace idtx
{
namespace thumbnails
{

class UsdImagingThumbnailGenerator final : public ThumbnailGenerator
{
    IDTX_LOG_CATEGORY("UsdImagingThumbnailGenerator")

public:
    explicit UsdImagingThumbnailGenerator(std::uint32_t size = 256) noexcept
        : m_size_(size == 0 ? 256u : size)
        , m_fallback_(m_size_)
    {}

    bool Generate(const std::filesystem::path& usd_file,
                  const std::filesystem::path& out_path,
                  std::string& out_error) override;

    const char* Extension() const noexcept override { return ".png"; }

private:
    std::uint32_t                 m_size_;
    PlaceholderThumbnailGenerator m_fallback_;
};

} // namespace thumbnails
} // namespace idtx

#endif // IDTX_ENABLE_IMAGING
