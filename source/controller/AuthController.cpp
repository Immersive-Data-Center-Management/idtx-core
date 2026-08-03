#include "AuthController.h"

#include <utility>
#include <variant>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "dto/AuthDto.h"
#include "dto/ErrorResponse.h"
#include "dto/JsonDto.h"
#include "utils/HttpClient.h"

using json = nlohmann::json;

AuthController::AuthController(std::string tokenUrl,
                               std::string clientId,
                               std::string clientSecret,
                               std::string scope)
    : m_tokenUrl_(std::move(tokenUrl))
    , m_clientId_(std::move(clientId))
    , m_clientSecret_(std::move(clientSecret))
    , m_scope_(std::move(scope))
{
}

crow::response AuthController::Login(const crow::request& req)
{
    // Server must be configured.
    if (m_tokenUrl_.empty() || m_clientId_.empty())
    {
        IDTX_LOG(IDTX_ERROR, "AuthController not configured (token url or client id missing)");
        return idtx::dto::make_error(503, "auth_unavailable",
                                     "Authentication is not configured on the server.");
    }

    // Parse the typed request body. Field presence and type are validated by
    // the JsonDto helper via nlohmann's from_json hook.
    auto parsed = idtx::dto::parse_json_dto<idtx::dto::LoginRequest>(req);
    if (auto* err = std::get_if<crow::response>(&parsed))
    {
        return std::move(*err);
    }
    const auto& creds = std::get<idtx::dto::LoginRequest>(parsed);

    if (creds.username.empty() || creds.password.empty())
    {
        return idtx::dto::make_error(400, "invalid_request",
                                     "'username' and 'password' must not be empty.");
    }

    // Build form-encoded payload for the OAuth2 ROPC flow.
    cpr::Payload payload{
        {"grant_type", "password"},
        {"username",   creds.username},
        {"password",   creds.password},
        {"client_id",  m_clientId_}
    };

    // If a client secret is configured, include it in the body alongside
    // client_id (most IdPs accept either body or basic-auth; body keeps the
    // request simple and works for both confidential and public clients).
    if (!m_clientSecret_.empty())
    {
        payload.Add({"client_secret", m_clientSecret_});
    }

    if (!m_scope_.empty())
    {
        payload.Add({"scope", m_scope_});
    }

    cpr::Header headers{
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Accept",       "application/json"}
    };

    IDTX_LOG(IDTX_INFO, "Requesting access token for user '{}'", creds.username);

    auto response = idtx::http::HttpClient::post(m_tokenUrl_, payload, headers);

    if (!response.ok)
    {
        IDTX_LOG(IDTX_ERROR, "Token request failed: HTTP {} {}", response.code, response.err);

        // 4xx from the IdP is treated as an authentication failure; everything
        // else is reported as a bad gateway so the client can distinguish.
        if (response.code >= 400 && response.code < 500)
        {
            return idtx::dto::make_error(401, "invalid_credentials",
                                         "Authentication failed.");
        }
        return idtx::dto::make_error(502, "idp_unreachable",
                                     "Failed to contact the identity provider.");
    }

    // Parse the IdP response into a typed DTO. The custom from_json hook
    // requires `access_token` and tolerates wrong-typed optional fields.
    idtx::dto::OAuthTokenResponse idp;
    try
    {
        json::parse(response.body).get_to(idp);
    }
    catch (const json::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "IdP response was not a valid token response: {}", e.what());
        return idtx::dto::make_error(502, "idp_invalid_response",
                                     "Identity provider returned an invalid response.");
    }

    // Re-shape into our public LoginResponse so we never leak unexpected
    // upstream fields and always present a stable contract to clients.
    idtx::dto::LoginResponse out{
        std::move(idp.access_token),
        idp.token_type.value_or("Bearer"),
        idp.expires_in,
        std::move(idp.refresh_token),
        std::move(idp.scope)
    };

    crow::response res(200, json(out).dump());
    res.set_header("Content-Type", "application/json");
    res.set_header("Cache-Control", "no-store");
    res.set_header("Pragma", "no-cache");
    return res;
}
