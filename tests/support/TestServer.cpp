#include "TestServer.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cpr/cpr.h>

#include <crow/websocket.h>

#include "app/ApplicationContext.h"
#include "server/Server.h"
#include "session/Session.h"
#include "session/SessionManager.h"

#include "UsdFixture.h"

namespace idtx::tests
{

namespace
{

/// Set an environment variable in a cross-platform way.
void SetEnv(const char* name, const std::string& value)
{
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), /*overwrite=*/1);
#endif
}

/// Unique-ish directory suffix — good enough for tests.
std::string RandomSuffix()
{
    auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<unsigned long long>(ns));
}

// ---------------------------------------------------------------------------
// Teardown helpers — see the block comment in Teardown() for rationale.
// ---------------------------------------------------------------------------

/// Best-effort: close every websocket connection currently attached to any
/// live session.
void CloseAllWebSocketConnections(idtx::session::SessionManager& manager)
{
    const auto sessions = manager.List();
    for (const auto& session : sessions)
    {
        if (!session) continue;
        std::vector<crow::websocket::connection*> conns;
        {
            std::shared_lock lk(session->clients_mutex);
            conns.reserve(session->clients.size());
            for (auto* c : session->clients) conns.push_back(c);
        }
        for (auto* c : conns)
        {
            if (!c) continue;
            try { c->close("server shutting down"); }
            catch (...) { /* best-effort */ }
        }
    }
}

/// Poll the SessionManager until every session reports zero attached
/// clients or the deadline expires.
bool WaitForSessionsToDrain(idtx::session::SessionManager& manager,
                            std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto sessions = manager.List();
        bool any_clients = false;
        for (const auto& s : sessions)
        {
            if (!s) continue;
            std::shared_lock lk(s->clients_mutex);
            if (!s->clients.empty()) { any_clients = true; break; }
        }
        if (!any_clients) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Options factories
// ---------------------------------------------------------------------------

TestServer::Options TestServer::WithThumbnails()
{
    Options o; o.thumbnails_enabled = true; return o;
}

TestServer::Options TestServer::WithoutThumbnails()
{
    Options o; o.thumbnails_enabled = false; return o;
}

// ---------------------------------------------------------------------------
// Construction / teardown
// ---------------------------------------------------------------------------

std::uint16_t TestServer::PickFreePort()
{
#if defined(_WIN32)
    WSADATA wsa;
    static bool wsa_started = false;
    if (!wsa_started) { WSAStartup(MAKEWORD(2, 2), &wsa); wsa_started = true; }
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) throw std::runtime_error("socket() failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    { ::closesocket(s); throw std::runtime_error("bind() failed"); }
    int addrlen = sizeof(addr);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &addrlen) != 0)
    { ::closesocket(s); throw std::runtime_error("getsockname() failed"); }
    std::uint16_t port = ntohs(addr.sin_port);
    ::closesocket(s);
    return port;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) throw std::runtime_error("socket() failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    { ::close(s); throw std::runtime_error("bind() failed"); }
    socklen_t addrlen = sizeof(addr);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &addrlen) != 0)
    { ::close(s); throw std::runtime_error("getsockname() failed"); }
    std::uint16_t port = ntohs(addr.sin_port);
    ::close(s);
    return port;
#endif
}

TestServer::TestServer(Options opts)
    : m_opts_(opts)
{
    m_prev_cwd_ = std::filesystem::current_path();
    m_work_dir_ = std::filesystem::temp_directory_path()
                / ("idtx_test_" + RandomSuffix());
    std::filesystem::create_directories(m_work_dir_);
    std::filesystem::current_path(m_work_dir_);
    std::filesystem::create_directories(m_work_dir_ / "uploads");

    if (m_opts_.write_cube_usda)
    {
        WriteCubeUsda(m_work_dir_ / "uploads" / "cube.usda");
    }

    m_port_ = PickFreePort();
    SetEnv("SERVER_PORT", std::to_string(m_port_));
    SetEnv("IDTX_THUMBNAIL_ENABLED", m_opts_.thumbnails_enabled ? "true" : "false");
    SetEnv("IDTX_THUMBNAIL_SIZE",    std::to_string(m_opts_.thumbnail_size));

    m_ctx_    = std::make_unique<ApplicationContext>(ApplicationContext::create());
    m_server_ = std::make_unique<idtx::core::Server>(*m_ctx_);
    m_server_->RegisterRoutes();

    m_thread_ = std::thread([this]() {
        try
        {
            m_server_->GetCrowApp().port(m_port_).concurrency(2).run();
        }
        catch (...) { /* swallow; destructor will join */ }
    });
}

TestServer::~TestServer()
{
    Teardown();
}

void TestServer::Teardown() noexcept
{
    // The teardown sequence has to satisfy three ordering constraints:
    //
    //   1. Every WS connection must be closed *before* we stop the Crow
    //      app, so the per-connection OnClose handler runs while its
    //      SessionManager (owned by m_ctx_) is still alive and can be
    //      safely called from any Crow worker thread.
    //
    //   2. CrowApp::stop() has to return, and its worker threads have to
    //      join, before we destroy m_ctx_. Otherwise a straggling Crow
    //      worker can dereference a controller that has already been
    //      freed.
    //
    //   3. A short quiescence wait between join() and destroying the
    //      context gives async OnClose callbacks a chance to complete
    //      under the still-alive SessionManager. Without it, we
    //      occasionally saw the SessionManager's session map being torn
    //      down while a Crow worker was still inside
    //      SessionManager::DetachClient.
    try
    {
        if (m_ctx_ && m_ctx_->sessionManager)
        {
            CloseAllWebSocketConnections(*m_ctx_->sessionManager);
        }
    }
    catch (...) {}

    try
    {
        if (m_server_)
        {
            m_server_->GetCrowApp().stop();
        }
    }
    catch (...) {}

    if (m_thread_.joinable())
    {
        try { m_thread_.join(); } catch (...) {}
    }

    // Bounded wait so a hung connection cannot stall the test suite.
    if (m_ctx_ && m_ctx_->sessionManager)
    {
        (void)WaitForSessionsToDrain(*m_ctx_->sessionManager,
                                     std::chrono::milliseconds{500});
    }

    // Destroy the server before the context so route handlers can no
    // longer reach into the context, then release the context (which owns
    // the SessionManager, and therefore all UsdStage handles).
    m_server_.reset();
    m_ctx_.reset();

    std::error_code ec;
    if (!m_prev_cwd_.empty())
    {
        std::filesystem::current_path(m_prev_cwd_, ec);
    }
    if (!m_work_dir_.empty())
    {
        std::filesystem::remove_all(m_work_dir_, ec);
    }
}

// ---------------------------------------------------------------------------
// Readiness / URLs
// ---------------------------------------------------------------------------

void TestServer::WaitReady(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const std::string url = base_http_url() + "/api/v1/health";
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto r = cpr::Get(cpr::Url{url},
                          cpr::Timeout{std::chrono::milliseconds{200}});
        if (r.status_code == 200) return;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    throw std::runtime_error("TestServer failed to become ready at " + url);
}

std::string TestServer::base_http_url() const
{
    return "http://" + m_host_ + ":" + std::to_string(m_port_);
}

std::string TestServer::base_ws_url() const
{
    return "ws://" + m_host_ + ":" + std::to_string(m_port_);
}

} // namespace idtx::tests