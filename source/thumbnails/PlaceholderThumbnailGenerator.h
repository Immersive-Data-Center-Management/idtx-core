/**
 * @file PlaceholderThumbnailGenerator.h
 * @brief Default ThumbnailGenerator implementation.
 *
 * OpenUSD is compiled without imaging support in this project (see
 * @c scons/openusd.py: @c --no-imaging, @c --no-usdview), so we cannot run
 * Hydra to produce a real 3D preview. Instead this generator reads a small
 * amount of metadata from the USD layer (default prim, root prim count) and
 * paints a deterministic gradient tile that:
 *   - varies per file (colour derived from a hash of the filename), so
 *     thumbnails are visually distinguishable in a file browser;
 *   - is small (default 256x256), fast to produce, and never fails on files
 *     that USD cannot open (falls back to a monochrome tile);
 *   - is written as a valid PNG through @c MiniPng.
 *
 * A future @c UsdImagingThumbnailGenerator can transparently replace this
 * class without changes to the controller — see @c ThumbnailGenerator.h.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "thumbnails/ThumbnailGenerator.h"

namespace idtx
{
namespace thumbnails
{

class PlaceholderThumbnailGenerator final : public ThumbnailGenerator
{
public:
    explicit PlaceholderThumbnailGenerator(std::uint32_t size = 256) noexcept
        : m_size_(size == 0 ? 256u : size)
    {}

    bool Generate(const std::filesystem::path& usd_file,
                  const std::filesystem::path& out_path,
                  std::string& out_error) override;

    const char* Extension() const noexcept override { return ".png"; }

private:
    std::uint32_t m_size_;
};

} // namespace thumbnails
} // namespace idtx