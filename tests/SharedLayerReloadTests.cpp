// tests/SharedLayerReloadTests.cpp
//
// End-to-end checks of reloads of layers several sessions share: a session
// on the same file and a session that reaches the file through a payload are
// reloaded while their clients stream updates, a teardown's auto-commit
// reloads the file for the other sessions, a replacing upload waits for the
// file-write slot, and a session's used layers follow a soft delete. A
// listener that runs for a session that was not paused logs "does not own
// its stage"; those tests assert it never happens.

#include "thirdparty/doctest/doctest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>

#include <idtx/utils/Logger.h>

#include "concurrency/SharedLayerLock.h"
#include "session/Session.h"
#include "session/SessionManager.h"
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

namespace fs = std::filesystem;

// The listener's message for a notice from a thread that does not own the
// session's stage (see StageNoticeListener::OnObjectsChanged).
constexpr const char* kNotOwnedMessage = "does not own its stage";

/// Counts log lines containing a needle and forwards every line to the
/// logger it replaces. Construct it before the TestServer and destroy it
/// after, so no server thread sees the logger pointer change.
class CaptureLogger final : public idtx::utils::ILogger
{
public:
    explicit CaptureLogger(std::string needle)
        : m_needle_(std::move(needle))
        , m_previous_(idtx::utils::Log::get_logger())
    {
        idtx::utils::Log::set_logger(this);
    }

    ~CaptureLogger() override
    {
        idtx::utils::Log::set_logger(m_previous_);
    }

    CaptureLogger(const CaptureLogger&)            = delete;
    CaptureLogger& operator=(const CaptureLogger&) = delete;

    void log(std::string_view formatted_line, idtx::utils::LogLevel level) override
    {
        if (formatted_line.find(m_needle_) != std::string_view::npos) ++m_matches_;
        if (m_previous_) m_previous_->log(formatted_line, level);
    }

    [[nodiscard]] int Matches() const noexcept { return m_matches_.load(); }

private:
    std::string           m_needle_;
    idtx::utils::ILogger* m_previous_;
    std::atomic<int>      m_matches_{0};
};

void WriteFile(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out << text;
}

/// asset.usda: /Box, which the scene pulls in through a payload, and /Other,
/// which only clients of the asset's own sessions edit.
/// scene.usda: /Root/Group/Asset carries the payload; /Root and /Root/Group
/// are local, so /Root/Group can be soft-deleted.
void WriteSceneFiles(const TestServer& server)
{
    WriteFile(server.uploads_root() / "asset.usda",
              "#usda 1.0\n(\n    defaultPrim = \"Box\"\n)\n\n"
              "def Xform \"Box\"\n{\n"
              "    double3 xformOp:translate = (0, 0, 0)\n"
              "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
              "}\n\n"
              "def Xform \"Other\"\n{\n"
              "    double3 xformOp:translate = (0, 0, 0)\n"
              "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
              "}\n");
    WriteFile(server.uploads_root() / "scene.usda",
              "#usda 1.0\n(\n    defaultPrim = \"Root\"\n)\n\n"
              "def Xform \"Root\"\n{\n"
              "    double3 xformOp:translate = (0, 0, 0)\n"
              "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n\n"
              "    def Xform \"Group\"\n    {\n"
              "        def Xform \"Asset\" (\n"
              "            prepend payload = @./asset.usda@\n"
              "        )\n        {\n        }\n"
              "    }\n"
              "}\n");
}

std::string CreateSession(const TestServer& server,
                          const std::string& usd_file,
                          const std::string& mode,
                          bool auto_commit = false)
{
    HttpTestClient http;
    json req = {{"usd_file", usd_file}, {"mode", mode}, {"auto_commit", auto_commit}};
    auto r = http.PostJson(server.base_http_url() + "/api/v1/sessions", req.dump());
    REQUIRE(r.status == 201);
    return json::parse(r.text).value("session_id", "");
}

bool IsAck(const idtxcore::BaseMessage& m)
{
    return m.message_case() == idtxcore::BaseMessage::kAck;
}

bool IsBroadcastOf(const idtxcore::BaseMessage& m, const std::string& prim_path, double x)
{
    if (m.message_case() != idtxcore::BaseMessage::kXformBroadcast) return false;
    const auto& upd = m.xform_broadcast().update();
    if (upd.prim_path() != prim_path) return false;
    const double got = upd.has_matrix() ? upd.matrix().m30()
                                        : upd.seperate().translation().x();
    return got == doctest::Approx(x);
}

/// Connect and read until SnapshotComplete. Returns its server_seq.
std::uint64_t Join(WsTestClient& ws, const TestServer& server, const std::string& sid)
{
    auto conn = ws.Connect("127.0.0.1", server.port(), "/ws?sid=" + sid);
    REQUIRE(conn.handshake_ok);
    auto done = idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{5000},
        [](const idtxcore::BaseMessage& m) {
            return m.message_case() == idtxcore::BaseMessage::kSnapshotComplete;
        });
    REQUIRE(done.has_value());
    return done->server_seq();
}

/// Read until the Ack for @p request_id. Tracks the highest server_seq seen.
std::optional<idtxcore::BaseMessage> WaitForAck(WsTestClient& ws,
                                                std::uint64_t request_id,
                                                std::uint64_t& max_seq)
{
    return idtx::tests::WaitForMessage(
        ws, std::chrono::milliseconds{10000},
        [&](const idtxcore::BaseMessage& m) {
            max_seq = std::max(max_seq, m.server_seq());
            return IsAck(m) && m.request_id() == request_id;
        });
}

/// What a streaming client received. Filled on its own thread; checked on
/// the test thread (doctest assertions are not thread-safe).
struct StreamResult
{
    std::size_t                        acks = 0;
    std::vector<idtxcore::BaseMessage> broadcasts;
    bool                               connection_lost = false;
};

/// Send @p count updates of @p prim_path, then read until every one is
/// acked (ok or stale), keeping the broadcasts that arrive meanwhile.
StreamResult StreamUpdates(WsTestClient& ws,
                           const std::string& sid,
                           const std::string& usd_file,
                           const std::string& prim_path,
                           std::size_t count,
                           std::uint64_t first_request_id)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        ws.SendBinary(idtx::tests::BuildTransformUpdate(
            sid, usd_file, prim_path, static_cast<double>(i), 0.0, 0.0,
            first_request_id + i, 0));
        if (i % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    StreamResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (result.acks < count && std::chrono::steady_clock::now() < deadline)
    {
        // A timed-out read breaks the connection, so stop after one.
        auto frame = ws.ReadBinary(std::chrono::milliseconds{5000});
        if (!frame)
        {
            result.connection_lost = true;
            break;
        }
        idtxcore::BaseMessage msg;
        if (!idtx::tests::ParseBaseMessage(*frame, msg)) continue;
        if (IsAck(msg) && msg.request_id() >= first_request_id
            && msg.request_id() < first_request_id + count)
        {
            ++result.acks;
        }
        else if (msg.message_case() == idtxcore::BaseMessage::kXformBroadcast)
        {
            result.broadcasts.push_back(msg);
        }
    }
    return result;
}

/// True if @p received, or the frames that follow on @p ws, contain a
/// broadcast of @p prim_path with translate x == @p x.
bool ReceivesBroadcast(WsTestClient& ws,
                       const std::vector<idtxcore::BaseMessage>& received,
                       const std::string& prim_path,
                       double x)
{
    for (const auto& m : received)
    {
        if (IsBroadcastOf(m, prim_path, x)) return true;
    }
    return idtx::tests::WaitForMessage(
               ws, std::chrono::milliseconds{5000},
               [&](const idtxcore::BaseMessage& m) { return IsBroadcastOf(m, prim_path, x); })
        .has_value();
}

/// Whether @p session's cached used layers contain the file at @p path.
/// Read under SharedLayers(), which guards the list.
bool UsesFile(const idtx::session::Session& session, const fs::path& path)
{
    std::shared_lock layers(idtx::concurrency::SharedLayers());
    for (const auto& layer : session.used_layers)
    {
        if (!layer) continue;
        std::error_code ec;
        if (fs::equivalent(fs::path(layer->GetRealPath()), path, ec)) return true;
    }
    return false;
}

/// The translate default of @p prim_path in the in-memory layer of
/// @p usd_file, as every session using it sees it.
std::optional<pxr::GfVec3d> InMemoryTranslate(const fs::path& usd_file,
                                              const std::string& prim_path)
{
    std::shared_lock layers(idtx::concurrency::SharedLayers());
    const auto layer = pxr::SdfLayer::Find(usd_file.string());
    if (!layer) return std::nullopt;
    const auto attr =
        layer->GetAttributeAtPath(pxr::SdfPath(prim_path + ".xformOp:translate"));
    if (!attr) return std::nullopt;
    const pxr::VtValue value = attr->GetDefaultValue();
    if (!value.IsHolding<pxr::GfVec3d>()) return std::nullopt;
    return value.UncheckedGet<pxr::GfVec3d>();
}

} // namespace

TEST_CASE("shared layer: a session lists the files it reaches through a payload, "
          "also after activating and deactivating the prim that carries it")
{
    TestServer server;
    server.WaitReady();
    WriteSceneFiles(server);
    const fs::path asset = server.uploads_root() / "asset.usda";

    // The scene with /Root/Group shipped inactive: the payload below it is not
    // composed, so asset.usda is not loaded for this stage at all.
    WriteFile(server.uploads_root() / "scene_inactive.usda",
              "#usda 1.0\n(\n    defaultPrim = \"Root\"\n)\n\n"
              "def Xform \"Root\"\n{\n"
              "    def Xform \"Group\" (\n        active = false\n    )\n    {\n"
              "        def Xform \"Asset\" (\n"
              "            prepend payload = @./asset.usda@\n"
              "        )\n        {\n        }\n"
              "    }\n"
              "}\n");

    const std::string sid = CreateSession(server, "scene_inactive.usda", "single_edit");
    const auto session = server.session_manager().Get(sid);
    REQUIRE(session);
    CHECK_FALSE(UsesFile(*session, asset));

    WsTestClient ws;
    Join(ws, server, sid);
    std::uint64_t max_seq = 0;

    // Activating /Root/Group composes the payload, opening asset.usda through
    // the shared registry: from now on a reload of it must pause this
    // session, so the command has to have refreshed the list.
    ws.SendBinary(idtx::tests::BuildSetPrimActiveRequest(sid, "/Root/Group", true, 1));
    auto ack = WaitForAck(ws, 1, max_seq);
    REQUIRE(ack.has_value());
    REQUIRE(ack->ack().ok());
    CHECK(UsesFile(*session, asset));

    // Deactivating prunes the payload again. Dropping the layer from the
    // list is expected but not required for correctness: listing a layer the
    // stage no longer uses only pauses this session once too often.
    ws.SendBinary(idtx::tests::BuildSetPrimActiveRequest(sid, "/Root/Group", false, 2));
    ack = WaitForAck(ws, 2, max_seq);
    REQUIRE(ack.has_value());
    REQUIRE(ack->ack().ok());
    WARN_FALSE(UsesFile(*session, asset));
}

TEST_CASE("shared layer: commits reload sessions on the same file and through a payload "
          "while their clients stream updates")
{
    CaptureLogger capture(kNotOwnedMessage);
    {
        TestServer server;
        server.WaitReady();
        WriteSceneFiles(server);

        const std::string editor_sid = CreateSession(server, "asset.usda", "collaborative_edit");
        const std::string peer_sid   = CreateSession(server, "asset.usda", "collaborative_edit");
        const std::string scene_sid  = CreateSession(server, "scene.usda", "collaborative_edit");

        WsTestClient editor;
        WsTestClient peer;
        WsTestClient scene;
        std::uint64_t editor_seq = Join(editor, server, editor_sid);
        Join(peer, server, peer_sid);
        Join(scene, server, scene_sid);

        // The peer and the scene client stream updates of prims nobody else
        // touches, on their own threads, while the editor commits /Box over
        // and over. Each commit reloads asset.usda, which recomposes all
        // three sessions' stages on the layer thread.
        constexpr std::size_t kUpdates = 300;
        auto peer_stream = std::async(std::launch::async, [&] {
            return StreamUpdates(peer, peer_sid, "asset.usda", "/Other", kUpdates, 1000);
        });
        auto scene_stream = std::async(std::launch::async, [&] {
            return StreamUpdates(scene, scene_sid, "scene.usda", "/Root", kUpdates, 1000);
        });

        HttpTestClient http(std::chrono::milliseconds{60000});
        constexpr int kCommits = 5;
        double final_x = 0.0;
        for (int i = 0; i < kCommits; ++i)
        {
            final_x = 10.0 + i;
            const std::uint64_t request_id = 1 + static_cast<std::uint64_t>(i);
            editor.SendBinary(idtx::tests::BuildTransformUpdate(
                editor_sid, "asset.usda", "/Box", final_x, 0.0, 0.0, request_id, editor_seq));
            const auto ack = WaitForAck(editor, request_id, editor_seq);
            REQUIRE(ack.has_value());
            REQUIRE(ack->ack().ok());

            const auto commit = http.PostJson(
                server.base_http_url() + "/api/v1/sessions/" + editor_sid + "/commit", "{}");
            REQUIRE(commit.status == 200);
        }

        const StreamResult peer_result  = peer_stream.get();
        const StreamResult scene_result = scene_stream.get();
        REQUIRE_FALSE(peer_result.connection_lost);
        REQUIRE_FALSE(scene_result.connection_lost);
        CHECK(peer_result.acks == kUpdates);
        CHECK(scene_result.acks == kUpdates);

        // Both other sessions end on the last committed value: the peer on
        // the same file, the scene through the payload.
        CHECK(ReceivesBroadcast(peer, peer_result.broadcasts, "/Box", final_x));
        CHECK(ReceivesBroadcast(scene, scene_result.broadcasts, "/Root/Group/Asset", final_x));

        const auto in_memory = InMemoryTranslate(server.uploads_root() / "asset.usda", "/Box");
        REQUIRE(in_memory.has_value());
        CHECK(*in_memory == pxr::GfVec3d(final_x, 0.0, 0.0));
    }
    CHECK(capture.Matches() == 0);
}

TEST_CASE("shared layer: an upload replacing a file reloads it while a session streams updates")
{
    CaptureLogger capture(kNotOwnedMessage);
    {
        TestServer server;
        server.WaitReady();
        WriteSceneFiles(server);

        const std::string scene_sid = CreateSession(server, "scene.usda", "collaborative_edit");
        WsTestClient scene;
        Join(scene, server, scene_sid);

        auto scene_stream = std::async(std::launch::async, [&] {
            return StreamUpdates(scene, scene_sid, "scene.usda", "/Root", 300, 1000);
        });

        HttpTestClient http(std::chrono::milliseconds{60000});
        double final_x = 0.0;
        for (int i = 0; i < 5; ++i)
        {
            final_x = 20.0 + i;
            const std::string bytes =
                "#usda 1.0\n(\n    defaultPrim = \"Box\"\n)\n\n"
                "def Xform \"Box\"\n{\n"
                "    double3 xformOp:translate = (" + std::to_string(final_x) + ", 0, 0)\n"
                "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                "}\n\n"
                "def Xform \"Other\"\n{\n"
                "    double3 xformOp:translate = (0, 0, 0)\n"
                "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                "}\n";
            const auto r = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                                bytes, "asset.usda",
                                                /*target_dir=*/"", /*overwrite=*/true);
            REQUIRE(r.status == 201);
        }

        const StreamResult result = scene_stream.get();
        REQUIRE_FALSE(result.connection_lost);
        CHECK(result.acks == 300);
        CHECK(ReceivesBroadcast(scene, result.broadcasts, "/Root/Group/Asset", final_x));
    }
    CHECK(capture.Matches() == 0);
}

TEST_CASE("shared layer: deleting an auto-commit session reloads the file for the other sessions")
{
    TestServer server;
    server.WaitReady();

    const std::string committing = CreateSession(server, "cube.usda", "collaborative_edit",
                                                 /*auto_commit=*/true);
    const std::string watching   = CreateSession(server, "cube.usda", "collaborative_edit");

    WsTestClient editor;
    WsTestClient watcher;
    std::uint64_t editor_seq = Join(editor, server, committing);
    Join(watcher, server, watching);

    editor.SendBinary(idtx::tests::BuildTransformUpdate(
        committing, "cube.usda", "/Root/Cube", 7.0, 0.0, 0.0, 1, editor_seq));
    const auto ack = WaitForAck(editor, 1, editor_seq);
    REQUIRE(ack.has_value());
    REQUIRE(ack->ack().ok());

    // The other session does not see the edit: it lives in the committing
    // session's own layer.
    HttpTestClient http(std::chrono::milliseconds{60000});
    REQUIRE(http.Delete(server.base_http_url() + "/api/v1/sessions/" + committing).status == 204);

    // The teardown wrote the file and reloaded its layer, so the remaining
    // session shows the committed value instead of keeping the old baseline.
    CHECK(ReceivesBroadcast(watcher, {}, "/Root/Cube", 7.0));

    double x = 0, y = 0, z = 0;
    REQUIRE(idtx::tests::ReadTranslate(server.uploads_root() / "cube.usda", "/Root/Cube", x, y, z));
    CHECK(x == doctest::Approx(7.0));
}

TEST_CASE("shared layer: a replacing upload waits for the file-write slot a commit holds")
{
    TestServer server;
    server.WaitReady();
    const fs::path cube = server.uploads_root() / "cube.usda";

    // Stand in for a commit that is still writing or reloading.
    auto slot = server.session_manager().AcquireFileWriteSlot(std::chrono::seconds{1});
    REQUIRE(slot);

    auto replacing = std::async(std::launch::async, [&] {
        HttpTestClient http(std::chrono::milliseconds{60000});
        return http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                    idtx::tests::BuildCubeUsdaBytes(3.0, 2.0, 1.0),
                                    "cube.usda", /*target_dir=*/"", /*overwrite=*/true)
            .status;
    });
    CHECK(replacing.wait_for(std::chrono::milliseconds{500}) == std::future_status::timeout);

    double x = 0, y = 0, z = 0;
    REQUIRE(idtx::tests::ReadTranslate(cube, "/Root/Cube", x, y, z));
    CHECK(x == doctest::Approx(0.0)); // not written while the slot is held

    // A new file cannot collide with a commit and does not wait.
    {
        HttpTestClient http;
        const auto fresh = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                                idtx::tests::BuildCubeUsdaBytes(1.0, 1.0, 1.0),
                                                "fresh.usda", /*target_dir=*/"",
                                                /*overwrite=*/false);
        CHECK(fresh.status == 201);
    }

    slot.reset();
    CHECK(replacing.get() == 201);
    REQUIRE(idtx::tests::ReadTranslate(cube, "/Root/Cube", x, y, z));
    CHECK(x == doctest::Approx(3.0));
}
