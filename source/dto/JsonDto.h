/**
 * @file JsonDto.h
 * @brief Generic helper for parsing typed JSON request bodies.
 *
 * Intended usage in a controller:
 * @code
 *   auto parsed = idtx::dto::parse_json_dto<MyRequest>(req);
 *   if (auto* err = std::get_if<crow::response>(&parsed)) return std::move(*err);
 *   const MyRequest& dto = std::get<MyRequest>(parsed);
 * @endcode
 *
 * The helper relies on @c nlohmann::json's ADL hooks. A request DTO must
 * either:
 *   - provide a @c from_json(const nlohmann::json&, T&) overload, or
 *   - be defined with @c NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE.
 *
 * Parsing or type errors are translated to a uniform 400 response built with
 * @c idtx::dto::make_error so all controllers expose the same error contract.
 */
#pragma once

#include <string>
#include <variant>

#include <crow/http_request.h>
#include <crow/http_response.h>
#include <nlohmann/json.hpp>

#include "ErrorResponse.h"

namespace idtx
{
namespace dto
{
    /**
     * @brief Parse the JSON body of @p req into a value of type @c T.
     *
     * @tparam T  A request DTO type with an associated nlohmann::json @c from_json
     *            (provided either manually or via @c NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE).
     * @param req The incoming Crow request whose @c body is parsed.
     * @return Either the parsed @c T or a @c crow::response carrying a 400
     *         error in the standard error shape.
     */
    template <typename T>
    std::variant<T, crow::response> parse_json_dto(const crow::request& req)
    {
        nlohmann::json parsed;
        try
        {
            parsed = nlohmann::json::parse(req.body);
        }
        catch (const nlohmann::json::parse_error& e)
        {
            return make_error(400, "invalid_request",
                              std::string("Request body is not valid JSON: ") + e.what());
        }

        if (!parsed.is_object())
        {
            return make_error(400, "invalid_request",
                              "Request body must be a JSON object.");
        }

        try
        {
            return parsed.get<T>();
        }
        catch (const nlohmann::json::out_of_range& e)
        {
            // A required field is missing.
            return make_error(400, "invalid_request",
                              std::string("Missing required field: ") + e.what());
        }
        catch (const nlohmann::json::type_error& e)
        {
            // A field has the wrong JSON type.
            return make_error(400, "invalid_request",
                              std::string("Invalid field type: ") + e.what());
        }
        catch (const std::exception& e)
        {
            return make_error(400, "invalid_request",
                              std::string("Invalid request body: ") + e.what());
        }
    }
}
}