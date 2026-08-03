/**
 * @file RouteRegistry.h
 * @brief Aggregates the individual per-feature route registrars.
 *
 * Rather than have @c Server directly know every URL exposed by the
 * application, we compose a small template that owns one @c *Routes helper
 * per feature (health, auth, files, sessions, websocket) and forwards a
 * single @c RegisterAllRoutes() call to each of them. This keeps route
 * wiring co-located with the controllers they target and makes it trivial
 * to add or remove a whole feature by editing one place.
 *
 * The registry is templated on the concrete Crow @c App type so that the
 * middleware list (e.g. @c CORSHandler, @c JwtAuthHandler) can be varied
 * between production and tests without changing this file.
 */
#pragma once
#include <crow.h>

#include "app/ApplicationContext.h"
#include "http/AuthRoutes.h"
#include "http/FileServingRoutes.h"
#include "http/HealthRoutes.h"
#include "http/SessionRoutes.h"
#include "websocket/WebSocketRoutes.h"

/**
 * @brief Composes and registers all feature-specific route registrars.
 *
 * @tparam CrowApp  The concrete Crow @c App instantiation (with its
 *                  middleware list) used by the server.
 */
template<typename CrowApp> 
class RouteRegistry
{
public:
    /**
     * @brief Construct the registry by binding one @c *Routes helper per
     *        feature to the corresponding controller from @p applicationContext.
     *
     * The stored references must remain valid for the lifetime of the
     * registry (they typically live in @c idtx::core::Server).
     *
     * @param crowApp             The Crow application to register routes on.
     * @param applicationContext  The composition root providing controllers.
     */
    RouteRegistry(CrowApp& crowApp, ApplicationContext& applicationContext)
        : m_crowApp_(crowApp)
        , m_applicationContext_(applicationContext)
        , m_healthRoutes_(crowApp, applicationContext.healthController)
        , m_authRoutes_(crowApp, applicationContext.authController)
        , m_fileRoutes_(crowApp, applicationContext.fileServingController)
        , m_sessionRoutes_(crowApp, applicationContext.sessionController)
        , m_webSocketRoutes_(crowApp, applicationContext.webSocketController)
    {}
    
    /**
     * @brief Register every HTTP and websocket route with the Crow app.
     *
     * Called once during server initialisation, before the event loop
     * starts. Delegates to each per-feature route registrar in a fixed,
     * documented order (health → auth → files → sessions → websocket).
     */
    void RegisterAllRoutes()
    {
        m_healthRoutes_.RegisterRoutes();
        m_authRoutes_.RegisterRoutes();
        m_fileRoutes_.RegisterRoutes();
        m_sessionRoutes_.RegisterRoutes();
        m_webSocketRoutes_.RegisterRoutes();
    }
    
private:
    CrowApp& m_crowApp_;
    ApplicationContext& m_applicationContext_;
    
    HealthRoutes<CrowApp> m_healthRoutes_;
    AuthRoutes<CrowApp> m_authRoutes_;
    FileServingRoutes<CrowApp> m_fileRoutes_;
    SessionRoutes<CrowApp> m_sessionRoutes_;
    WebSocketRoutes<CrowApp> m_webSocketRoutes_;
};