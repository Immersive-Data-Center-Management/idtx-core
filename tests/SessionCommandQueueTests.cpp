// tests/SessionCommandQueueTests.cpp
//
// End-to-end checks of the per-session command queue over websocket and
// REST: request_id / server_seq semantics, exactly-once acks under load,
// a single authoritative order for concurrent editors, join snapshots, and
// commit / delete while commands are queued.

#include "thirdparty/doctest/doctest.h"

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

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

std::string CreateSession(const TestServer& server, const std::string& mode)
{
    HttpTestClient http;
    json req = {{"usd_file", "cube.usda"}, {"mode", mode}};
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

double TranslateX(const idtxcore::BaseMessage& broadcast)
{
    return broadcast.xform_broadcast().update().seperate().translation().x();
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

void SendUpdate(WsTestClient& ws, const std::string& sid, double x, std::uint64_t request_id)
{
    ws.SendBinary(idtx::tests::BuildTransformUpdate(sid, "cube.usda", kPrim,
                                                    x, 0.0, 0.0, request_id));
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

    std::set<std::uint64_t> seen;
    std::size_t   ok_count         = 0;
    std::size_t   queue_full_count = 0;
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
    MESSAGE("burst of " << kBurst << ": " << ok_count << " applied, "
                        << queue_full_count << " rejected with queue_full");
}

TEST_CASE("queue: concurrent editors observe a single server order")
{
    TestServer server;
    server.WaitReady();
    const std::string sid = CreateSession(server, "collaborative_edit");

    constexpr std::uint64_t kPerClient = 100;

    struct Client
    {
        WsTestClient                       ws;
        std::uint64_t                      base = 0;   // x value and request_id offset
        std::vector<idtxcore::BaseMessage> received;   // acks and broadcasts, in order
        bool                               io_failed = false;
    };
    Client a, b;
    a.base = 1000;
    b.base = 2000;
    Join(a.ws, server, sid);
    Join(b.ws, server, sid);

    auto run = [&sid](Client& c) {
        try
        {
            for (std::uint64_t i = 0; i < kPerClient; ++i)
            {
                SendUpdate(c.ws, sid, static_cast<double>(c.base + i), c.base + i);
            }
            // Own acks plus one broadcast for every update of the other client.
            c.received = Collect(c.ws, 2 * kPerClient, std::chrono::seconds{30},
                                 [](const idtxcore::BaseMessage& m) { return IsAck(m) || IsBroadcast(m); });
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
    REQUIRE(a.received.size() == 2 * kPerClient);
    REQUIRE(b.received.size() == 2 * kPerClient);

    // server_seq of the update each ack confirms, keyed by x value.
    std::map<std::uint64_t, double> seq_to_x;
    for (Client* c : {&a, &b})
    {
        std::uint64_t previous  = 0;
        bool          monotonic = true;
        std::uint64_t next_own  = c->base;
        bool          own_order = true;
        for (const auto& m : c->received)
        {
            if (m.server_seq() < previous) monotonic = false;
            previous = m.server_seq();
            if (IsAck(m))
            {
                CHECK(m.ack().ok());
                if (m.request_id() != next_own) own_order = false;
                ++next_own;
                const bool unique = seq_to_x.emplace(m.server_seq(),
                                                     static_cast<double>(m.request_id())).second;
                CHECK(unique);
            }
        }
        CHECK(monotonic);
        CHECK(own_order);
    }
    REQUIRE(seq_to_x.size() == 2 * kPerClient);

    // Each client saw every update of the other one, as the server applied it.
    for (auto [receiver, sender] : {std::pair{&a, &b}, std::pair{&b, &a}})
    {
        std::size_t broadcasts = 0;
        for (const auto& m : receiver->received)
        {
            if (!IsBroadcast(m)) continue;
            ++broadcasts;
            auto it = seq_to_x.find(m.server_seq());
            REQUIRE(it != seq_to_x.end());
            CHECK(TranslateX(m) == doctest::Approx(it->second));
            CHECK(it->second >= static_cast<double>(sender->base));
            CHECK(it->second <  static_cast<double>(sender->base + kPerClient));
        }
        CHECK(broadcasts == kPerClient);
    }

    // A late joiner sees the value of the update the server applied last.
    WsTestClient c;
    const auto join = Join(c, server, sid);
    const auto* cube = join.FrameFor(kPrim);
    REQUIRE(cube != nullptr);
    CHECK(join.snapshot_seq == seq_to_x.rbegin()->first);
    CHECK(TranslateX(*cube) == doctest::Approx(seq_to_x.rbegin()->second));
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
