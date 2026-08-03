#include "WebSocketController.h"

#include <chrono>
#include <utility>

#include <idtx/proto/base.pb.h>
#include <idtx/proto/transform.pb.h>

#include "session/Session.h"
#include "session/SessionManager.h"

WebSocketController::WebSocketController(std::shared_ptr<idtx::session::SessionManager> manager)
    : m_manager_(std::move(manager))
{}

bool WebSocketController::OnAccept(const crow::request& req, void** userdata)
{
    IDTX_LOG(IDTX_INFO, "WebSocket connection accept requested from {}.", req.remote_ip_address);

    char* sid_param = req.url_params.get("sid");
    if (!sid_param)
    {
        IDTX_LOG(IDTX_ERROR, "No session_id provided for ws connection. Reject.");
        return false;
    }

    std::string sid = sid_param;
    if (!m_manager_ || !m_manager_->Exists(sid))
    {
        IDTX_LOG(IDTX_ERROR, "Unknown session id '{}'. Reject ws connection.", sid);
        return false;
    }

    // Enforce single-editor semantics as early as possible: reject the WS
    // upgrade with an HTTP 4xx if this session is in SingleEdit mode and
    // already has an editor attached. Crow interprets a returned `false`
    // as a reject and closes the connection with an HTTP error.
    if (m_manager_->IsSingleEditBusy(sid))
    {
        IDTX_LOG(IDTX_WARN,
                 "Rejecting ws upgrade: session {} is in single_edit mode and busy.",
                 sid);
        return false;
    }

    auto* ws_data = new WsUserData();
    ws_data->session_id = std::move(sid);
    *userdata = ws_data;

    IDTX_LOG(IDTX_DEBUG, "Websocket accepted for session {}.", ws_data->session_id);
    return true;
}

void WebSocketController::OnOpen(crow::websocket::connection& conn)
{
    auto* ws_data = static_cast<WsUserData*>(conn.userdata());
    if (!ws_data || ws_data->session_id.empty())
    {
        conn.close("missing session id");
        IDTX_LOG(IDTX_WARN, "Unable to open Websocket: session id missing.");
        return;
    }

    auto session = m_manager_ ? m_manager_->Get(ws_data->session_id) : nullptr;
    if (!session)
    {
        conn.close("session no longer exists");
        IDTX_LOG(IDTX_WARN, "Session {} no longer exists at OnOpen.", ws_data->session_id);
        return;
    }

    // Attach; honour the returned status. The SingleEditBusy branch is the
    // narrow race in which two upgrades slipped past OnAccept concurrently:
    // reject the second one at OnOpen by closing with a distinctive reason
    // so the client can react.
    auto attach_status = m_manager_->AttachClient(ws_data->session_id, &conn);
    if (attach_status == idtx::session::SessionManager::AttachStatus::SingleEditBusy)
    {
        conn.close("single_edit_busy");
        IDTX_LOG(IDTX_WARN,
                 "Closing ws for session {} at OnOpen: single_edit_busy.",
                 ws_data->session_id);
        return;
    }
    if (attach_status == idtx::session::SessionManager::AttachStatus::UnknownSession)
    {
        conn.close("session no longer exists");
        return;
    }

    // Send a Handshake so the client can fetch the initial state via REST.
    idtxcore::BaseMessage msg;
    msg.set_session_id(session->id);
    auto* hs = msg.mutable_handshake();
    hs->set_session_id(session->id);
    hs->set_usd_path(session->usd_file);
    hs->set_usd_uri(std::string("/api/v1/download/") + session->usd_file);

    std::string payload;
    if (msg.SerializeToString(&payload))
    {
        conn.send_binary(payload);
    }
    else
    {
        IDTX_LOG(IDTX_ERROR, "Failed to serialize Handshake for session {}.", session->id);
    }

    IDTX_LOG(IDTX_INFO, "WebSocket connection opened for session {} from {}.",
             session->id, conn.get_remote_ip());
}

void WebSocketController::OnClose(crow::websocket::connection& conn,
                                  const std::string& reason, uint16_t code)
{
    auto* ws_data = static_cast<WsUserData*>(conn.userdata());
    if (ws_data)
    {
        if (m_manager_ && !ws_data->session_id.empty())
        {
            m_manager_->DetachClient(ws_data->session_id, &conn);
        }
        delete ws_data;
    }
    IDTX_LOG(IDTX_INFO, "WebSocket connection closed: {} (code={}, reason={}).",
             conn.get_remote_ip(), code, reason);
}

void WebSocketController::OnMessage(crow::websocket::connection& conn,
                                    const std::string& data, bool isBinary)
{
    if (!isBinary)
    {
        IDTX_LOG(IDTX_DEBUG, "Ignoring non-binary websocket message (size={}).", data.size());
        return;
    }

    auto* ws_data = static_cast<WsUserData*>(conn.userdata());
    if (!ws_data || ws_data->session_id.empty() || !m_manager_) return;

    idtxcore::BaseMessage msg;
    if (!msg.ParseFromString(data))
    {
        IDTX_LOG(IDTX_WARN, "Failed to parse BaseMessage on session {}.", ws_data->session_id);
        return;
    }

    switch (msg.message_case())
    {
        case idtxcore::BaseMessage::kXformUpdate:
        {
            const auto& upd = msg.xform_update();
            bool ok = m_manager_->ApplyTransformUpdate(ws_data->session_id, upd, &conn);

            // Send an Ack back to the originator. This also doubles as the
            // "you saw your own action" signal in the TfNotice broadcast model,
            // because the listener suppresses the echo to the origin.
            idtxcore::BaseMessage ack_msg;
            ack_msg.set_session_id(ws_data->session_id);
            auto* ack = ack_msg.mutable_ack();
            ack->set_ok(ok);
            if (!ok) ack->set_error("Failed to apply TransformUpdate");

            std::string payload;
            if (ack_msg.SerializeToString(&payload)) conn.send_binary(payload);
            break;
        }
        case idtxcore::BaseMessage::kHandshake:
            // Client-initiated handshake is currently informational only.
            IDTX_LOG(IDTX_DEBUG, "Handshake message received from client on session {}.",
                     ws_data->session_id);
            break;
        case idtxcore::BaseMessage::kXformBroadcast:
        case idtxcore::BaseMessage::kAck:
        case idtxcore::BaseMessage::kError:
            // Server-originated message types; ignore if a client sends them.
            IDTX_LOG(IDTX_DEBUG, "Ignoring server-originated message type from client.");
            break;
        case idtxcore::BaseMessage::MESSAGE_NOT_SET:
        default:
            IDTX_LOG(IDTX_DEBUG, "Empty BaseMessage received on session {}.",
                     ws_data->session_id);
            break;
    }
}