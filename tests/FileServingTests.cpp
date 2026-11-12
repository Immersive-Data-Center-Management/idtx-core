// tests/FileServingTests.cpp
//

#include "thirdparty/doctest/doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

#include "support/HttpTestClient.h"
#include "support/TestServer.h"

using idtx::tests::HttpTestClient;
using idtx::tests::TestServer;
using json = nlohmann::json;

TEST_CASE("server is up and healthy")
{
    TestServer server;
    server.WaitReady();

    HttpTestClient http;
    auto r = http.Get(server.base_http_url() + "/api/v1/health");
    CHECK(r.status == 200);
    CHECK(r.text.find("\"status\":\"ok\"") != std::string::npos);
}

TEST_CASE("file listing exposes cube.usda")
{
    TestServer server;
    server.WaitReady();

    HttpTestClient http;
    auto r = http.Get(server.base_http_url() + "/api/v1/files");
    REQUIRE(r.status == 200);
    auto body = json::parse(r.text);
    REQUIRE(body.contains("files"));
    REQUIRE(body["files"].is_array());

    bool found = false;
    for (const auto& f : body["files"])
    {
        if (f.value("filename", "") == "cube.usda") { found = true; break; }
    }
    CHECK(found);
}

TEST_CASE("HEAD download reports 200 for existing USD file")
{
    TestServer server;
    server.WaitReady();

    HttpTestClient http;
    auto r = http.Head(server.base_http_url() + "/api/v1/download/cube.usda");
    CHECK(r.status == 200);
}

TEST_CASE("HEAD download reports 404 for unknown USD file")
{
    TestServer server;
    server.WaitReady();

    HttpTestClient http;
    auto r = http.Head(server.base_http_url() + "/api/v1/download/does_not_exist.usda");
    CHECK(r.status == 404);
}

TEST_CASE("GET download returns the file bytes")
{
    TestServer server;
    server.WaitReady();

    // Read the on-disk truth for comparison.
    std::ifstream f(server.uploads_root() / "cube.usda", std::ios::binary);
    std::string expected((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    REQUIRE(!expected.empty());

    HttpTestClient http;
    auto r = http.Get(server.base_http_url() + "/api/v1/download/cube.usda");
    CHECK(r.status == 200);
    CHECK(r.text == expected);
}

TEST_CASE("upload creates a file, thumbnails queued when worker present")
{
    TestServer server{TestServer::WithThumbnails()};
    server.WaitReady();

    // Reuse the fixture on disk as the "new" upload body.
    std::ifstream f(server.uploads_root() / "cube.usda", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
    REQUIRE(!bytes.empty());

    HttpTestClient http;
    auto r = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                  bytes, "uploaded_with_thumb.usda",
                                  /*target_dir=*/"", /*overwrite=*/true);
    REQUIRE(r.status == 201);
    auto body = json::parse(r.text);
    CHECK(body.value("filename",  "") == "uploaded_with_thumb.usda");
    CHECK(body.value("directory", "") == "");
    REQUIRE(body.contains("thumbnail"));
    // Depending on scheduling the worker may already have generated it, but
    // "queued" and "generated" are both acceptable — anything but "skipped".
    const std::string status = body["thumbnail"].value("status", "");
    CHECK((status == "queued" || status == "generated"));
    CHECK(std::filesystem::exists(server.uploads_root() / "uploaded_with_thumb.usda"));
}

TEST_CASE("upload with thumbnails disabled marks status=skipped")
{
    TestServer server{TestServer::WithoutThumbnails()};
    server.WaitReady();

    std::ifstream f(server.uploads_root() / "cube.usda", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
    REQUIRE(!bytes.empty());

    HttpTestClient http;
    auto r = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                  bytes, "no_thumb.usda",
                                  /*target_dir=*/"", /*overwrite=*/true);
    REQUIRE(r.status == 201);
    auto body = json::parse(r.text);
    REQUIRE(body.contains("thumbnail"));
    CHECK(body["thumbnail"].value("status", "") == "skipped");
    CHECK(std::filesystem::exists(server.uploads_root() / "no_thumb.usda"));
}
TEST_CASE("upload of an existing file without overwrite is rejected with 409")
{
    TestServer server{TestServer::WithoutThumbnails()};
    server.WaitReady();

    HttpTestClient http;
    auto r = http.UploadMultipart(server.base_http_url() + "/api/v1/upload",
                                  "#usda 1.0\n", "cube.usda",
                                  /*target_dir=*/"", /*overwrite=*/false);
    CHECK(r.status == 409);
}

TEST_CASE("an upload's reservation of its name ends with the upload")
{
    TestServer server{TestServer::WithoutThumbnails()};
    server.WaitReady();

    // Rejected, failed and successful uploads all release the name, so the
    // next upload of it is not turned away as still in progress.
    HttpTestClient http;
    const std::string url = server.base_http_url() + "/api/v1/upload";
    CHECK(http.UploadMultipart(url, "#usda 1.0\n", "cube.usda", "", /*overwrite=*/false).status == 409);
    CHECK(http.UploadMultipart(url, "#usda 1.0\n", "again.usda", "", /*overwrite=*/false).status == 201);
    CHECK(http.UploadMultipart(url, "#usda 1.0\n", "again.usda", "", /*overwrite=*/false).status == 409);
    CHECK(http.UploadMultipart(url, "#usda 1.0\n", "again.usda", "", /*overwrite=*/true).status == 201);
    CHECK(http.UploadMultipart(url, "#usda 1.0\n", "again.usda", "", /*overwrite=*/true).status == 201);
}

TEST_CASE("concurrent uploads of the same new name leave one complete file")
{
    TestServer server{TestServer::WithoutThumbnails()};
    server.WaitReady();

    // Large enough that the uploads overlap while they write.
    const std::string a(8 * 1024 * 1024, 'a');
    const std::string b(8 * 1024 * 1024, 'b');
    const std::string url = server.base_http_url() + "/api/v1/upload";

    auto first = std::async(std::launch::async, [&] {
        return HttpTestClient(std::chrono::milliseconds{60000})
            .UploadMultipart(url, a, "race.usda", "", /*overwrite=*/false).status;
    });
    auto second = std::async(std::launch::async, [&] {
        return HttpTestClient(std::chrono::milliseconds{60000})
            .UploadMultipart(url, b, "race.usda", "", /*overwrite=*/false).status;
    });
    const int s1 = first.get();
    const int s2 = second.get();

    // Exactly one upload wins; the other is told the name is busy (503) or,
    // if it came after the winner finished, that the file exists (409).
    CHECK(((s1 == 201) != (s2 == 201)));
    const int loser = s1 == 201 ? s2 : s1;
    CHECK((loser == 503 || loser == 409));

    std::ifstream f(server.uploads_root() / "race.usda", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK((bytes == a || bytes == b));
    CHECK_FALSE(std::filesystem::exists(server.uploads_root() / "race.usda.part"));
}
