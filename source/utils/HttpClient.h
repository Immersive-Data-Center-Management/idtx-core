#pragma once

#include <cpr/cpr.h>
#include <string>
#include <optional>

namespace idtx
{
namespace http
{
    struct Response {
        bool ok;
        std::string body;
        long code;
        std::string err;
    };

    class HttpClient {
    public:
        /**
         * @brief Blocking HTTP GET. Returns {ok, body, code, err}.
         *
         * ok is true if and only if no transport error occurred and the HTTP status is 2xx.
         *
         * @param url         The absolute url.
         * @param timeout_ms  Timeout in milliseconds (set at default 5000ms) for each transfer.
         * @param headers     Optional headers (e.g {"Authorization", "Bearer <token>"}).
         * @param user_agent  Optional User-Agent, if set to std::nullopt, the library default is used .
         * @return Response with fields: ok, body, code, err
         */
        static Response get(const std::string& url,
                            std::int32_t timeout_ms = 5000,
                            const cpr::Header& headers = {},
                            const std::optional<std::string>& user_agent = std::nullopt);

        /**
         * @brief Blocking HTTP Post. Returns {ok, body, code, err}.
         *
         * ok is true if and only if no transport error occurred and the HTTP status is 2xx.
         *
         * @param url         The absolute url.
         * @param payload  The form-encoded key–value data to send in the request body.
         * @param timeout_ms  Timeout in milliseconds (set at default 5000ms) for each transfer.
         * @param headers     Optional headers (e.g {"Authorization", "Bearer <token>"}).
         * @param user_agent  Optional User-Agent, if set to std::nullopt, the library default is used .
         * @return Response with fields: ok, body, code, err
         */
        static Response post(const std::string& url,
                             const cpr::Payload& payload,
                             const cpr::Header& headers = {},
                             const std::optional<std::string>& user_agent = std::nullopt,
                             std::int32_t timeout_ms = 5000L);

    };
}
}
