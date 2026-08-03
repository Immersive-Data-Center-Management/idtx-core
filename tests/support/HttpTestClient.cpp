#include "HttpTestClient.h"

#include <cpr/cpr.h>

namespace idtx::tests
{

namespace
{

HttpTestClient::Response ToResponse(const cpr::Response& r)
{
    HttpTestClient::Response out;
    out.status  = r.status_code;
    out.text    = r.text;
    out.headers = r.header;
    return out;
}

cpr::Header MakeHeaders(
    std::initializer_list<std::pair<std::string, std::string>> extra)
{
    cpr::Header h;
    for (const auto& p : extra) h[p.first] = p.second;
    return h;
}

} // namespace

HttpTestClient::Response HttpTestClient::Get(
    const std::string& url,
    std::initializer_list<std::pair<std::string, std::string>> extra) const
{
    return ToResponse(cpr::Get(cpr::Url{url},
                               MakeHeaders(extra),
                               cpr::Timeout{m_timeout_}));
}

HttpTestClient::Response HttpTestClient::Head(const std::string& url) const
{
    return ToResponse(cpr::Head(cpr::Url{url}, cpr::Timeout{m_timeout_}));
}

HttpTestClient::Response HttpTestClient::PostJson(const std::string& url,
                                                  const std::string& body) const
{
    return ToResponse(cpr::Post(
        cpr::Url{url},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{body},
        cpr::Timeout{m_timeout_}));
}

HttpTestClient::Response HttpTestClient::Delete(const std::string& url) const
{
    return ToResponse(cpr::Delete(cpr::Url{url}, cpr::Timeout{m_timeout_}));
}

HttpTestClient::Response HttpTestClient::UploadMultipart(
    const std::string& url,
    const std::string& file_bytes,
    const std::string& filename,
    const std::string& target_dir,
    bool overwrite) const
{
    // cpr::Multipart carries the "file" buffer as cpr::Buffer plus separate
    // string parts for filename/path/overwrite. This mirrors what the server
    // extracts in FileServingController::UploadFile.
    cpr::Multipart parts{
        {"file",
         cpr::Buffer{file_bytes.begin(), file_bytes.end(), filename}},
        {"filename",  filename},
        {"path",      target_dir},
        {"overwrite", overwrite ? std::string("true") : std::string("false")}
    };
    return ToResponse(cpr::Post(cpr::Url{url},
                                std::move(parts),
                                cpr::Timeout{m_timeout_}));
}

} // namespace idtx::tests