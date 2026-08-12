#include "AuthController.h"

#include <utility>
#include <variant>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "dto/AuthDto.h"
#include "dto/ErrorResponse.h"
#include "dto/JsonDto.h"
#include "middleware/RateLimitHandler.h"
#include "utils/HttpClient.h"
#include "utils/SecurityAuditLog.h"

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
    // Derive the client key up-front so every audit event and throttler
    // interaction uses the same source identity as the rate-limiting
    // middleware (proxy-aware when behind an ingress/LB).
    const std::string clientIp =
        idtx::middleware::RateLimitHandler::ResolveClientIp(req, m_trustForwardedFor_);
    const std::string userAgent = req.get_header_value("User-Agent");

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
        idtx::security::SecurityAuditLog::Record({
            idtx::security::AuditEvent::LoginFailed, clientIp, "POST",
            "/api/v1/auth/login", userAgent, std::nullopt, 400,
            "missing username or password", std::nullopt, std::nullopt});
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

        // Surface the IdP's own error response to aid diagnosis (e.g.
        // "invalid_client", "invalid_grant", "unauthorized_client"). The OAuth2
        // error body is safe to log: per RFC 6749 it carries only error codes /
        // human-readable descriptions and never the submitted credentials or
        // client secret. We still guard against an unexpectedly large body so a
        // misbehaving IdP cannot flood the logs.
        if (!response.body.empty())
        {
            constexpr std::size_t kMaxIdpBodyLog = 2048;
            const std::string_view body{response.body};
            IDTX_LOG(IDTX_INFO, "IdP error response (HTTP {}): {}{}",
                     response.code,
                     body.substr(0, kMaxIdpBodyLog),
                     body.size() > kMaxIdpBodyLog ? "...(truncated)" : "");
        }

        // 4xx from the IdP is treated as an authentication failure; everything
        // else is reported as a bad gateway so the client can distinguish.
        if (response.code >= 400 && response.code < 500)
        {
            // Feed the failure into the shared throttler. When this failure
            // crosses the configured threshold the source becomes locked out
            // and subsequent attempts are short-circuited by the middleware.
            bool lockedOut = false;
            if (m_loginThrottler_)
            {
                lockedOut = m_loginThrottler_->RecordFailure(clientIp);
            }

            idtx::security::SecurityAuditLog::Record({
                idtx::security::AuditEvent::LoginFailed, clientIp, "POST",
                "/api/v1/auth/login", userAgent, creds.username, 401,
                "invalid credentials", std::nullopt, std::nullopt});

            if (lockedOut)
            {
                idtx::security::SecurityAuditLog::Record({
                    idtx::security::AuditEvent::LoginLockout, clientIp, "POST",
                    "/api/v1/auth/login", userAgent, creds.username, 401,
                    "source locked out after repeated failures",
                    std::nullopt, std::nullopt});
            }

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

    // Successful authentication clears any accumulated failure streak / lockout
    // for this source so a legitimate user is not penalised after recovering.
    if (m_loginThrottler_)
    {
        m_loginThrottler_->RecordSuccess(clientIp);
    }

    crow::response res(200, json(out).dump());
    res.set_header("Content-Type", "application/json");
    res.set_header("Cache-Control", "no-store");
    res.set_header("Pragma", "no-cache");
    return res;
}
