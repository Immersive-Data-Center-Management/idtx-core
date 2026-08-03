#include "HttpClient.h"

using namespace idtx::http;

Response HttpClient::get(const std::string& url, std::int32_t timeout_ms, const cpr::Header& headers, const std::optional<std::string>& user_agent)
{
    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    session.SetTimeout(cpr::Timeout{timeout_ms});
    session.SetOption(cpr::Redirect{true});

    if (!headers.empty())
    {
        session.SetHeader(headers);
    }
    if (user_agent)
    {
        session.SetUserAgent(cpr::UserAgent{*user_agent});
    }

    cpr::Response response = session.Get();
    const bool http_ok = (response.status_code >= 200 && response.status_code < 300);
    const bool curl_ok = (response.error.code == cpr::ErrorCode::OK);

    std::string err;
    if (!curl_ok)
    {
        err = response.error.message;
    }
    else if (!http_ok)
    {
        err = "HTTP " + std::to_string(response.status_code);
    }

    return {curl_ok && http_ok, std::move(response.text), (response.status_code), std::move(err)};
}

Response HttpClient::post(const std::string& url, const cpr::Payload& payload, const cpr::Header& headers,
                            const std::optional<std::string>& user_agent, std::int32_t timeout_ms)
{
    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    session.SetTimeout(cpr::Timeout{timeout_ms});
    session.SetOption(cpr::Redirect{true});

    if (!headers.empty())
    {
        session.SetHeader(headers);
    }

    if (user_agent)
    {
        session.SetUserAgent(cpr::UserAgent{*user_agent});
    }

    session.SetPayload(payload);

    cpr::Response response = session.Post();

    const bool http_ok = (response.status_code >= 200 && response.status_code < 300);
    const bool curl_ok = (response.error.code == cpr::ErrorCode::OK);

    std::string err;
    if (!curl_ok)
    {
        err = response.error.message;
    }
    else if (!http_ok)
    {
        err = "HTTP " + std::to_string(response.status_code);
    }

    return {curl_ok && http_ok, std::move(response.text), response.status_code, std::move(err)};
}
