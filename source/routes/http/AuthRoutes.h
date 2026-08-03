/**
 * @file AuthRoutes.h
 * @brief Crow route bindings for the authentication endpoint.
 */
#pragma once

#include <memory>

#include <crow/common.h>

#include "controller/AuthController.h"

template<typename CrowApp>
class AuthRoutes
{
public:
    AuthRoutes(CrowApp& app, std::shared_ptr<AuthController> authController)
        : m_app_(app)
        , m_authController_(std::move(authController))
    {}

    ~AuthRoutes() = default;

    void RegisterRoutes()
    {
        // Issue an access token using the OAuth2 Resource Owner Password
        // Credentials flow against the configured IdP.
        CROW_ROUTE(m_app_, "/api/v1/auth/login")
            .methods(crow::HTTPMethod::Post)
            ([this](const crow::request& req) {
                return m_authController_->Login(req);
            });
    }

private:
    CrowApp& m_app_;
    std::shared_ptr<AuthController> m_authController_;
};