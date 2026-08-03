// tests/support/WsTestClient.h — synchronous websocket client (boost::beast).
//
// The server ships with a Crow-based WS *server*; there is no client. cpr /
// libcurl are HTTP-only. Rather than hand-roll a full WS client we depend on
// boost-beast which is header-only from the consumer's perspective and links
// against boost-system (already in the tree).

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace idtx::tests
{

class WsTestClient
{
public:
    struct ConnectResult
    {
        bool         handshake_ok = false;
        unsigned int http_status  = 0;  // populated on handshake_ok == false
        std::string  error;
    };

    WsTestClient();
    ~WsTestClient();

    WsTestClient(const WsTestClient&)            = delete;
    WsTestClient& operator=(const WsTestClient&) = delete;

    /// Perform a synchronous WebSocket upgrade.
    /// @param target the path+query, e.g. "/ws?sid=<uuid>"
    ConnectResult Connect(const std::string& host,
                          std::uint16_t port,
                          const std::string& target);

    /// Send a binary frame. Throws on I/O errors.
    void SendBinary(const std::string& payload);

    /// Read a single frame, treated as binary. Returns std::nullopt on
    /// timeout, throws on hard I/O errors.
    std::optional<std::string> ReadBinary(std::chrono::milliseconds timeout);

    /// Close the WS gracefully (best-effort).
    void Close();

    bool is_open() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl_;
};

} // namespace idtx::tests