/**
 * @file AuthController.h
 * @brief HTTP REST adapter for the authentication endpoint.
 *
 * Performs an OAuth2 Resource Owner Password Credentials (ROPC) flow against
 * the configured IdP token endpoint, using credentials supplied by the client
 * in the request body, and returns the resulting access token (and refresh
 * token, if provided by the IdP) to the caller.
 */
#pragma once

#include <string>

#include <crow/http_request.h>
#include <crow/http_response.h>

#include <idtx/utils/Logger.h>

class AuthController
{
    IDTX_LOG_CATEGORY("AuthController")

public:
    /**
     * @brief Constructs the AuthController with OAuth2 client configuration.
     *
     * @param tokenUrl     The OAuth2 token endpoint URL of the IdP.
     * @param clientId     The OAuth2 client identifier.
     * @param clientSecret Optional OAuth2 client secret. Empty string if the
     *                     client is public.
     * @param scope        Optional OAuth2 scope(s). Empty string for default.
     */
    AuthController(std::string tokenUrl,
                   std::string clientId,
                   std::string clientSecret,
                   std::string scope);
    ~AuthController() = default;

    /**
     * @brief Handles POST /api/v1/auth/login.
     *
     * Expects a JSON body of the form:
     * @code{.json}
     * { "username": "alice", "password": "secret" }
     * @endcode
     *
     * On success returns 200 with:
     * @code{.json}
     * {
     *   "access_token": "...",
     *   "token_type": "Bearer",
     *   "expires_in": 3600,
     *   "refresh_token": "..."  // only if provided by IdP
     * }
     * @endcode
     */
    crow::response Login(const crow::request& req);

private:
    std::string m_tokenUrl_;
    std::string m_clientId_;
    std::string m_clientSecret_;
    std::string m_scope_;
};