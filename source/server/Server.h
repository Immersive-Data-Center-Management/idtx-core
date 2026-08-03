/**
 * @file Server.h
 * @brief HTTP/websocket server wrapper around a Crow application.
 *
 * The @c Server encapsulates the Crow @c App instance, its middleware stack
 * (CORS + JWT authentication) and the @c RouteRegistry that maps controller
 * methods onto URL paths. It offers a small, purposeful surface to
 * @c Application:
 *   - @c RegisterRoutes() — wire controllers to URLs (called at startup);
 *   - @c GetCrowApp()     — access the underlying Crow app to install
 *                           additional middleware or configure listeners;
 *   - @c Run()            — start the event loop and block.
 *
 * Configuration is read from the environment (@c SERVER_PORT).
 */
#pragma once

#include <crow.h>
#include <crow/middlewares/cors.h>

#include <idtx/utils/Logger.h>
#include "middleware/JwtAuthHandler.h"
#include "routes/RouteRegistry.h"

namespace idtx
{
namespace core
{
    /**
     * @brief Crow-based HTTP/websocket server for the IDTX-Core process.
     *
     * The class is non-copyable and non-movable: it owns the Crow @c App
     * instance directly and is expected to live for the entire process
     * lifetime alongside the @c Application that constructed it.
     */
    class Server
    {
        IDTX_LOG_CATEGORY("Server");
        
        using CrowApp = crow::App<crow::CORSHandler, middleware::JwtAuthHandler>;
    
    public:
        /**
         * @brief Construct the server bound to a shared application context.
         *
         * The context supplies the controllers and services that the
         * routes will dispatch to. The reference must remain valid for the
         * lifetime of the server.
         *
         * @param applicationContext Composition root providing all
         *                           controllers and shared services.
         */
        Server(ApplicationContext& applicationContext);
        ~Server() = default;
    
        /**
         * @brief Wire every controller onto the Crow app via @c RouteRegistry.
         *
         * Must be called exactly once, before @c Run(). Registers HTTP and
         * websocket routes for health, authentication, file serving,
         * session management and the collaboration websocket.
         */
        void RegisterRoutes();

        /**
         * @brief Access the underlying Crow application.
         *
         * Exposed so callers (typically @c Application::Initialize) can
         * configure middleware such as the JWT authentication handler
         * before the server starts.
         *
         * @return Reference to the Crow app; valid for the lifetime of the
         *         @c Server.
         */
        CrowApp& GetCrowApp() { return m_crowApp_; } 

        /**
         * @brief Start the Crow event loop and block until it exits.
         *
         * Reads the listening port from the @c SERVER_PORT environment
         * variable, falling back to a compile-time default when unset.
         * Returns once the Crow app has stopped serving traffic (typically
         * only on process shutdown).
         */
        void Run();
    
    private:
        const std::string c_serverPortVar_ = "SERVER_PORT";

        CrowApp m_crowApp_;
        ApplicationContext& m_appContext_;
        RouteRegistry<CrowApp> m_routeRegistry_;
    };
}
}
