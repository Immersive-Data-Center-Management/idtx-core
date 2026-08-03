/**
 * @file AuthDto.h
 * @brief Request/response DTOs for the authentication endpoint.
 *
 * Centralizing the field names here ensures every JSON key used by the auth
 * surface — both inbound (`LoginRequest`), upstream (`OAuthTokenResponse`)
 * and outbound (`LoginResponse`) — is declared in exactly one place.
 */
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace idtx
{
namespace dto
{
    // ---------------------------------------------------------------------
    // Inbound: POST /api/v1/auth/login body
    // ---------------------------------------------------------------------

    /**
     * @brief Body of POST /api/v1/auth/login.
     *
     * @code{.json}
     * { "username": "alice", "password": "secret" }
     * @endcode
     */
    struct LoginRequest
    {
        std::string username;
        std::string password;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LoginRequest, username, password)

    // ---------------------------------------------------------------------
    // Upstream: OAuth2 token endpoint response (RFC 6749 §5.1)
    // ---------------------------------------------------------------------

    /**
     * @brief Typed view of the JSON document returned by the IdP's token
     * endpoint. Only @c access_token is required by the spec; everything
     * else is optional and therefore wrapped in @c std::optional.
     *
     * Wrong-typed *optional* fields are silently ignored so a quirky IdP
     * (e.g. one that returns @c expires_in as a JSON string) doesn't break
     * the whole login flow — they are simply left as @c std::nullopt.
     * A missing or wrong-typed @c access_token, on the other hand, raises
     * a @c nlohmann::json::exception which the controller turns into a
     * 502 @c idp_invalid_response.
     */
    struct OAuthTokenResponse
    {
        std::string                 access_token;
        std::optional<std::string>  token_type;
        std::optional<long long>    expires_in;
        std::optional<std::string>  refresh_token;
        std::optional<std::string>  scope;
        std::optional<std::string>  id_token;
    };

    inline void from_json(const nlohmann::json& j, OAuthTokenResponse& r)
    {
        // Required: access_token. `at(...).get_to(...)` throws on missing
        // key (out_of_range) or wrong type (type_error).
        j.at("access_token").get_to(r.access_token);

        const auto get_string = [&](const char* key, std::optional<std::string>& out) {
            if (auto it = j.find(key); it != j.end() && it->is_string())
            {
                out.emplace(it->get<std::string>());
            }
        };
        const auto get_int = [&](const char* key, std::optional<long long>& out) {
            if (auto it = j.find(key); it != j.end() && it->is_number_integer())
            {
                out.emplace(it->get<long long>());
            }
        };

        get_string("token_type",    r.token_type);
        get_int   ("expires_in",    r.expires_in);
        get_string("refresh_token", r.refresh_token);
        get_string("scope",         r.scope);
        get_string("id_token",      r.id_token);
    }

    // ---------------------------------------------------------------------
    // Outbound: POST /api/v1/auth/login success response
    // ---------------------------------------------------------------------

    /**
     * @brief What the server returns to the client on a successful login.
     *
     * @code{.json}
     * {
     *   "access_token": "...",
     *   "token_type":   "Bearer",
     *   "expires_in":   3600,
     *   "refresh_token": "...",
     *   "scope":        "..."
     * }
     * @endcode
     *
     * @c expires_in, @c refresh_token and @c scope are emitted only when
     * the IdP supplied them.
     */
    struct LoginResponse
    {
        std::string                 access_token;
        std::string                 token_type;       // always set, defaults to "Bearer"
        std::optional<long long>    expires_in;
        std::optional<std::string>  refresh_token;
        std::optional<std::string>  scope;
    };

    inline void to_json(nlohmann::json& j, const LoginResponse& r)
    {
        j = nlohmann::json{
            {"access_token", r.access_token},
            {"token_type",   r.token_type}
        };
        if (r.expires_in)    j["expires_in"]    = *r.expires_in;
        if (r.refresh_token) j["refresh_token"] = *r.refresh_token;
        if (r.scope)         j["scope"]         = *r.scope;
    }
}
}