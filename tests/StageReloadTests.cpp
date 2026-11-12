// tests/StageReloadTests.cpp
//

#include "thirdparty/doctest/doctest.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

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

/// Wait for a LayerReloaded of @p usd_path and fail the test if none arrives.
idtxcore::BaseMessage WaitForLayerReloaded(WsTestClient& ws, const std::string& usd_path)
{
    auto reloaded = idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{5000},
        [&](const idtxcore::BaseMessage& m) {
            return m.message_case() == idtxcore::BaseMessage::kLayerReloaded
                && m.layer_reloaded().usd_path() == usd_path;
        });
    REQUIRE(reloaded.has_value());
    return *reloaded;
}

/// Ask for a snapshot and collect its transform frames up to the
/// SnapshotComplete that answers it.
std::vector<idtxcore::BaseMessage> RequestSnapshot(WsTestClient& ws, const std::string& sid)
{
    constexpr std::uint64_t kRequestId = 42;
    idtxcore::BaseMessage request;
    request.set_session_id(sid);
    request.set_request_id(kRequestId);
    request.mutable_snapshot_request();
    std::string payload;
    REQUIRE(request.SerializeToString(&payload));
    ws.SendBinary(payload);

    std::vector<idtxcore::BaseMessage> frames;
    const auto complete = idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{5000},
        [&](const idtxcore::BaseMessage& m) {
            if (m.message_case() == idtxcore::BaseMessage::kXformBroadcast) frames.push_back(m);
            return m.message_case() == idtxcore::BaseMessage::kSnapshotComplete
                && m.request_id() == kRequestId;
        });
    REQUIRE(complete.has_value());
    return frames;
}

} // namespace

TEST_CASE("upload replacement reloads live sessions and announces the reloaded file")
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

    // The client is told to re-fetch the file; the reload sends no frame of
    // its own for the changed prim.
    bool saw_frame = false;
    const auto reloaded = idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{5000},
        [&](const idtxcore::BaseMessage& m) {
            if (m.message_case() == idtxcore::BaseMessage::kXformBroadcast) saw_frame = true;
            return m.message_case() == idtxcore::BaseMessage::kLayerReloaded;
        });
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->layer_reloaded().usd_path() == "cube.usda");
    CHECK(reloaded->layer_reloaded().usd_uri() == "/api/v1/download/cube.usda");
    CHECK(reloaded->server_seq() > 0);
    CHECK_FALSE(saw_frame);

    // The file the client re-fetches carries the new translate.
    double x = 0, y = 0, z = 0;
    REQUIRE(idtx::tests::ReadTranslate(server.uploads_root() / "cube.usda", "/Root/Cube", x, y, z));
    CHECK(x == doctest::Approx(9.0));
    CHECK(y == doctest::Approx(8.0));
    CHECK(z == doctest::Approx(7.0));

    // The session has no edits of its own, so its snapshot is empty.
    CHECK(RequestSnapshot(ws, sid).empty());
}

TEST_CASE("upload replacement keeps the session's override, which the snapshot restores")
{
    TestServer server;
    server.WaitReady();

    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient ws;
    AttachAndDrainHandshake(ws, server, sid);

    // 1) Client authors a stronger opinion on the session layer by sending a
    //    TransformUpdate - this puts /Root/Cube at (1, 2, 3) with the winning
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
    HttpTestClient http;
    const std::string new_bytes = idtx::tests::BuildCubeUsdaBytes(9.0, 8.0, 7.0);
    auto r = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                  new_bytes, "cube.usda",
                                  /*target_dir=*/"", /*overwrite=*/true);
    REQUIRE(r.status == 201);
    (void)WaitForLayerReloaded(ws, "cube.usda");

    // 3) A client that rebuilt its stage from the new file asks for a
    //    snapshot, which restores the session's (1, 2, 3): it still wins by
    //    composition strength.
    const auto frames = RequestSnapshot(ws, sid);
    REQUIRE(frames.size() == 1);
    const auto& upd = frames.front().xform_broadcast().update();
    CHECK(upd.prim_path() == "/Root/Cube");
    REQUIRE(upd.transform_case() == idtxcore::TransformUpdate::kSeperate);
    const auto& t = upd.seperate().translation();
    CHECK(t.x() == doctest::Approx(1.0));
    CHECK(t.y() == doctest::Approx(2.0));
    CHECK(t.z() == doctest::Approx(3.0));
}
