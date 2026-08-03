/**
 * @file JwtAuthHandler.h
 * @brief CROW Middleware handler for JWT based authentication
 */
#pragma once

#include <crow.h>
#include <unordered_set>
#include <jwt-cpp/jwt.h>

#include "OidcConfiguration.h"

#include "idtx/utils/Logger.h"

namespace idtx
{
namespace middleware
{
    struct JwtAuthHandler
    {
        IDTX_LOG_CATEGORY("JwtAuthHandler")
        
        /**
        * @brief Configures the authentication middleware using OIDC metadata.
        *
        * Sets the wellknown configuration url, expected audiences, allowed algorithms and clock‑skew leeway.
        * Then attempts initialization by calling initialize().
        * Logs an error if initialization fails or if an exception occurs during setup.
        *
        * @param oidcConfig   The OIDC configuration data (wellknown url and audiences).
        * @param allowedAlgs Set of allowed jwt algorithms (defaults to RS256 and PS256).
        * @param leeway     Allowed clock skew when validating token timestamps.
        */
        void configure(const OidcConfiguration& oidcConfig,
                       std::unordered_set<std::string> allowedAlgs = {"RS256", "PS256"}, std::chrono::seconds leeway = std::chrono::seconds(60));
        
        /**
         * @brief The contract for a struct/class to be used as CROW middleware requires this type to be available. The
         * contents of this type are opaque to the CROW middleware dispatcher
         */
        struct context {
            bool authenticated = false;
            std::string message;
            std::string sub;
        };
    
        /**
        * @brief Processes an incoming request before it reaches the route handler.
        *
        * Extracts and validates the jwt token.
        * It updates the context with authentication status and terminates the request early with an error response when
        * authentication fails.
        *
        * @param req  The incoming HTTP request.
        * @param res  The HTTP response.
        * @param ctx  The per request authentication context to populate.
        */
        void before_handle(crow::request& req, crow::response& res, context& ctx);
    
        /**
         * @brief Executes after the route handler has processed the request.
         * 
         * @param req  The original HTTP request.
         * @param res  The HTTP response after route handling.
         * @param ctx  The per request authentication context.
         */
        void after_handle(crow::request& req, crow::response& res, context& ctx);
        
        JwtAuthHandler();
        ~JwtAuthHandler();
        
    private:
        /**
         * Structure to communicate authentication request results
         */
        struct AuthResult {
            bool success{false};
            std::string message;
            std::string sub;
        };

        /**
         * Extract the auth bearer token from an incoming request
         * @param req 
         * @return 
         */
        std::optional<std::string> getBearerToken(const crow::request& req) const;

        /**
         * @brief Checks if the JWT passed with the incoming request authenticates the usage of this service 
         * @param req 
         * @return 
         */
        AuthResult authenticate(const crow::request& req);

        /**
         * @brief Checks if the JWT string authenticates the usage of this service
         * @param token 
         * @return 
         */
        AuthResult authenticate(const std::string& token);

        /**
         * @brief Refresh the JWKS entries from OIDC provider endpoint
         * @return 
         */
        bool refreshJwks();
        
        template <typename Traits>
        bool tryVerify(const jwt::decoded_jwt<Traits>& decoded, const std::string& alg, const std::string& kid, std::string& out_error);
        
        bool is_enabled = false;
        std::string well_known_url;
        std::string issuer_;
        std::string jwks_uri;
        std::vector<std::string> audiences_;
        std::unordered_set<std::string> allowed_algs;
        std::chrono::seconds leeway_{60};
        std::string last_error_;
        std::unique_ptr<class JwksCache> cache_;
    };
}
}
