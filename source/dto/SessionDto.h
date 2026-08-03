/**
 * @file SessionDto.h
 * @brief Request DTOs for the session endpoints.
 */
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace idtx
{
namespace dto
{
    /**
     * @brief Editing/runtime mode a session is created in.
     *
     * Serialized to/from JSON as a snake_case string:
     *   - @c SingleEdit           -> @c "single_edit"
     *   - @c SingleRuntime        -> @c "single_runtime"
     *   - @c CollaborativeEdit    -> @c "collaborative_edit"
     *   - @c CollaborativeRuntime -> @c "collaborative_runtime"
     *
     * @note Deserializing an unknown string silently falls back to the first
     *       entry below (@c SingleEdit). This is the documented behavior of
     *       @c NLOHMANN_JSON_SERIALIZE_ENUM.
     */
    enum class SessionMode
    {
        SingleEdit,
        SingleRuntime,
        CollaborativeEdit,
        CollaborativeRuntime,
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(SessionMode, {
        { SessionMode::SingleEdit,           "single_edit"           },
        { SessionMode::SingleRuntime,        "single_runtime"        },
        { SessionMode::CollaborativeEdit,    "collaborative_edit"    },
        { SessionMode::CollaborativeRuntime, "collaborative_runtime" },
    })

    /**
     * @brief Body of POST /api/v1/sessions.
     *
     * The @c mode field is optional and defaults to
     * @c SessionMode::SingleEdit when omitted.
     *
     * @code{.json}
     * { "usd_file": "scenes/foo.usda", "mode": "collaborative_edit" }
     * @endcode
     */
    struct CreateSessionRequest
    {
        std::string usd_file;
        SessionMode mode = SessionMode::SingleEdit;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CreateSessionRequest, usd_file, mode)
}
}