/**
 * @file WebSocketController.h
 * @brief Crow websocket adapter for collaborative USD editing.
 *
 * Owns no session state; it delegates everything (existence check, client
 * registration, stage authoring) to SessionManager. Messages are encoded
 * with protobuf (idtxcore::BaseMessage) and exchanged as binary frames.
 *
 * Incoming updates are queued on the session and never processed on the
 * websocket thread. The resulting Ack is sent by the session's consumer; only
 * an update the session could not accept is answered here directly.
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <crow/http_request.h>
#include <crow/websocket.h>

#include <idtx/utils/Logger.h>

#include "session/SessionCommand.h"

namespace idtx { namespace session { class SessionManager; } }

class WebSocketController
{
    IDTX_LOG_CATEGORY("WebSocketController")

    // Per-connection user data attached during the websocket handshake.
    struct WsUserData
    {
        std::string                       session_id;
        idtx::session::ConnectionId       connection_id = 0;
        // Captured at accept time: querying the socket later throws once it
        // has been closed, e.g. inside the close handler.
        std::string                       remote_ip;
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

    // Source of connection ids, unique per controller.
    std::atomic<idtx::session::ConnectionId> m_next_connection_id_{1};
};