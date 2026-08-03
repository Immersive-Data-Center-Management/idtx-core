#include "SessionController.h"

#include <exception>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "dto/ErrorResponse.h"
#include "dto/JsonDto.h"
#include "dto/SessionDto.h"
#include "session/Session.h"
#include "session/SessionManager.h"

using json = nlohmann::json;

SessionController::SessionController(std::shared_ptr<idtx::session::SessionManager> manager)
    : m_manager_(std::move(manager))
{}

crow::response SessionController::GetSessionList(const crow::request& /*req*/)
{
    try
    {
        json arr = json::array();
        for (const auto& session : m_manager_->List())
        {
            arr.push_back(idtx::session::SessionManager::ToJson(*session));
        }
        crow::response res(200, json{{"sessions", arr}}.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "GetSessionList failed: {}", e.what());
        return idtx::dto::make_error(500, "internal_error", e.what());
    }
}

crow::response SessionController::GetSession(const std::string& session_id)
{
    auto session = m_manager_->Get(session_id);
    if (!session)
    {
        return idtx::dto::make_error(404, "not_found", "Session not found.");
    }
    crow::response res(200, idtx::session::SessionManager::ToJson(*session).dump());
    res.set_header("Content-Type", "application/json");
    return res;
}

crow::response SessionController::CreateSession(const crow::request& req)
{
    try
    {
        // Parse the typed request body via the shared JsonDto helper.
        auto parsed = idtx::dto::parse_json_dto<idtx::dto::CreateSessionRequest>(req);
        if (auto* err = std::get_if<crow::response>(&parsed))
        {
            return std::move(*err);
        }
        const auto& body = std::get<idtx::dto::CreateSessionRequest>(parsed);

        if (body.usd_file.empty())
        {
            return idtx::dto::make_error(400, "invalid_request",
                                         "'usd_file' must not be empty.");
        }

        std::string error_msg;
        idtx::session::SessionManager::CreateStatus status =
            idtx::session::SessionManager::CreateStatus::Ok;
        auto session = m_manager_->Create(body.usd_file, body.mode, error_msg, status);

        if (!session)
        {
            switch (status)
            {
                case idtx::session::SessionManager::CreateStatus::FileNotFound:
                    return idtx::dto::make_error(404, "not_found", error_msg);
                case idtx::session::SessionManager::CreateStatus::StageOpenFailed:
                    return idtx::dto::make_error(409, "stage_open_failed", error_msg);
                default:
                    return idtx::dto::make_error(500, "internal_error", error_msg);
            }
        }

        crow::response res(201, idtx::session::SessionManager::ToJson(*session).dump());
        res.set_header("Content-Type", "application/json");
        res.set_header("Location", std::string("/api/v1/sessions/") + session->id);
        return res;
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "CreateSession failed: {}", e.what());
        return idtx::dto::make_error(500, "internal_error", e.what());
    }
}

crow::response SessionController::DeleteSession(const std::string& session_id)
{
    if (!m_manager_->Destroy(session_id))
    {
        return idtx::dto::make_error(404, "not_found", "Session not found.");
    }
    return crow::response(204);
}