/**
 * @file WebSocketController.h
 * @brief Crow websocket adapter for collaborative USD editing.
 *
 * Owns no session state; it delegates everything (existence check, client
 * registration, stage authoring) to SessionManager. Messages are encoded
 * with protobuf (idtxcore::BaseMessage) and exchanged as binary frames.
 */
#pragma once

#include <memory>
#include <string>

#include <crow/http_request.h>
#include <crow/websocket.h>

#include <idtx/utils/Logger.h>

namespace idtx { namespace session { class SessionManager; } }

class WebSocketController
{
    IDTX_LOG_CATEGORY("WebSocketController")

    // Per-connection user data attached during the websocket handshake.
    struct WsUserData
    {
        std::string session_id;
    };

public:
    explicit WebSocketController(std::shared_ptr<idtx::session::SessionManager> manager);
    ~WebSocketController() = default;

    bool OnAccept(const crow::request& req, void** userdata);
    void OnOpen(crow::websocket::connection& conn);
    void OnClose(crow::websocket::connection& conn, const std::string& reason, uint16_t code);
    void OnMessage(crow::websocket::connection& conn, const std::string& data, bool isBinary);

private:
    std::shared_ptr<idtx::session::SessionManager> m_manager_;
};