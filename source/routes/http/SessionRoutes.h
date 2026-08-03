/**
 * @file SessionRoutes.h
 * @brief Crow route bindings for collaboration session management.
 */
#pragma once

#include <memory>

#include <crow/common.h>

#include "controller/SessionController.h"

template<typename CrowApp>
class SessionRoutes
{
public:
    SessionRoutes(CrowApp& app, std::shared_ptr<SessionController> sessionController)
        : m_app_(app)
        , m_sessionController_(std::move(sessionController))
    {}

    ~SessionRoutes() = default;

    void RegisterRoutes()
    {
        // List all currently active sessions.
        CROW_ROUTE(m_app_, "/api/v1/sessions")
            .methods(crow::HTTPMethod::Get)
            ([this](const crow::request& req) {
                return m_sessionController_->GetSessionList(req);
            });

        // Create a new session for a given USD file.
        CROW_ROUTE(m_app_, "/api/v1/sessions")
            .methods(crow::HTTPMethod::Post)
            ([this](const crow::request& req) {
                return m_sessionController_->CreateSession(req);
            });

        // Inspect a specific session.
        CROW_ROUTE(m_app_, "/api/v1/sessions/<string>")
            .methods(crow::HTTPMethod::Get)
            ([this](const std::string& session_id) {
                return m_sessionController_->GetSession(session_id);
            });

        // Tear down a specific session.
        CROW_ROUTE(m_app_, "/api/v1/sessions/<string>")
            .methods(crow::HTTPMethod::Delete)
            ([this](const std::string& session_id) {
                return m_sessionController_->DeleteSession(session_id);
            });
    }

private:
    CrowApp& m_app_;
    std::shared_ptr<SessionController> m_sessionController_;
};