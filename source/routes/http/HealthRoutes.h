/**
 * @file HealthRoutes.h
 * @brief Crow route bindings for the health-check endpoint.
 *
 * Exposes @c GET /api/v1/health, delegating to @c HealthController::GetHealth.
 * The health path is typically added to the JWT middleware's public
 * allow-list so probes can succeed without a bearer token.
 */
#pragma once
#include "controller/HealthController.h"

/**
 * @brief Registers the health-check HTTP route on a Crow application.
 *
 * @tparam CrowApp  The concrete Crow @c App instantiation.
 */
template<typename CrowApp>
class HealthRoutes
{
public:
    /**
     * @brief Construct the route helper bound to a specific controller.
     *
     * @param app                The Crow application to register routes on.
     *                           Must outlive this helper.
     * @param healthController   Controller instance whose @c GetHealth
     *                           method backs the endpoint. Must not be null.
     */
    HealthRoutes(CrowApp& app, std::shared_ptr<HealthController> healthController)
        : m_app_(app)
        , m_healthController_(healthController)
    {}
    ~HealthRoutes() = default;
    
    /**
     * @brief Register @c GET /api/v1/health on the Crow application.
     *
     * Called by @c RouteRegistry::RegisterAllRoutes during server startup.
     */
    void RegisterRoutes()
    {
        CROW_ROUTE(m_app_, "/api/v1/health")
            .methods(crow::HTTPMethod::Get)
            ([this](const crow::request& req)
            {
                return m_healthController_->GetHealth(req);
            });
    }
private:
    CrowApp& m_app_;
    std::shared_ptr<HealthController> m_healthController_;
};
