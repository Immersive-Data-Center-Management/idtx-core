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
 * an update the session could not accept is answered here directly. For an
 * update rejected because the queue was full, a correction is requested so
 * the client learns the server state of the prim.
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

#include <crow/http_request.h>
#include <crow/websocket.h>

#include <idtx/utils/Logger.h>

#include "session/SessionCommand.h"

namespace idtx { namespace session { class SessionManager; struct Session; } }

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
        // The session this connection is attached to, set once attaching
        // succeeded. Detaching goes through it rather than the id, because
        // a deleted session is no longer found by id while it is still
        // being torn down and sending to its clients.
        std::weak_ptr<idtx::session::Session> session;
        // One correction request per prim this connection had an update of
        // rejected with queue_full, reused for every later rejection of the
        // same prim. Only touched by the connection's message handler, which
        // crow never runs concurrently for one connection.
        std::unordered_map<std::string, std::shared_ptr<idtx::session::PendingCorrection>>
                                          pending_corrections;
    };

public:
    explicit WebSocketController(std::shared_ptr<idtx::session::SessionManager> manager);
    ~WebSocketController() = default;

    bool OnAccept(const crow::request& req, void** userdata);
    void OnOpen(crow::websocket::connection& conn);
    void OnClose(crow::websocket::connection& conn, const std::string& reason, uint16_t code);
    void OnMessage(crow::websocket::connection& conn, const std::string& data, bool isBinary);

private:
    void RequestCorrection(WsUserData& ws_data, const std::string& prim_path);

    std::shared_ptr<idtx::session::SessionManager> m_manager_;

    // Source of connection ids, unique per controller.
    std::atomic<idtx::session::ConnectionId> m_next_connection_id_{1};
};