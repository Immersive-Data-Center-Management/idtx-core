/**
 * @file Application.h
 * @brief Top-level application class that wires together the IDTX-Core services.
 *
 * The @c Application owns the shared @c ApplicationContext (controllers,
 * session manager, thumbnail worker, ...) and the Crow @c Server, and
 * exposes the two-step lifecycle expected by @c main():
 *   1. @c Initialize()  — read configuration from the environment, load
 *      OIDC metadata and register HTTP/websocket routes;
 *   2. @c Run()          — start the Crow event loop and block until it exits.
 *
 * The class is intentionally not copyable/movable: it is instantiated once,
 * on the stack in @c main(), for the lifetime of the process.
 */
#pragma once

#include "ApplicationContext.h"
#include "idtx/utils/Logger.h"
#include "server/Server.h"

namespace idtx
{
namespace core
{
    /**
     * @brief Orchestrates initialization and execution of the IDTX-Core server.
     *
     * Holds the composed @c ApplicationContext (services shared across the
     * REST and websocket layers) and the underlying Crow-based @c Server.
     */
    class Application
    {
        IDTX_LOG_CATEGORY("Application");
    
    public:
        /**
         * @brief Construct the application and eagerly build its context.
         *
         * The context factory (@c ApplicationContext::create) instantiates all
         * long-lived services (controllers, session manager, optional
         * thumbnail worker). The @c Server is then constructed with a
         * reference to that context so it can register routes against it.
         */
        Application()
            : m_appContext_(ApplicationContext::create())
            , m_server_(m_appContext_)
        {}
        ~Application() = default;
    
        /**
         * @brief Perform runtime initialization prior to serving traffic.
         *
         * Reads the OIDC well-known URL and audiences from environment
         * variables (@c OIDC_WELLKNOWN_URL, @c OIDC_AUDIENCES), configures
         * the JWT authentication middleware and registers all HTTP and
         * websocket routes on the Crow application.
         *
         * @return @c true on success; @c false if a required piece of
         *         configuration is missing or invalid. In the failure case
         *         the application must not call @c Run().
         */
        bool Initialize();

        /**
         * @brief Start the HTTP/websocket server and block until it shuts down.
         *
         * Delegates to @c Server::Run(). Any exception thrown by the server
         * is propagated to @c main() so the process exits with a non-zero
         * status code.
         */
        void Run();
    
    private:
        const std::string c_wellKnownVar_ = "OIDC_WELLKNOWN_URL";
        const std::string c_audiencesVar_ = "OIDC_AUDIENCES";
        
        ApplicationContext m_appContext_;
        Server  m_server_;
    };
}
}
