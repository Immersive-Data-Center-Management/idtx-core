#include "WsTestClient.h"

#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>

namespace idtx::tests
{

namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace net   = boost::asio;
using tcp       = boost::asio::ip::tcp;

struct WsTestClient::Impl
{
    net::io_context ioc;
    std::unique_ptr<beast::websocket::stream<tcp::socket>> ws;
    bool open = false;
};

WsTestClient::WsTestClient()  : m_impl_(std::make_unique<Impl>()) {}
WsTestClient::~WsTestClient() { try { Close(); } catch (...) {} }

bool WsTestClient::is_open() const noexcept
{
    return m_impl_ && m_impl_->open;
}

WsTestClient::ConnectResult WsTestClient::Connect(const std::string& host,
                                                  std::uint16_t port,
                                                  const std::string& target)
{
    ConnectResult r;
    try
    {
        tcp::resolver resolver{m_impl_->ioc};
        auto results = resolver.resolve(host, std::to_string(port));

        m_impl_->ws = std::make_unique<beast::websocket::stream<tcp::socket>>(m_impl_->ioc);

        auto& lowest = beast::get_lowest_layer(*m_impl_->ws);
        net::connect(lowest, results.begin(), results.end());

        // Capture the response so we can inspect the HTTP status on reject.
        beast::websocket::response_type resp;
        std::string host_hdr = host + ":" + std::to_string(port);

        try
        {
            m_impl_->ws->handshake(resp, host_hdr, target);
            r.handshake_ok = true;
            r.http_status  = static_cast<unsigned int>(resp.result_int());
            m_impl_->open  = true;
            // Prefer binary reads by default; frames from the server are all
            // binary (protobuf).
            m_impl_->ws->binary(true);
        }
        catch (const beast::system_error& se)
        {
            r.handshake_ok = false;
            r.http_status  = static_cast<unsigned int>(resp.result_int());
            r.error        = se.what();
            m_impl_->open  = false;
        }
    }
    catch (const std::exception& e)
    {
        r.handshake_ok = false;
        r.error        = e.what();
        m_impl_->open  = false;
    }
    return r;
}

void WsTestClient::SendBinary(const std::string& payload)
{
    if (!is_open()) throw std::runtime_error("WsTestClient: not connected");
    m_impl_->ws->binary(true);
    m_impl_->ws->write(net::buffer(payload));
}

std::optional<std::string> WsTestClient::ReadBinary(std::chrono::milliseconds timeout)
{
    if (!is_open()) throw std::runtime_error("WsTestClient: not connected");

    // Use an async read with a deadline timer to implement a timed blocking read.
    beast::flat_buffer buffer;
    bool               done   = false;
    bool               timed  = false;
    std::string        result;
    std::exception_ptr err;

    net::steady_timer timer(m_impl_->ioc);
    timer.expires_after(timeout);
    timer.async_wait([&](const boost::system::error_code& ec) {
        if (ec) return;               // cancelled
        if (done) return;
        timed = true;
        // Cancel any outstanding async ops on the socket so async_read returns.
        boost::system::error_code ig;
        beast::get_lowest_layer(*m_impl_->ws).cancel(ig);
    });

    m_impl_->ws->async_read(buffer,
        [&](const beast::error_code& ec, std::size_t /*n*/) {
            done = true;
            timer.cancel();
            if (ec)
            {
                // A cancellation triggered by the deadline timer above shows up
                // here as operation_aborted; treat it as "timed out" rather
                // than a hard failure.
                if (!timed
                    && ec != net::error::operation_aborted
                    && ec != beast::websocket::error::closed)
                {
                    err = std::make_exception_ptr(std::runtime_error(ec.message()));
                }
                return;
            }
            result = beast::buffers_to_string(buffer.data());
        });

    // Drive the io_context until either read or timer completes.
    m_impl_->ioc.restart();
    m_impl_->ioc.run();

    if (timed && !done) return std::nullopt; // safety
    if (err) std::rethrow_exception(err);
    if (timed) return std::nullopt;
    return result;
}

void WsTestClient::Close()
{
    if (!m_impl_ || !m_impl_->ws) return;
    boost::system::error_code ec;
    if (m_impl_->open)
    {
        m_impl_->ws->close(beast::websocket::close_code::normal, ec);
        m_impl_->open = false;
    }
    beast::get_lowest_layer(*m_impl_->ws).close(ec);
    m_impl_->ws.reset();
}

} // namespace idtx::tests