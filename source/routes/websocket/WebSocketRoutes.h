/**
 * @file WebSocketRoutes.h
 * @brief
 *
 **/
#pragma once

#include <crow/common.h>
#include <crow/websocket.h>

#include "controller/WebSocketController.h"

template<typename CrowApp>
class WebSocketRoutes
{
public:
    WebSocketRoutes(CrowApp& app, std::shared_ptr<WebSocketController> webSocketController)
        : m_app_(app)
        , m_webSocketController_(std::move(webSocketController))
    {}
    
    ~WebSocketRoutes() = default;
    
    void RegisterRoutes()
    {
        // WebSocket endpoint exchanging protobuf messages with clients.
        // as the macro definition ignores the option to have a templated CrowApp we need to manually
        // expand it to create the proper code. Interistingly the CROW_ROUTE macro is defined properly.
        //CROW_WEBSOCKET_ROUTE(m_app_, "/ws")
        m_app_.template route<crow::black_magic::get_parameter_tag("/ws")>("/ws")
              .template websocket<typename std::remove_reference<decltype(m_app_)>::type>(&m_app_)
            .onaccept([this](const crow::request& req, void** userdata) {
                return m_webSocketController_->OnAccept(req, userdata);
            })
            .onopen([this](crow::websocket::connection& conn) {
                m_webSocketController_->OnOpen(conn);
            })
            .onclose([this](crow::websocket::connection& conn, const std::string& reason, uint16_t code) {
                m_webSocketController_->OnClose(conn, reason, code);
            })
            .onmessage([this](crow::websocket::connection& conn, const std::string& data, bool isBinary) {
                m_webSocketController_->OnMessage(conn, data, isBinary);
            });
    }
    
private:
    CrowApp& m_app_;
    std::shared_ptr<WebSocketController> m_webSocketController_;
};