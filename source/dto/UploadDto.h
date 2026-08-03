/**
 * @file UploadDto.h
 * @brief Response DTO for the file upload endpoint.
 *
 * The upload request itself is multipart/form-data (not JSON) and is parsed
 * directly by the controller via crow::multipart. Only the response is
 * serialised through this DTO so clients get a stable, documented shape.
 */
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace idtx
{
namespace dto
{
    /**
     * @brief Status of the thumbnail-generation side effect that runs after a
     *        successful upload.
     *
     * Serialized to/from JSON as a snake_case string:
     *   - @c Queued    -> @c "queued"    (thumbnail job scheduled asynchronously)
     *   - @c Generated -> @c "generated" (generated synchronously and successfully)
     *   - @c Skipped   -> @c "skipped"   (feature disabled or already up-to-date)
     *   - @c Failed    -> @c "failed"    (generator ran but returned an error;
     *                                     the uploaded USD file itself is fine)
     */
    enum class ThumbnailStatus
    {
        Queued,
        Generated,
        Skipped,
        Failed,
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(ThumbnailStatus, {
        { ThumbnailStatus::Queued,    "queued"    },
        { ThumbnailStatus::Generated, "generated" },
        { ThumbnailStatus::Skipped,   "skipped"   },
        { ThumbnailStatus::Failed,    "failed"    },
    })

    struct ThumbnailInfo
    {
        ThumbnailStatus status = ThumbnailStatus::Skipped;
        /// Path (relative to the uploads root) at which the thumbnail image is
        /// / will be stored. Empty when the thumbnail generator is disabled.
        std::string path;
        /// Human-readable error message when @c status == @c Failed. Empty
        /// otherwise. Never contains sensitive server paths.
        std::string error;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ThumbnailInfo, status, path, error)

    /**
     * @brief Body of a successful @c POST /api/v1/upload response.
     *
     * @code{.json}
     * {
     *   "filepath": "scenes/foo.usda",
     *   "filename": "foo.usda",
     *   "directory": "scenes",
     *   "size": 12345,
     *   "thumbnail": { "status": "queued", "path": "scenes/thumbs/foo.png" }
     * }
     * @endcode
     */
    struct UploadResponse
    {
        std::string   filepath;
        std::string   filename;
        std::string   directory;
        std::uint64_t size = 0;
        ThumbnailInfo thumbnail;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(UploadResponse,
                                                   filepath, filename,
                                                   directory, size, thumbnail)
}
}