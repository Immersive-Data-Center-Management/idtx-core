/**
 * @file SessionController.h
 * @brief HTTP REST adapter for collaboration session management.
 *
 * The controller is intentionally stateless: it parses requests, delegates
 * to SessionManager, and turns the result into crow::response objects.
 */
#pragma once

#include <memory>

#include <crow/http_request.h>
#include <crow/http_response.h>

#include <idtx/utils/Logger.h>

namespace idtx::session
{
    class SessionManager;
}

class SessionController
{
    IDTX_LOG_CATEGORY("SessionController")

public:
    explicit SessionController(std::shared_ptr<idtx::session::SessionManager> manager);
    ~SessionController() = default;

    crow::response GetSessionList(const crow::request& req);
    crow::response GetSession(const std::string& session_id);
    crow::response CreateSession(const crow::request& req);
    crow::response DeleteSession(const std::string& session_id);

private:
    std::shared_ptr<idtx::session::SessionManager> m_manager_;
};