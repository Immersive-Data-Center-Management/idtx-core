/**
 * @file ErrorResponse.h
 * @brief Shared helpers for producing uniformly-shaped JSON error responses.
 *
 * All controllers should use these helpers so that clients see a consistent
 * error contract:
 * @code{.json}
 * { "error": "<short_machine_code>", "message": "<human readable text>" }
 * @endcode
 */
#pragma once

#include <string>

#include <crow/http_response.h>
#include <nlohmann/json.hpp>

namespace idtx
{
namespace dto
{
    /**
     * @brief Build a uniformly-shaped JSON error response.
     *
     * @param code     HTTP status code.
     * @param error    A short, stable, machine-readable error code (snake_case),
     *                 e.g. "invalid_request", "not_found", "auth_unavailable".
     * @param message  A human-readable explanation. May safely contain detail
     *                 from upstream errors when not security-sensitive.
     */
    inline crow::response make_error(int code,
                                     const std::string& error,
                                     const std::string& message)
    {
        nlohmann::json body = {
            {"error",   error},
            {"message", message}
        };
        crow::response res(code, body.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    }
}
}