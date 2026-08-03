// tests/SessionRestTests.cpp — covers requirement 4 (initiate SingleEdit
// session) and exercises the mode roundtrip introduced by the plan.

#include "thirdparty/doctest/doctest.h"

#include <nlohmann/json.hpp>
#include <string>

#include "support/HttpTestClient.h"
#include "support/TestServer.h"

using idtx::tests::HttpTestClient;
using idtx::tests::TestServer;
using json = nlohmann::json;

TEST_CASE("create single_edit session for cube.usda")
{
    TestServer server;
    server.WaitReady();

    HttpTestClient http;
    json req = {
        {"usd_file", "cube.usda"},
        {"mode",     "single_edit"}
    };
    auto r = http.PostJson(server.base_http_url() + "/api/v1/sessions", req.dump());
    REQUIRE(r.status == 201);

    auto body = json::parse(r.text);
    CHECK(body.value("usd_file", "") == "cube.usda");
    CHECK(body.value("mode",     "") == "single_edit");
    REQUIRE(body.contains("session_id"));
    REQUIRE(body.contains("ws_url"));
    const std::string sid = body["session_id"];
    CHECK(body["ws_url"] == std::string("/ws?sid=") + sid);

    // GET the session back and confirm the same shape.
    auto r2 = http.Get(server.base_http_url() + "/api/v1/sessions/" + sid);
    REQUIRE(r2.status == 200);
    auto body2 = json::parse(r2.text);
    CHECK(body2.value("session_id", "") == sid);
    CHECK(body2.value("mode",       "") == "single_edit");

    // Listing should now contain that one session.
    auto r3 = http.Get(server.base_http_url() + "/api/v1/sessions");
    REQUIRE(r3.status == 200);
    auto body3 = json::parse(r3.text);
    REQUIRE(body3.contains("sessions"));
    CHECK(body3["sessions"].is_array());
    CHECK(body3["sessions"].size() == 1);
}

TEST_CASE("create session rejects unknown USD file")
{
    TestServer server;
    server.WaitReady();

    HttpTestClient http;
    json req = {
        {"usd_file", "no_such_file.usda"},
        {"mode",     "single_edit"}
    };
    auto r = http.PostJson(server.base_http_url() + "/api/v1/sessions", req.dump());
    CHECK(r.status == 404);
}

TEST_CASE("delete session returns 204 and the session disappears")
{
    TestServer server;
    server.WaitReady();

    HttpTestClient http;
    json req = {{"usd_file", "cube.usda"}, {"mode", "collaborative_edit"}};
    auto created = http.PostJson(server.base_http_url() + "/api/v1/sessions", req.dump());
    REQUIRE(created.status == 201);
    const std::string sid = json::parse(created.text).value("session_id", "");

    auto del = http.Delete(server.base_http_url() + "/api/v1/sessions/" + sid);
    CHECK(del.status == 204);

    auto follow = http.Get(server.base_http_url() + "/api/v1/sessions/" + sid);
    CHECK(follow.status == 404);
}