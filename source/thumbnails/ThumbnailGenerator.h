/**
 * @file ThumbnailGenerator.h
 * @brief Abstract strategy for turning an on-disk USD file into a small
 *        preview image.
 *
 * The interface is intentionally minimal so multiple backends can coexist:
 *   - @c PlaceholderThumbnailGenerator (default, ships now): reads basic
 *     metadata from the SdfLayer and writes a deterministic PNG.
 *   - @c UsdImagingThumbnailGenerator (future, opt-in): real Hydra render.
 *     Requires an OpenUSD build with imaging enabled.
 *
 * Implementations must be thread-safe: the ThumbnailWorker calls @c Generate
 * from a dedicated background thread and may in the future move to a pool.
 */
#pragma once

#include <filesystem>
#include <string>

namespace idtx
{
namespace thumbnails
{

class ThumbnailGenerator
{
public:
    virtual ~ThumbnailGenerator() = default;

    /**
     * @brief Produce a thumbnail image for @p usd_file at @p out_path.
     *
     * @param usd_file   Absolute path to an existing @c .usd / @c .usda /
     *                   @c .usdc / @c .usdz file.
     * @param out_path   Absolute path of the image file to write. The parent
     *                   directory is guaranteed to exist. Implementations
     *                   should write atomically (write to a temp file, then
     *                   rename) whenever practical.
     * @param out_error  On failure, filled with a short, non-sensitive error
     *                   message suitable for logging and for the HTTP response.
     * @return @c true iff the image was written successfully.
     */
    virtual bool Generate(const std::filesystem::path& usd_file,
                          const std::filesystem::path& out_path,
                          std::string& out_error) = 0;

    /**
     * @brief File extension (including the leading dot) that this generator
     *        produces, e.g. @c ".png" or @c ".jpg".
     *
     * The controller uses this to derive the thumbnail filename from the USD
     * filename (same basename, image extension).
     */
    virtual const char* Extension() const noexcept = 0;
};

} // namespace thumbnails
} // namespace idtx