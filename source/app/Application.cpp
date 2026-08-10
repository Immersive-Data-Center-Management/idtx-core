#include "Application.h"

#include <charconv>
#include <chrono>

#include "middleware/JwtAuthHandler.h" 
#include "middleware/RateLimitHandler.h"

#include "utils/Environment.h"

using namespace idtx::core;

bool Application::Initialize()
{
    IDTX_LOG(IDTX_DEBUG, "Initializing IDTX Core Application");
        
    // register all routes for the server
    m_server_.RegisterRoutes();
    
    // configure the middleware of the server
    auto& corsMiddleware = m_server_.GetCrowApp().get_middleware<crow::CORSHandler>();
    corsMiddleware.global()
        .headers("Content-Type", "Authorization")
        .methods(
            crow::HTTPMethod::Post,
            crow::HTTPMethod::Get,
            crow::HTTPMethod::Put,
            crow::HTTPMethod::Delete,
            crow::HTTPMethod::Head
        )
        .origin("*");

    // Configure the in-process rate-limiting / abuse safety net. This runs
    // ahead of auth and complements the primary throttling at the ingress/LB.
    // All thresholds are overridable via environment variables so operators can
    // tune them per environment without a rebuild.
    auto& rateLimitMiddleware =
        m_server_.GetCrowApp().get_middleware<middleware::RateLimitHandler>();

    middleware::RateLimitHandler::Config rlConfig;
    rlConfig.globalMaxRequests  = EnvironmentUtils::get_env_u64("RL_GLOBAL_MAX_REQUESTS", rlConfig.globalMaxRequests);
    rlConfig.globalWindow       = std::chrono::seconds(
        EnvironmentUtils::get_env_u64("RL_GLOBAL_WINDOW_SECONDS", rlConfig.globalWindow.count()));
    rlConfig.loginMaxRequests   = EnvironmentUtils::get_env_u64("RL_LOGIN_MAX_REQUESTS", rlConfig.loginMaxRequests);
    rlConfig.loginWindow        = std::chrono::seconds(
        EnvironmentUtils::get_env_u64("RL_LOGIN_WINDOW_SECONDS", rlConfig.loginWindow.count()));
    rlConfig.loginMaxFailures   = EnvironmentUtils::get_env_u64("RL_LOGIN_MAX_FAILURES", rlConfig.loginMaxFailures);
    rlConfig.loginFailureWindow = std::chrono::seconds(
        EnvironmentUtils::get_env_u64("RL_LOGIN_FAILURE_WINDOW_SECONDS", rlConfig.loginFailureWindow.count()));
    rlConfig.loginLockout       = std::chrono::seconds(
        EnvironmentUtils::get_env_u64("RL_LOGIN_LOCKOUT_SECONDS", rlConfig.loginLockout.count()));
    rlConfig.loginMaxBodyBytes  = static_cast<std::size_t>(
        EnvironmentUtils::get_env_u64("RL_LOGIN_MAX_BODY_BYTES", rlConfig.loginMaxBodyBytes));
    rlConfig.globalMaxBodyBytes = static_cast<std::size_t>(
        EnvironmentUtils::get_env_u64("RL_GLOBAL_MAX_BODY_BYTES", rlConfig.globalMaxBodyBytes));
    if (auto v = EnvironmentUtils::EnvironmentUtils::get_env("RL_TRUST_FORWARDED_FOR"))
    {
        rlConfig.trustForwardedFor = (*v != "false" && *v != "0");
    }

    rateLimitMiddleware.configure(rlConfig);

    // Share the login throttler with the AuthController so that login
    // successes/failures feed the same lockout accounting the middleware uses
    // to short-circuit brute-force / credential-stuffing attempts.
    if (m_appContext_.authController)
    {
        m_appContext_.authController->SetLoginThrottler(
            rateLimitMiddleware.GetLoginThrottler(),
            rlConfig.trustForwardedFor);
    }
    
    auto& authMiddleware = m_server_.GetCrowApp().get_middleware<middleware::JwtAuthHandler>();
    authMiddleware.configure(
        middleware::OidcConfiguration::create(c_wellKnownVar_, c_audiencesVar_)
    );
    
    return true;
}

void Application::Run()
{
    IDTX_LOG(IDTX_DEBUG, "Run IDTX Core Server");
    m_server_.Run();
}
