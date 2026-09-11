// tests/SessionReaperTests.cpp
//
// Exercises the idle-session reaper: a session with zero connected clients is
// destroyed after IDTX_SESSION_IDLE_TIMEOUT_SECONDS, and only then. These are
// black-box tests driven entirely over REST + WebSocket, using a short (1s)
// timeout so they stay fast.

#include "thirdparty/doctest/doctest.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "support/HttpTestClient.h"
#include "support/TestServer.h"
#include "support/WsTestClient.h"

using idtx::tests::HttpTestClient;
using idtx::tests::TestServer;
using idtx::tests::WsTestClient;
using json = nlohmann::json;

namespace
{

TestServer::Options WithIdleTimeout(std::uint32_t seconds)
{
    auto o = TestServer::WithoutThumbnails();   // thumbnails irrelevant here
    o.idle_timeout_seconds = seconds;
    return o;
}

std::string CreateSession(const TestServer& server)
{
    HttpTestClient http;
    json req = {{"usd_file", "cube.usda"}, {"mode", "collaborative_edit"}};
    auto r = http.PostJson(server.base_http_url() + "/api/v1/sessions", req.dump());
    REQUIRE(r.status == 201);
    return json::parse(r.text).value("session_id", "");
}

// Poll GET /api/v1/sessions/<sid> until it returns 404 (session gone) or the
// deadline elapses. Returns true if the session disappeared in time.
bool WaitForSessionGone(const TestServer& server, const std::string& sid,
                        std::chrono::milliseconds timeout)
{
    HttpTestClient http;
    const auto url = server.base_http_url() + "/api/v1/sessions/" + sid;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto r = http.Get(url);
        if (r.status == 404) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}

} // namespace

TEST_CASE("idle session is reaped after the last client disconnects")
{
    TestServer server(WithIdleTimeout(1));
    server.WaitReady();

    const std::string sid = CreateSession(server);

    // Connect a client so the session is definitely "in use", then drop it.
    {
        WsTestClient ws;
        auto conn = ws.Connect("127.0.0.1", server.port(), "/ws?sid=" + sid);
        REQUIRE(conn.handshake_ok);
        REQUIRE(ws.is_open());

        // While connected, the session must NOT be reaped even past the
        // timeout — the reaper only counts empty sessions.
        std::this_thread::sleep_for(std::chrono::milliseconds{1500});
        HttpTestClient http;
        auto alive = http.Get(server.base_http_url() + "/api/v1/sessions/" + sid);
        CHECK(alive.status == 200);

        ws.Close();
    }

    // Now that the client is gone, the reaper should destroy the session
    // within ~1s (timeout) + one sweep interval.
    CHECK(WaitForSessionGone(server, sid, std::chrono::milliseconds{5000}));
}

TEST_CASE("session created but never connected is reaped")
{
    TestServer server(WithIdleTimeout(1));
    server.WaitReady();

    const std::string sid = CreateSession(server);

    // No client ever attaches; empty_since is set at creation, so it should be
    // reaped once the timeout elapses.
    CHECK(WaitForSessionGone(server, sid, std::chrono::milliseconds{5000}));
}

TEST_CASE("reaper disabled keeps an idle session alive")
{
    TestServer server(WithIdleTimeout(0));   // reaper off (default behavior)
    server.WaitReady();

    const std::string sid = CreateSession(server);

    // Connect and immediately disconnect, then wait well past what a 1s reaper
    // would need. With the reaper disabled the session must persist.
    {
        WsTestClient ws;
        auto conn = ws.Connect("127.0.0.1", server.port(), "/ws?sid=" + sid);
        REQUIRE(conn.handshake_ok);
        ws.Close();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{1500});

    HttpTestClient http;
    auto r = http.Get(server.base_http_url() + "/api/v1/sessions/" + sid);
    CHECK(r.status == 200);
}
