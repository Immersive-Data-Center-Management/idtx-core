/**
 * @file SecurityAuditLog.h
 * @brief Centralised audit logging for suspicious security events.
 *
 * Provides a single, structured entry point for recording security-relevant
 * events (rate-limit breaches, login failures, credential-stuffing attempts,
 * oversized request bodies, ...). Every event is emitted as a single-line
 * JSON object through the existing @c IDTX_LOG pipeline at WARN level so that
 * log-aggregation tooling (Loki, ELK, Cloud Logging, ...) can index and alert
 * on the @c audit marker and the @c event field.
 *
 * The audit log intentionally records only non-sensitive metadata. It never
 * logs passwords, bearer tokens or full request bodies. Usernames are recorded
 * because they are essential for detecting credential-stuffing and account
 * lockout, but callers may choose to omit or hash them where policy requires.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <idtx/utils/Logger.h>

namespace idtx
{
namespace security
{
    /**
     * @brief Enumerates the categories of suspicious security events.
     *
     * The string form (see @c to_string) is what lands in the structured log
     * under the @c event key and is what dashboards / alerts should match on.
     */
    enum class AuditEvent
    {
        RateLimitExceeded,      ///< A per-IP request rate limit was reached.
        LoginRateLimitExceeded, ///< Too many login attempts from one source.
        LoginFailed,            ///< An individual login attempt was rejected.
        LoginLockout,           ///< A source/account was locked out after repeated failures.
        BodyTooLarge,           ///< A request body exceeded the configured cap.
        ConcurrencyLimited,     ///< A concurrency/backpressure limit was hit.
        Unauthorized,           ///< A protected route was accessed without valid auth.
    };

    /**
     * @brief Convert an @c AuditEvent to its stable, machine-readable string.
     */
    inline std::string_view to_string(AuditEvent e) noexcept
    {
        switch (e)
        {
        case AuditEvent::RateLimitExceeded:      return "rate_limit_exceeded";
        case AuditEvent::LoginRateLimitExceeded: return "login_rate_limit_exceeded";
        case AuditEvent::LoginFailed:            return "login_failed";
        case AuditEvent::LoginLockout:           return "login_lockout";
        case AuditEvent::BodyTooLarge:           return "body_too_large";
        case AuditEvent::ConcurrencyLimited:     return "concurrency_limited";
        case AuditEvent::Unauthorized:           return "unauthorized";
        default:                                 return "unknown";
        }
    }

    /**
     * @brief Structured metadata describing a single suspicious event.
     *
     * All fields are optional except @c event: populate whatever context is
     * available at the call site. Sensitive values (passwords, tokens) must
     * never be placed here.
     */
    struct AuditRecord
    {
        AuditEvent  event;                       ///< The event category.
        std::string clientIp;                    ///< Best-effort remote client IP.
        std::string method;                      ///< HTTP method, e.g. "POST".
        std::string path;                        ///< Request path, e.g. "/api/v1/auth/login".
        std::string userAgent;                   ///< User-Agent header (may be empty).
        std::optional<std::string> username;     ///< Login subject, when relevant.
        std::optional<int>         statusCode;   ///< HTTP status returned to client.
        std::optional<std::string> detail;       ///< Short human-readable reason.
        std::optional<std::uint64_t> limit;      ///< Configured limit that was breached.
        std::optional<std::uint64_t> observed;   ///< Observed value (count / bytes).
    };

    /**
     * @brief Emits structured security audit events into the logging pipeline.
     *
     * Stateless and thread-safe (delegates to the global @c Log). Intended to
     * be called from middleware and controllers whenever a suspicious event
     * occurs. Output is a single JSON line prefixed with an @c audit=1 marker
     * to make filtering trivial in downstream log tooling.
     */
    class SecurityAuditLog
    {
        IDTX_LOG_CATEGORY("SecurityAudit");

    public:
        /**
         * @brief Record a suspicious security event.
         *
         * Serialises @p record to a compact JSON object and logs it at WARN
         * level. Missing optional fields are omitted from the JSON so the
         * output stays small and queryable.
         *
         * @param record The populated audit metadata to emit.
         */
        static void Record(const AuditRecord& record)
        {
            nlohmann::json j;
            j["audit"]  = 1;
            j["event"]  = to_string(record.event);

            if (!record.clientIp.empty())  j["client_ip"]  = record.clientIp;
            if (!record.method.empty())    j["method"]     = record.method;
            if (!record.path.empty())      j["path"]       = record.path;
            if (!record.userAgent.empty()) j["user_agent"] = record.userAgent;
            if (record.username)           j["username"]   = *record.username;
            if (record.statusCode)         j["status"]     = *record.statusCode;
            if (record.detail)             j["detail"]     = *record.detail;
            if (record.limit)              j["limit"]      = *record.limit;
            if (record.observed)           j["observed"]   = *record.observed;

            // Single-line JSON keeps each event on one log record.
            IDTX_LOG(IDTX_WARN, "{}", j.dump());
        }
    };
}
}