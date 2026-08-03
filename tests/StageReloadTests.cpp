// tests/StageReloadTests.cpp — covers the reload-on-upload feature described
// in docs/plans/reload-on-upload.md.
//
// Two scenarios:
//   A. When an upload replaces the backing USD file of a live session, every
//      attached client receives a TransformBroadcast reflecting the new
//      on-disk contents.
//   B. When a client has already authored a stronger opinion on the session
//      layer (via a TransformUpdate), the on-disk change is masked by
//      composition and no additional TransformBroadcast reaches the client.

#include "thirdparty/doctest/doctest.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "support/HttpTestClient.h"
#include "support/ProtoHelpers.h"
#include "support/TestServer.h"
#include "support/UsdFixture.h"
#include "support/WsTestClient.h"

using idtx::tests::HttpTestClient;
using idtx::tests::TestServer;
using idtx::tests::WsTestClient;
using json = nlohmann::json;

namespace
{

/// Create a session and return its id. Fails the test on non-201.
std::string CreateSession(const TestServer& server, const std::string& mode)
{
    HttpTestClient http;
    json req = {{"usd_file", "cube.usda"}, {"mode", mode}};
    auto r = http.PostJson(server.base_http_url() + "/api/v1/sessions", req.dump());
    REQUIRE(r.status == 201);
    return json::parse(r.text).value("session_id", "");
}

/// Convenience: attach a WS client and drain the initial Handshake frame.
void AttachAndDrainHandshake(WsTestClient& ws,
                             const TestServer& server,
                             const std::string& sid)
{
    auto conn = ws.Connect("127.0.0.1", server.port(), "/ws?sid=" + sid);
    REQUIRE(conn.handshake_ok);
    auto hs = idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{2000},
        [](const idtxcore::BaseMessage& m) {
            return m.message_case() == idtxcore::BaseMessage::kHandshake;
        });
    REQUIRE(hs.has_value());
}

} // namespace

TEST_CASE("upload replacement reloads live sessions and broadcasts new transform")
{
    TestServer server;
    server.WaitReady();

    // Collaborative session so the second-editor rejection path does not
    // interfere; either mode would work for a single attached client.
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient ws;
    AttachAndDrainHandshake(ws, server, sid);

    // Replace cube.usda on disk via the upload endpoint. The new content
    // authors translate (9, 8, 7) on /Root/Cube.
    HttpTestClient http;
    const std::string new_bytes = idtx::tests::BuildCubeUsdaBytes(9.0, 8.0, 7.0);
    auto r = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                  new_bytes, "cube.usda",
                                  /*target_dir=*/"", /*overwrite=*/true);
    REQUIRE(r.status == 201);

    // Expect a single TransformBroadcast for /Root/Cube with the new translate.
    auto bcast = idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{2000},
        [](const idtxcore::BaseMessage& m) {
            return m.message_case() == idtxcore::BaseMessage::kXformBroadcast;
        });
    REQUIRE(bcast.has_value());
    const auto& upd = bcast->xform_broadcast().update();
    CHECK(upd.prim_path() == "/Root/Cube");

    // The server-authored broadcast uses the "separate" transform form.
    REQUIRE(upd.transform_case() == idtxcore::TransformUpdate::kSeperate);
    const auto& t = upd.seperate().translation();
    CHECK(t.x() == doctest::Approx(9.0));
    CHECK(t.y() == doctest::Approx(8.0));
    CHECK(t.z() == doctest::Approx(7.0));
}

TEST_CASE("upload replacement is suppressed when session layer overrides the change")
{
    TestServer server;
    server.WaitReady();

    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient ws;
    AttachAndDrainHandshake(ws, server, sid);

    // 1) Client authors a stronger opinion on the session layer by sending a
    //    TransformUpdate — this puts /Root/Cube at (1, 2, 3) with the winning
    //    composition strength.
    const std::string payload = idtx::tests::BuildTransformUpdate(
        sid, "cube.usda", "/Root/Cube", 1.0, 2.0, 3.0);
    ws.SendBinary(payload);

    // Drain the Ack. Since there is only one WS client, no self-broadcast is
    // emitted for the update itself.
    auto ack = idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{2000},
        [](const idtxcore::BaseMessage& m) {
            return m.message_case() == idtxcore::BaseMessage::kAck;
        });
    REQUIRE(ack.has_value());
    CHECK(ack->ack().ok());

    // 2) Replace cube.usda on disk with a *different* translate (9, 8, 7).
    //    The session layer's (1, 2, 3) opinion for xformOp:translate still
    //    wins by composition strength, so the listener must suppress the
    //    reload broadcast for /Root/Cube.
    HttpTestClient http;
    const std::string new_bytes = idtx::tests::BuildCubeUsdaBytes(9.0, 8.0, 7.0);
    auto r = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                  new_bytes, "cube.usda",
                                  /*target_dir=*/"", /*overwrite=*/true);
    REQUIRE(r.status == 201);

    // 3) Wait a short interval and verify no additional TransformBroadcast
    //    arrives. WaitForMessage returns nullopt on timeout, which is the
    //    expected outcome here.
    auto extra = idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{500},
        [](const idtxcore::BaseMessage& m) {
            return m.message_case() == idtxcore::BaseMessage::kXformBroadcast;
        });
    CHECK_FALSE(extra.has_value());
}