// tests/SessionCommandQueueTests.cpp
//
// End-to-end checks of the per-session command queue over websocket and
// REST: request_id / server_seq semantics, exactly-once acks under load,
// a single authoritative order for concurrent editors, join snapshots,
// commit / delete while commands are queued, and the rejection of updates
// made on an outdated state together with the corrections that follow
// updates which were valid but not applied.

#include "thirdparty/doctest/doctest.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>

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

constexpr const char* kPrim = "/Root/Cube";

std::string CreateSession(const TestServer& server,
                          const std::string& mode,
                          const std::string& usd_file = "cube.usda")
{
    HttpTestClient http;
    json req = {{"usd_file", usd_file}, {"mode", mode}};
    auto r = http.PostJson(server.base_http_url() + "/api/v1/sessions", req.dump());
    REQUIRE(r.status == 201);
    return json::parse(r.text).value("session_id", "");
}

bool IsAck(const idtxcore::BaseMessage& m)
{
    return m.message_case() == idtxcore::BaseMessage::kAck;
}

bool IsBroadcast(const idtxcore::BaseMessage& m)
{
    return m.message_case() == idtxcore::BaseMessage::kXformBroadcast;
}

bool IsSnapshotComplete(const idtxcore::BaseMessage& m)
{
    return m.message_case() == idtxcore::BaseMessage::kSnapshotComplete;
}

bool IsAckError(const idtxcore::BaseMessage& m, const std::string& error)
{
    return IsAck(m) && !m.ack().ok() && m.ack().error() == error;
}

double TranslateX(const idtxcore::BaseMessage& broadcast)
{
    return broadcast.xform_broadcast().update().seperate().translation().x();
}

/// Read frames until the Ack for @p request_id arrives. Every frame read,
/// including the Ack, is appended to @p received.
std::optional<idtxcore::BaseMessage> WaitForAck(WsTestClient& ws,
                                                std::uint64_t request_id,
                                                std::vector<idtxcore::BaseMessage>& received)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto frame = ws.ReadBinary(std::chrono::milliseconds{2000});
        if (!frame) break;
        idtxcore::BaseMessage msg;
        if (!idtx::tests::ParseBaseMessage(*frame, msg)) continue;
        received.push_back(msg);
        if (IsAck(msg) && msg.request_id() == request_id) return msg;
    }
    return std::nullopt;
}

/// Round trip through the session's queue: an update of a prim that does not
/// exist, which is acked with apply_failed and changes nothing. Returns every
/// frame the server sent to @p ws before that Ack (a timed-out read would
/// break the connection, so "nothing else arrived" is checked this way).
std::vector<idtxcore::BaseMessage> Barrier(WsTestClient& ws, const std::string& sid)
{
    constexpr std::uint64_t kBarrierId = 0xB000'0000;
    ws.SendBinary(idtx::tests::BuildTransformUpdate(sid, "cube.usda", "/Barrier/Missing",
                                                    0.0, 0.0, 0.0, kBarrierId, 0));
    std::vector<idtxcore::BaseMessage> frames;
    REQUIRE(WaitForAck(ws, kBarrierId, frames).has_value());
    CHECK(IsAckError(frames.back(), "apply_failed"));
    frames.pop_back();
    return frames;
}

/// Highest server_seq in @p messages, at least @p floor.
std::uint64_t MaxSeq(const std::vector<idtxcore::BaseMessage>& messages, std::uint64_t floor = 0)
{
    for (const auto& m : messages) floor = std::max(floor, m.server_seq());
    return floor;
}

/// A TransformUpdate without a transform: the prim exists, but the update
/// cannot be applied.
std::string BuildEmptyUpdate(const std::string& sid, std::uint64_t request_id, std::uint64_t base)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(sid);
    msg.set_request_id(request_id);
    msg.set_server_seq(base);
    auto* upd = msg.mutable_xform_update();
    upd->set_session_id(sid);
    upd->set_usd_file("cube.usda");
    upd->set_prim_path(kPrim);
    std::string out;
    msg.SerializeToString(&out);
    return out;
}

/// A TransformUpdate with a matrix that only translates by @p x.
std::string BuildMatrixUpdate(const std::string& sid, const std::string& usd_file, double x,
                              std::uint64_t request_id, std::uint64_t base)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(sid);
    msg.set_request_id(request_id);
    msg.set_server_seq(base);
    auto* upd = msg.mutable_xform_update();
    upd->set_session_id(sid);
    upd->set_usd_file(usd_file);
    upd->set_prim_path(kPrim);
    auto* m = upd->mutable_matrix();
    m->set_m00(1.0);
    m->set_m11(1.0);
    m->set_m22(1.0);
    m->set_m33(1.0);
    m->set_m30(x); // USD matrices keep the translation in the last row
    std::string out;
    msg.SerializeToString(&out);
    return out;
}

/// Translation x of a broadcast in either transform form.
double TransformX(const idtxcore::BaseMessage& broadcast)
{
    const auto& upd = broadcast.xform_broadcast().update();
    return upd.has_matrix() ? upd.matrix().m30() : upd.seperate().translation().x();
}

/// Write a scene whose /Root/Cube has a leftover double scale that is not in
/// the op order. The server authors a float scale for separate transforms,
/// which USD refuses to add next to an existing attribute of the same name
/// with another type, so every separate update of the cube fails. Matrix
/// updates still work.
void WriteDoubleScaleCube(const TestServer& server, const std::string& file)
{
    std::ofstream out(server.uploads_root() / file);
    out << "#usda 1.0\n"
           "(\n    defaultPrim = \"Root\"\n)\n\n"
           "def Xform \"Root\"\n{\n"
           "    def Xform \"Cube\"\n    {\n"
           "        double3 xformOp:translate = (1, 2, 3)\n"
           "        double3 xformOp:scale = (2, 2, 2)\n"
           "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
           "    }\n}\n";
}

/// Read frames until @p count messages matching @p pred were collected or
/// @p timeout elapsed. Non-matching frames are dropped.
std::vector<idtxcore::BaseMessage> Collect(
    WsTestClient& ws,
    std::size_t count,
    std::chrono::milliseconds timeout,
    const std::function<bool(const idtxcore::BaseMessage&)>& pred)
{
    std::vector<idtxcore::BaseMessage> out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (out.size() < count)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;
        auto frame = ws.ReadBinary(remaining);
        if (!frame) break;
        idtxcore::BaseMessage msg;
        if (idtx::tests::ParseBaseMessage(*frame, msg) && pred(msg)) out.push_back(std::move(msg));
    }
    return out;
}

/// What a client receives while joining: xform frames up to and including
/// the snapshot, and the server_seq of the terminal SnapshotComplete.
struct JoinResult
{
    std::vector<idtxcore::BaseMessage> snapshot_frames;
    std::uint64_t                      snapshot_seq = 0;

    /// The snapshot frame for @p prim_path. Ancestors of an edited prim have
    /// specs in the session layer too, so they get frames of their own. A
    /// joining client can also receive live broadcasts before its snapshot
    /// (with server_seq <= snapshot_seq); the snapshot frames come last.
    const idtxcore::BaseMessage* FrameFor(const std::string& prim_path) const
    {
        for (auto it = snapshot_frames.rbegin(); it != snapshot_frames.rend(); ++it)
        {
            if (it->xform_broadcast().update().prim_path() == prim_path) return &*it;
        }
        return nullptr;
    }
};

JoinResult Join(WsTestClient& ws, const TestServer& server, const std::string& sid)
{
    auto conn = ws.Connect("127.0.0.1", server.port(), "/ws?sid=" + sid);
    REQUIRE(conn.handshake_ok);

    JoinResult result;
    bool handshake = false;
    bool complete  = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!complete && std::chrono::steady_clock::now() < deadline)
    {
        auto frame = ws.ReadBinary(std::chrono::milliseconds{2000});
        if (!frame) break;
        idtxcore::BaseMessage msg;
        if (!idtx::tests::ParseBaseMessage(*frame, msg)) continue;
        if (msg.message_case() == idtxcore::BaseMessage::kHandshake) handshake = true;
        else if (IsBroadcast(msg)) result.snapshot_frames.push_back(msg);
        else if (IsSnapshotComplete(msg))
        {
            for (const auto& f : result.snapshot_frames) CHECK(f.server_seq() <= msg.server_seq());
            result.snapshot_seq = msg.server_seq();
            complete = true;
        }
    }
    REQUIRE(handshake);
    REQUIRE(complete);
    return result;
}

void SendUpdate(WsTestClient& ws, const std::string& sid, double x, std::uint64_t request_id,
                std::uint64_t base = 0)
{
    ws.SendBinary(idtx::tests::BuildTransformUpdate(sid, "cube.usda", kPrim,
                                                    x, 0.0, 0.0, request_id, base));
}

} // namespace

TEST_CASE("queue: acks echo request_id and report increasing server_seq")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "single_edit");

    WsTestClient ws;
    const auto join = Join(ws, server, sid);

    for (std::uint64_t i = 1; i <= 5; ++i) SendUpdate(ws, sid, static_cast<double>(i), 100 + i);

    const auto acks = Collect(ws, 5, std::chrono::seconds{5}, IsAck);
    REQUIRE(acks.size() == 5);

    std::uint64_t previous_seq = join.snapshot_seq;
    for (std::size_t i = 0; i < acks.size(); ++i)
    {
        CHECK(acks[i].ack().ok());
        CHECK(acks[i].request_id() == 101 + i);
        CHECK(acks[i].session_id() == sid);
        CHECK(acks[i].server_seq() > previous_seq);
        previous_seq = acks[i].server_seq();
    }
}

TEST_CASE("queue: a failed update is acked with apply_failed and does not advance server_seq")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "single_edit");

    WsTestClient ws;
    Join(ws, server, sid);

    SendUpdate(ws, sid, 1.0, 1);
    ws.SendBinary(idtx::tests::BuildTransformUpdate(sid, "cube.usda", "/Root/Missing",
                                                    2.0, 0.0, 0.0, 2));
    SendUpdate(ws, sid, 3.0, 3);

    const auto acks = Collect(ws, 3, std::chrono::seconds{5}, IsAck);
    REQUIRE(acks.size() == 3);
    CHECK(acks[0].ack().ok());
    CHECK_FALSE(acks[1].ack().ok());
    CHECK(acks[1].request_id() == 2);
    CHECK(acks[1].ack().error() == "apply_failed");
    CHECK(acks[1].server_seq() == acks[0].server_seq());
    CHECK(acks[2].ack().ok());
    CHECK(acks[2].server_seq() == acks[0].server_seq() + 1);
}

TEST_CASE("queue: every update of a burst gets exactly one ack")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient ws;
    Join(ws, server, sid);

    // Larger than the session's queue, sent without waiting for any ack.
    constexpr std::uint64_t kBurst = 3000;
    for (std::uint64_t i = 1; i <= kBurst; ++i) SendUpdate(ws, sid, static_cast<double>(i), i);

    const auto acks = Collect(ws, kBurst, std::chrono::seconds{60}, IsAck);
    REQUIRE(acks.size() == kBurst);

    // After the first queue_full the client gets a correction, and its queued
    // updates, all sent with base 0, become stale.
    std::set<std::uint64_t> seen;
    std::size_t   ok_count         = 0;
    std::size_t   queue_full_count = 0;
    std::size_t   stale_count      = 0;
    std::uint64_t last_ok_seq      = 0;
    bool          ok_seq_increasing = true;
    bool          errors_known      = true;
    for (const auto& ack : acks)
    {
        seen.insert(ack.request_id());
        if (ack.ack().ok())
        {
            ++ok_count;
            if (ack.server_seq() <= last_ok_seq) ok_seq_increasing = false;
            last_ok_seq = ack.server_seq();
        }
        else if (ack.ack().error() == "queue_full")
        {
            ++queue_full_count;
            CHECK(ack.server_seq() == 0);
        }
        else if (ack.ack().error() == "stale")
        {
            ++stale_count;
        }
        else
        {
            errors_known = false;
        }
    }
    CHECK(seen.size() == kBurst);
    CHECK(*seen.begin() == 1);
    CHECK(*seen.rbegin() == kBurst);
    CHECK(ok_count > 0);
    CHECK(ok_seq_increasing);
    CHECK(errors_known);
    if (queue_full_count == 0) CHECK(stale_count == 0);
    MESSAGE("burst of " << kBurst << ": " << ok_count << " applied, "
                        << queue_full_count << " rejected with queue_full, "
                        << stale_count << " rejected as stale");
}

TEST_CASE("queue: concurrent editors observe a single server order and converge")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    constexpr std::uint64_t kPerClient = 100;

    // Behaves like a real client: shows its own update right away, applies
    // every broadcast it receives and sends the highest server_seq it has
    // received as the base of its next update.
    struct Client
    {
        WsTestClient                       ws;
        std::uint64_t                      first = 0;  // x value and request_id offset
        std::uint64_t                      base  = 0;
        double                             shown = 0.0;
        std::vector<idtxcore::BaseMessage> received;   // everything after the join, in order
        bool                               io_failed = false;

        void Apply(std::vector<idtxcore::BaseMessage> messages)
        {
            for (auto& m : messages)
            {
                base = std::max(base, m.server_seq());
                if (IsBroadcast(m)) shown = TranslateX(m);
                received.push_back(std::move(m));
            }
        }
    };
    Client a, b;
    a.first = 1000;
    b.first = 2000;
    a.base  = Join(a.ws, server, sid).snapshot_seq;
    b.base  = Join(b.ws, server, sid).snapshot_seq;

    auto run = [&sid](Client& c) {
        try
        {
            for (std::uint64_t i = 0; i < kPerClient; ++i)
            {
                const std::uint64_t x = c.first + i;
                SendUpdate(c.ws, sid, static_cast<double>(x), x, c.base);
                c.shown = static_cast<double>(x);

                std::vector<idtxcore::BaseMessage> frames;
                const bool acked = WaitForAck(c.ws, x, frames).has_value();
                c.Apply(std::move(frames));
                if (!acked)
                {
                    c.io_failed = true;
                    return;
                }
            }
        }
        catch (...)
        {
            c.io_failed = true;
        }
    };
    std::thread ta(run, std::ref(a));
    std::thread tb(run, std::ref(b));
    ta.join();
    tb.join();
    REQUIRE_FALSE(a.io_failed);
    REQUIRE_FALSE(b.io_failed);

    // Broadcasts of the other client's last updates may still be on the way.
    a.Apply(Barrier(a.ws, sid));
    b.Apply(Barrier(b.ws, sid));

    // x value of every applied update, keyed by the server_seq of its Ack.
    std::map<std::uint64_t, double> seq_to_x;
    std::size_t stale = 0;
    for (Client* c : {&a, &b})
    {
        std::uint64_t previous  = 0;
        bool          monotonic = true;
        std::uint64_t next_own  = c->first;
        bool          own_order = true;
        bool          known     = true;
        for (const auto& m : c->received)
        {
            if (m.server_seq() < previous) monotonic = false;
            previous = m.server_seq();
            if (!IsAck(m)) continue;

            if (m.request_id() != next_own) own_order = false;
            ++next_own;
            if (m.ack().ok())
            {
                const bool unique = seq_to_x.emplace(m.server_seq(),
                                                     static_cast<double>(m.request_id())).second;
                CHECK(unique);
            }
            else if (m.ack().error() == "stale")
            {
                ++stale;
            }
            else
            {
                known = false;
            }
        }
        CHECK(monotonic);
        CHECK(own_order);
        CHECK(known);
        CHECK(next_own == c->first + kPerClient);
    }
    REQUIRE_FALSE(seq_to_x.empty());

    // Each client saw every applied update of the other one, as the server
    // applied it.
    for (auto [receiver, sender] : {std::pair{&a, &b}, std::pair{&b, &a}})
    {
        std::size_t broadcasts = 0;
        std::size_t sender_ok  = 0;
        for (const auto& m : sender->received) if (IsAck(m) && m.ack().ok()) ++sender_ok;
        for (const auto& m : receiver->received)
        {
            if (!IsBroadcast(m)) continue;
            ++broadcasts;
            auto it = seq_to_x.find(m.server_seq());
            REQUIRE(it != seq_to_x.end());
            CHECK(TranslateX(m) == doctest::Approx(it->second));
            CHECK(it->second >= static_cast<double>(sender->first));
            CHECK(it->second <  static_cast<double>(sender->first + kPerClient));
        }
        CHECK(broadcasts == sender_ok);
    }

    // Both clients show the value the server applied last.
    WsTestClient c;
    const auto join = Join(c, server, sid);
    const auto* cube = join.FrameFor(kPrim);
    REQUIRE(cube != nullptr);
    CHECK(join.snapshot_seq == seq_to_x.rbegin()->first);
    CHECK(TranslateX(*cube) == doctest::Approx(seq_to_x.rbegin()->second));
    CHECK(a.shown == doctest::Approx(seq_to_x.rbegin()->second));
    CHECK(b.shown == doctest::Approx(seq_to_x.rbegin()->second));
    MESSAGE(seq_to_x.size() << " updates applied, " << stale << " rejected as stale");
}

TEST_CASE("queue: join snapshot matches the state and precedes later broadcasts")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient a;
    const auto empty_join = Join(a, server, sid);
    CHECK(empty_join.snapshot_frames.empty());

    for (std::uint64_t i = 1; i <= 3; ++i) SendUpdate(a, sid, static_cast<double>(i), i);
    const auto acks = Collect(a, 3, std::chrono::seconds{5}, IsAck);
    REQUIRE(acks.size() == 3);
    const std::uint64_t state_seq = acks.back().server_seq();

    WsTestClient b;
    const auto join = Join(b, server, sid);
    CHECK(join.snapshot_seq == state_seq);
    const auto* cube = join.FrameFor(kPrim);
    REQUIRE(cube != nullptr);
    for (const auto& f : join.snapshot_frames) CHECK(f.server_seq() == state_seq);
    CHECK(TranslateX(*cube) == doctest::Approx(3.0));

    SendUpdate(a, sid, 4.0, 4);
    const auto next = Collect(b, 1, std::chrono::seconds{5}, IsBroadcast);
    REQUIRE(next.size() == 1);
    CHECK(next.front().server_seq() > state_seq);
    CHECK(TranslateX(next.front()) == doctest::Approx(4.0));
}

TEST_CASE("queue: commit writes every acknowledged update to the file")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "single_edit");

    WsTestClient ws;
    Join(ws, server, sid);
    ws.SendBinary(idtx::tests::BuildTransformUpdate(sid, "cube.usda", kPrim, 4.0, 5.0, 6.0, 1));
    const auto acks = Collect(ws, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(acks.size() == 1);
    REQUIRE(acks.front().ack().ok());

    HttpTestClient http;
    const auto commit = http.PostJson(
        server.base_http_url() + "/api/v1/sessions/" + sid + "/commit", "{}");
    REQUIRE(commit.status == 200);
    CHECK(json::parse(commit.text).value("committed", false));

    double x = 0, y = 0, z = 0;
    REQUIRE(idtx::tests::ReadTranslate(server.uploads_root() / "cube.usda", kPrim, x, y, z));
    CHECK(x == doctest::Approx(4.0));
    CHECK(y == doctest::Approx(5.0));
    CHECK(z == doctest::Approx(6.0));

    const auto unknown = http.PostJson(
        server.base_http_url() + "/api/v1/sessions/does-not-exist/commit", "{}");
    CHECK(unknown.status == 404);
}

TEST_CASE("queue: after a commit the server's root layer already holds the committed content")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "single_edit");

    WsTestClient ws;
    Join(ws, server, sid);
    ws.SendBinary(idtx::tests::BuildTransformUpdate(sid, "cube.usda", kPrim, 4.0, 5.0, 6.0, 1));
    const auto acks = Collect(ws, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(acks.size() == 1);
    REQUIRE(acks.front().ack().ok());

    HttpTestClient http;
    REQUIRE(http.PostJson(server.base_http_url() + "/api/v1/sessions/" + sid + "/commit",
                          "{}").status == 200);

    // The in-memory root layer the server shares through the layer registry,
    // which a session created now would be opened on. It must match the file
    // as soon as the commit has answered, not only after a queued reload.
    const auto root = pxr::SdfLayer::Find((server.uploads_root() / "cube.usda").string());
    REQUIRE(root);
    const auto attr = root->GetAttributeAtPath(pxr::SdfPath("/Root/Cube.xformOp:translate"));
    REQUIRE(attr);
    const pxr::VtValue value = attr->GetDefaultValue();
    REQUIRE(value.IsHolding<pxr::GfVec3d>());
    CHECK(value.UncheckedGet<pxr::GfVec3d>() == pxr::GfVec3d(4.0, 5.0, 6.0));
}

TEST_CASE("queue: deleting a session with queued updates drains them cleanly")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient ws;
    Join(ws, server, sid);
    for (std::uint64_t i = 1; i <= 500; ++i) SendUpdate(ws, sid, static_cast<double>(i), i);

    HttpTestClient http;
    const auto del = http.Delete(server.base_http_url() + "/api/v1/sessions/" + sid);
    CHECK(del.status == 204);
    CHECK(http.Get(server.base_http_url() + "/api/v1/sessions/" + sid).status == 404);
    CHECK(http.Get(server.base_http_url() + "/api/v1/health").status == 200);
}

TEST_CASE("queue: a client that disconnects after its session was deleted is detached")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient ws;
    Join(ws, server, sid);

    // Keep the session object alive past the delete to inspect its clients.
    const auto session = server.session_manager().Get(sid);
    REQUIRE(session != nullptr);
    const auto client_count = [&session] {
        std::shared_lock lock(session->clients_mutex);
        return session->clients.size();
    };
    REQUIRE(client_count() == 1);

    HttpTestClient http;
    REQUIRE(http.Delete(server.base_http_url() + "/api/v1/sessions/" + sid).status == 204);
    // Still attached: the delete does not close websockets.
    REQUIRE(client_count() == 1);

    // The session is no longer found by id, so the close handler has to
    // detach through the session itself. A connection left in the registry
    // would be freed by crow while the session could still send to it.
    ws.Close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (client_count() != 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    CHECK(client_count() == 0);
}

TEST_CASE("queue: updates are applied even if their sender disconnects right away")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    {
        WsTestClient a;
        Join(a, server, sid);
        for (std::uint64_t i = 1; i <= 50; ++i) SendUpdate(a, sid, static_cast<double>(i), i);
        a.Close();
    }

    // The close frame followed the updates on the same connection, so every
    // update was queued before the new client's join snapshot.
    WsTestClient b;
    const auto join = Join(b, server, sid);
    const auto* cube = join.FrameFor(kPrim);
    REQUIRE(cube != nullptr);
    CHECK(TranslateX(*cube) == doctest::Approx(50.0));
}

TEST_CASE("stale: an update made on an outdated state is rejected")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient a, b;
    Join(a, server, sid);
    Join(b, server, sid);

    SendUpdate(b, sid, 9.0, 1);
    const auto b_ack = Collect(b, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(b_ack.size() == 1);
    REQUIRE(b_ack.front().ack().ok());
    const std::uint64_t b_seq = b_ack.front().server_seq();

    const auto seen = Collect(a, 1, std::chrono::seconds{5}, IsBroadcast);
    REQUIRE(seen.size() == 1);
    CHECK(seen.front().server_seq() == b_seq);

    // A did not take B's change into account.
    SendUpdate(a, sid, 5.0, 2, /*base=*/0);
    const auto stale = Collect(a, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(stale.size() == 1);
    CHECK(IsAckError(stale.front(), "stale"));
    CHECK(stale.front().request_id() == 2);
    CHECK(stale.front().server_seq() == b_seq);
    // No correction to A and nothing for B.
    CHECK(Barrier(a, sid).empty());
    CHECK(Barrier(b, sid).empty());

    {
        WsTestClient c;
        const auto join = Join(c, server, sid);
        const auto* cube = join.FrameFor(kPrim);
        REQUIRE(cube != nullptr);
        CHECK(TranslateX(*cube) == doctest::Approx(9.0));
    }

    // Based on B's change, the same update is accepted.
    SendUpdate(a, sid, 5.0, 3, b_seq);
    const auto ok = Collect(a, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(ok.size() == 1);
    CHECK(ok.front().ack().ok());
    const auto to_b = Collect(b, 1, std::chrono::seconds{5}, IsBroadcast);
    REQUIRE(to_b.size() == 1);
    CHECK(TranslateX(to_b.front()) == doctest::Approx(5.0));
}

TEST_CASE("stale: a client's own earlier updates never make its update stale")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient a, b;
    Join(a, server, sid);
    Join(b, server, sid);

    SendUpdate(b, sid, 1.0, 1);
    const auto seen = Collect(a, 1, std::chrono::seconds{5}, IsBroadcast);
    REQUIRE(seen.size() == 1);
    const std::uint64_t base = seen.front().server_seq();

    // A drag: many updates sent without waiting, all with the same base.
    constexpr std::uint64_t kDrag = 50;
    for (std::uint64_t i = 1; i <= kDrag; ++i) SendUpdate(a, sid, static_cast<double>(i), i, base);
    const auto acks = Collect(a, kDrag, std::chrono::seconds{10}, IsAck);
    REQUIRE(acks.size() == kDrag);
    for (const auto& ack : acks) CHECK(ack.ack().ok());

    const auto to_b = Collect(b, kDrag, std::chrono::seconds{10}, IsBroadcast);
    REQUIRE(to_b.size() == kDrag);
    CHECK(TranslateX(to_b.back()) == doctest::Approx(static_cast<double>(kDrag)));
}

TEST_CASE("stale: a base the client was never sent is rejected as invalid_base")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient a, b;
    Join(a, server, sid);
    Join(b, server, sid);

    // Above the current server_seq.
    SendUpdate(a, sid, 5.0, 1, /*base=*/7);
    const auto above = Collect(a, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(above.size() == 1);
    CHECK(IsAckError(above.front(), "invalid_base"));
    CHECK(above.front().server_seq() == 0);

    // B's failed update produces a correction that only B receives, so its
    // version exists but was never sent to A.
    b.SendBinary(BuildEmptyUpdate(sid, 1, 0));
    std::vector<idtxcore::BaseMessage> b_frames;
    REQUIRE(WaitForAck(b, 1, b_frames).has_value());
    b_frames = Barrier(b, sid);
    REQUIRE(b_frames.size() == 1);
    REQUIRE(IsBroadcast(b_frames.front()));
    const std::uint64_t b_only = b_frames.front().server_seq();
    CHECK(b_only > 0);

    SendUpdate(a, sid, 5.0, 2, b_only);
    const auto unsent = Collect(a, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(unsent.size() == 1);
    CHECK(IsAckError(unsent.front(), "invalid_base"));
    CHECK(unsent.front().server_seq() == b_only);

    // No correction follows and the stage is unchanged.
    CHECK(Barrier(a, sid).empty());
    CHECK(Barrier(b, sid).empty());
    WsTestClient c;
    const auto join = Join(c, server, sid);
    CHECK(join.FrameFor(kPrim) == nullptr);
}

TEST_CASE("stale: an update based on a version before a reload is stale")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient a;
    Join(a, server, sid);

    HttpTestClient http;
    const auto upload = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                             idtx::tests::BuildCubeUsdaBytes(9.0, 8.0, 7.0),
                                             "cube.usda", /*target_dir=*/"", /*overwrite=*/true);
    REQUIRE(upload.status == 201);
    const auto reload = Collect(a, 1, std::chrono::seconds{5}, IsBroadcast);
    REQUIRE(reload.size() == 1);
    const std::uint64_t reload_seq = reload.front().server_seq();
    CHECK(reload_seq > 0);

    SendUpdate(a, sid, 1.0, 1, /*base=*/0);
    const auto stale = Collect(a, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(stale.size() == 1);
    CHECK(IsAckError(stale.front(), "stale"));

    SendUpdate(a, sid, 1.0, 2, reload_seq);
    const auto ok = Collect(a, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(ok.size() == 1);
    CHECK(ok.front().ack().ok());
}

TEST_CASE("correction: apply_failed on an existing prim leaves it untouched and corrects the origin")
{
    TestServer server;
    server.WaitReady();

    const std::string file = "double_scale_cube.usda";
    WriteDoubleScaleCube(server, file);
    const std::string sid = CreateSession(server, "collaborative_edit", file);

    WsTestClient a, b;
    Join(a, server, sid);
    Join(b, server, sid);

    // The second update is in flight with the old base when the correction
    // is sent.
    a.SendBinary(idtx::tests::BuildTransformUpdate(sid, file, kPrim, 4.0, 0.0, 0.0, 1, 0));
    a.SendBinary(idtx::tests::BuildTransformUpdate(sid, file, kPrim, 6.0, 0.0, 0.0, 2, 0));

    std::vector<idtxcore::BaseMessage> frames;
    REQUIRE(WaitForAck(a, 2, frames).has_value());
    REQUIRE(frames.size() == 3);
    CHECK(IsAckError(frames[0], "apply_failed"));
    CHECK(frames[0].request_id() == 1);
    REQUIRE(IsBroadcast(frames[1]));
    CHECK(frames[1].server_seq() == frames[0].server_seq() + 1);
    CHECK(TranslateX(frames[1]) == doctest::Approx(1.0));
    CHECK(frames[1].xform_broadcast().update().seperate().translation().y() == doctest::Approx(2.0));
    CHECK(IsAckError(frames[2], "stale"));
    CHECK(frames[2].request_id() == 2);

    CHECK(Barrier(a, sid).empty());
    CHECK(Barrier(b, sid).empty());

    // Nothing was authored into the session layer.
    WsTestClient c;
    const auto join = Join(c, server, sid);
    CHECK(join.FrameFor(kPrim) == nullptr);
}

TEST_CASE("correction: carries another client's change instead of the file value")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient a, b;
    Join(a, server, sid);
    Join(b, server, sid);

    SendUpdate(b, sid, 9.0, 1);
    const auto b_ack = Collect(b, 1, std::chrono::seconds{5}, IsAck);
    REQUIRE(b_ack.size() == 1);
    REQUIRE(b_ack.front().ack().ok());
    const auto seen = Collect(a, 1, std::chrono::seconds{5}, IsBroadcast);
    REQUIRE(seen.size() == 1);
    const std::uint64_t b_seq = seen.front().server_seq();

    // Based on B's change, so it is not stale, but it cannot be applied.
    a.SendBinary(BuildEmptyUpdate(sid, 1, b_seq));
    std::vector<idtxcore::BaseMessage> frames;
    REQUIRE(WaitForAck(a, 1, frames).has_value());
    REQUIRE(frames.size() == 1);
    CHECK(IsAckError(frames.front(), "apply_failed"));
    CHECK(frames.front().server_seq() == b_seq);

    const auto correction = Barrier(a, sid);
    REQUIRE(correction.size() == 1);
    REQUIRE(IsBroadcast(correction.front()));
    CHECK(correction.front().server_seq() == b_seq + 1);
    CHECK(TranslateX(correction.front()) == doctest::Approx(9.0));
    CHECK(Barrier(b, sid).empty());
}

TEST_CASE("correction: a client whose updates fail ends on the state another client keeps changing")
{
    TestServer server;
    server.WaitReady();

    // A's separate updates always fail on this scene, B's matrix updates
    // succeed, so A's corrections race B's changes.
    const std::string file = "double_scale_cube.usda";
    WriteDoubleScaleCube(server, file);
    const std::string sid = CreateSession(server, "collaborative_edit", file);

    constexpr std::uint64_t kUpdates = 50;

    // Behaves like a real client: shows its own update right away, applies
    // every broadcast it receives and sends the highest server_seq it has
    // received as the base of its next update.
    struct Client
    {
        WsTestClient                       ws;
        std::uint64_t                      base  = 0;
        double                             shown = 0.0;
        std::map<std::string, std::size_t> acks;        // "ok" or the error
        std::size_t                        broadcasts = 0;
        bool                               io_failed  = false;

        void Apply(const std::vector<idtxcore::BaseMessage>& messages)
        {
            for (const auto& m : messages)
            {
                base = std::max(base, m.server_seq());
                if (IsBroadcast(m))
                {
                    shown = TransformX(m);
                    ++broadcasts;
                }
                else if (IsAck(m))
                {
                    ++acks[m.ack().ok() ? "ok" : m.ack().error()];
                }
            }
        }
    };
    Client a, b;
    a.base = Join(a.ws, server, sid).snapshot_seq;
    b.base = Join(b.ws, server, sid).snapshot_seq;

    // Sends kUpdates updates with x = offset + 1 .. offset + kUpdates.
    auto run = [](Client& c, double offset,
                  const std::function<std::string(std::uint64_t, double)>& build) {
        try
        {
            for (std::uint64_t i = 1; i <= kUpdates; ++i)
            {
                const double x = offset + static_cast<double>(i);
                c.ws.SendBinary(build(i, x));
                c.shown = x;

                std::vector<idtxcore::BaseMessage> frames;
                const bool acked = WaitForAck(c.ws, i, frames).has_value();
                c.Apply(frames);
                if (!acked)
                {
                    c.io_failed = true;
                    return;
                }
            }
        }
        catch (...)
        {
            c.io_failed = true;
        }
    };
    std::thread ta(run, std::ref(a), 1000.0, [&](std::uint64_t id, double x) {
        return idtx::tests::BuildTransformUpdate(sid, file, kPrim, x, 0.0, 0.0, id, a.base);
    });
    std::thread tb(run, std::ref(b), 2000.0, [&](std::uint64_t id, double x) {
        return BuildMatrixUpdate(sid, file, x, id, b.base);
    });
    ta.join();
    tb.join();
    REQUIRE_FALSE(a.io_failed);
    REQUIRE_FALSE(b.io_failed);

    // Corrections and broadcasts may still be on the way.
    a.Apply(Barrier(a.ws, sid));
    b.Apply(Barrier(b.ws, sid));

    // A never changes the prim, so nothing makes B's updates stale.
    CHECK(b.acks["ok"] == kUpdates);
    CHECK(a.acks["ok"] == 0);
    CHECK(a.acks["apply_failed"] + a.acks["stale"] == kUpdates);
    CHECK(a.acks["apply_failed"] > 0);
    // B's changes plus one correction per apply_failed.
    CHECK(a.broadcasts == kUpdates + a.acks["apply_failed"]);
    CHECK(b.broadcasts == 0);

    // Both clients show the server state.
    WsTestClient c;
    const auto join = Join(c, server, sid);
    const auto* cube = join.FrameFor(kPrim);
    REQUIRE(cube != nullptr);
    CHECK(TransformX(*cube) == doctest::Approx(2000.0 + static_cast<double>(kUpdates)));
    CHECK(a.shown == doctest::Approx(TransformX(*cube)));
    CHECK(b.shown == doctest::Approx(TransformX(*cube)));
    MESSAGE("A: " << a.acks["apply_failed"] << " apply_failed with correction, "
                  << a.acks["stale"] << " stale");
}

TEST_CASE("correction: queue_full is followed by a correction the client ends with")
{
    TestServer server;
    server.WaitReady();
    // Collaborative, so a second client can join to read the server state.
    const std::string sid = CreateSession(server, "collaborative_edit");

    WsTestClient ws;
    Join(ws, server, sid);

    // Larger than the queue, unpaced, all with base 0.
    constexpr std::uint64_t kBurst = 5000;
    for (std::uint64_t i = 1; i <= kBurst; ++i) SendUpdate(ws, sid, static_cast<double>(i), i);

    std::vector<idtxcore::BaseMessage> received;
    std::size_t acks = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (acks < kBurst && std::chrono::steady_clock::now() < deadline)
    {
        auto frame = ws.ReadBinary(std::chrono::milliseconds{5000});
        if (!frame) break;
        idtxcore::BaseMessage msg;
        if (!idtx::tests::ParseBaseMessage(*frame, msg)) continue;
        if (IsAck(msg)) ++acks;
        received.push_back(std::move(msg));
    }
    REQUIRE(acks == kBurst);
    // Corrections for the last rejections may still follow.
    for (auto& m : Barrier(ws, sid)) received.push_back(std::move(m));

    std::size_t queue_full  = 0;
    std::size_t corrections = 0;
    bool        ok_after_correction = false;
    const idtxcore::BaseMessage* last_correction = nullptr;
    for (const auto& m : received)
    {
        if (IsBroadcast(m))
        {
            // The only client: every broadcast it gets is a correction.
            ++corrections;
            last_correction = &m;
        }
        else if (IsAckError(m, "queue_full"))
        {
            ++queue_full;
        }
        else if (IsAck(m) && m.ack().ok() && corrections > 0)
        {
            ok_after_correction = true;
        }
    }
    REQUIRE(queue_full > 0);
    REQUIRE(corrections > 0);
    CHECK(corrections <= queue_full);
    // Every update still queued when the first correction was sent is stale.
    CHECK_FALSE(ok_after_correction);

    // The last correction carries the server state.
    {
        WsTestClient c;
        const auto join = Join(c, server, sid);
        const auto* cube = join.FrameFor(kPrim);
        REQUIRE(cube != nullptr);
        CHECK(TranslateX(*last_correction) == doctest::Approx(TranslateX(*cube)));
    }

    // Based on what the client received, a new update is accepted again.
    const std::uint64_t base = MaxSeq(received);
    SendUpdate(ws, sid, -1.0, kBurst + 1, /*base=*/0);
    SendUpdate(ws, sid, -2.0, kBurst + 2, base);
    std::vector<idtxcore::BaseMessage> tail;
    REQUIRE(WaitForAck(ws, kBurst + 2, tail).has_value());
    std::vector<idtxcore::BaseMessage> tail_acks;
    for (const auto& m : tail) if (IsAck(m)) tail_acks.push_back(m);
    REQUIRE(tail_acks.size() == 2);
    CHECK(IsAckError(tail_acks[0], "stale"));
    CHECK(tail_acks[1].ack().ok());
    MESSAGE("burst of " << kBurst << ": " << queue_full << " rejected with queue_full, "
                        << corrections << " correction(s)");
}
