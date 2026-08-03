#include "Application.h"

#include "middleware/JwtAuthHandler.h" 

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
