/**
 * @file HealthController.h
 * @brief Trivial HTTP controller exposing a liveness / readiness endpoint.
 *
 * Used by load balancers, container orchestrators and monitoring probes to
 * determine whether the process is up and responsive. The endpoint performs
 * no dependency checks: it must remain cheap and side-effect free so it can
 * be invoked at high frequency without impacting real traffic.
 */
#pragma once

#include <crow/http_response.h>

#include <idtx/utils/Logger.h>

/**
 * @brief HTTP controller for the health-check endpoint.
 *
 * Stateless; the same instance can safely be shared across all requests.
 */
class HealthController
{
    IDTX_LOG_CATEGORY("HealthController");
    
public:
    /**
     * @brief Handle @c GET /api/v1/health.
     *
     * Returns a small JSON payload indicating that the process is alive
     * (e.g. @c {"status":"ok"}) with HTTP 200. The handler intentionally
     * performs no external checks so probe latency stays low and failures
     * of downstream systems do not cascade into the liveness signal.
     *
     * @param req The incoming HTTP request (unused, but kept for signature
     *            compatibility with Crow route handlers).
     * @return 200 OK JSON response.
     */
    crow::response GetHealth(const crow::request &req);
};
