// tests/support/HttpTestClient.h — thin cpr wrapper for the test suite.
//
// Existing tests already need cpr transitively (via the server's runtime
// dependency) — this helper just gives us shorter call sites and a single
// place to plug default timeouts.

#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

#include <cpr/cpr.h>

namespace idtx::tests
{

class HttpTestClient
{
public:
    struct Response
    {
        std::int32_t status = 0;
        std::string  text;
        cpr::Header  headers;
    };

    explicit HttpTestClient(std::chrono::milliseconds default_timeout =
                                std::chrono::milliseconds{5000})
        : m_timeout_(default_timeout) {}

    Response Get(const std::string& url,
                 std::initializer_list<std::pair<std::string, std::string>>
                     extra_headers = {}) const;

    Response Head(const std::string& url) const;

    Response PostJson(const std::string& url, const std::string& body) const;

    Response Delete(const std::string& url) const;

    /// Multipart upload matching the server's UploadFile endpoint.
    Response UploadMultipart(const std::string& url,
                             const std::string& file_bytes,
                             const std::string& filename,
                             const std::string& target_dir,
                             bool overwrite) const;

private:
    std::chrono::milliseconds m_timeout_;
};

} // namespace idtx::tests