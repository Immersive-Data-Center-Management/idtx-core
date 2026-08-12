/**
 * @file RateLimitHandler.h
 * @brief Crow middleware providing an in-process DDoS/abuse safety net.
 *
 * This handler is the application-level "safety net" that complements (does not
 * replace) the primary throttling expected at the Kubernetes ingress / cloud
 * load balancer. It runs before the route handler and enforces, per replica:
 *
 *   1. Request-body size caps — rejects oversized bodies with 413 before any
 *      expensive work (e.g. the outbound IdP call on login) is performed.
 *   2. Per-IP request rate limiting — a generous global bucket plus a stricter,
 *      dedicated bucket for @c POST /api/v1/auth/login.
 *   3. Login lockout hints — if a source is currently locked out due to
 *      repeated login failures, the login request is short-circuited with 429
 *      before it reaches the controller.
 *
 * Every enforcement action emits a structured @c SecurityAuditLog event with
 * request metadata (client IP, method, path, user-agent, observed vs limit).
 *
 * The client IP is resolved in a proxy-aware way: because the service sits
 * behind an ingress/LB, the left-most address in @c X-Forwarded-For is used
 * when present, falling back to the peer address Crow reports. Thresholds are
 * configurable from the environment via @c configure().
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <crow.h>

#include "middleware/RateLimiter.h"
#include "utils/SecurityAuditLog.h"

#include <idtx/utils/Logger.h>

namespace idtx
{
namespace middleware
{
    /**
     * @brief Crow middleware enforcing rate limits and body-size caps.
     *
     * Installed in the Crow @c App middleware chain ahead of the JWT handler so
     * that abusive traffic is shed before authentication and route dispatch.
     * The @c LoginThrottler is shared with @c AuthController (which reports
     * success/failure) via @c GetLoginThrottler so lockout decisions and the
     * failure accounting stay consistent.
     */
    struct RateLimitHandler
    {
        IDTX_LOG_CATEGORY("RateLimitHandler")

        /**
         * @brief Tunable thresholds for the middleware.
         *
         * Defaults are intentionally conservative for a per-replica safety net:
         * they should be well above expected legitimate traffic while still
         * capping pathological floods.
         */
        struct Config
        {
            /// Global per-IP request budget.
            std::uint64_t        globalMaxRequests  = 300;
            std::chrono::seconds globalWindow       = std::chrono::seconds(60);

            /// Stricter budget for the login endpoint (per IP).
            std::uint64_t        loginMaxRequests   = 10;
            std::chrono::seconds loginWindow        = std::chrono::seconds(60);

            /// Login failure lockout policy (credential-stuffing defense).
            std::uint64_t        loginMaxFailures   = 5;
            std::chrono::seconds loginFailureWindow = std::chrono::seconds(300);
            std::chrono::seconds loginLockout       = std::chrono::seconds(300);

            /// Maximum accepted request body size for the login endpoint (bytes).
            std::size_t          loginMaxBodyBytes  = 4 * 1024;   // 4 KiB
            /// Maximum accepted request body size for the upload endpoint (bytes).
            std::size_t          uploadMaxBodyBytes = 8 * 1024 * 1024 * 1024; // 8 GiB, as USD assets can get quite huge
            /// Maximum accepted request body size for any other endpoint (bytes).
            std::size_t          globalMaxBodyBytes = 8 * 1024 * 1024; // 8 MiB

            /// Trust X-Forwarded-For for client-IP resolution (true behind LB/ingress).
            bool                 trustForwardedFor  = true;
        };

        /**
         * @brief Per-request context populated by @c before_handle.
         *
         * Opaque to Crow's dispatcher; carried so that later stages (or tests)
         * can inspect the throttling decision if needed.
         */
        struct context
        {
            bool        blocked = false;
            std::string clientIp;
        };

        RateLimitHandler() = default;
        ~RateLimitHandler() = default;

        /**
         * @brief Configure thresholds and build the limiter instances.
         *
         * Must be called once during application initialisation before the
         * server starts. Safe to call again to reconfigure before @c Run().
         *
         * @param cfg The threshold configuration (typically built from env).
         */
        void configure(const Config& cfg)
        {
            m_config_ = cfg;
            m_globalLimiter_ = std::make_shared<SlidingWindowLimiter>(
                cfg.globalMaxRequests, cfg.globalWindow);
            m_loginLimiter_ = std::make_shared<SlidingWindowLimiter>(
                cfg.loginMaxRequests, cfg.loginWindow);
            m_loginThrottler_ = std::make_shared<LoginThrottler>(
                cfg.loginMaxFailures, cfg.loginFailureWindow, cfg.loginLockout);
            m_enabled_ = true;

            IDTX_LOG(IDTX_INFO,
                     "RateLimitHandler enabled (global {}/{}s, login {}/{}s, "
                     "lockout {} after {} failures/{}s, body cap login={}B upload={}B global={}B)",
                     cfg.globalMaxRequests, cfg.globalWindow.count(),
                     cfg.loginMaxRequests, cfg.loginWindow.count(),
                     cfg.loginLockout.count(), cfg.loginMaxFailures,
                     cfg.loginFailureWindow.count(),
                     cfg.loginMaxBodyBytes, cfg.uploadMaxBodyBytes, cfg.globalMaxBodyBytes);
        }

        /**
         * @brief Shared login throttler, so the AuthController can report
         *        success/failure outcomes into the same accounting.
         *
         * @return Shared pointer to the throttler, or nullptr if not configured.
         */
        std::shared_ptr<LoginThrottler> GetLoginThrottler() const { return m_loginThrottler_; }

        /**
         * @brief Enforce body-size caps, rate limits and login lockout.
         *
         * Runs before the route handler. On any violation it fills @p res with
         * an appropriate 4xx status (413 / 429), sets @c Retry-After where
         * applicable, ends the response and records a @c SecurityAuditLog event.
         *
         * @param req The incoming HTTP request.
         * @param res The response to short-circuit on violation.
         * @param ctx The per-request middleware context.
         */
        void before_handle(crow::request& req, crow::response& res, context& ctx)
        {
            if (!m_enabled_) return;
            // Let CORS preflight through untouched.
            if (req.method == crow::HTTPMethod::Options) return;

            const std::string ip     = ResolveClientIp(req);
            const std::string path   = NormalisePath(req.url);
            const std::string method = crow::method_name(req.method);
            const std::string ua     = req.get_header_value("User-Agent");
            ctx.clientIp = ip;

            const bool isLogin = (req.method == crow::HTTPMethod::Post) &&
                                 (path == c_loginPath_);

            const bool isUpload = (req.method == crow::HTTPMethod::Post) &&
                                  (path == c_uploadPath_);

            // 1) Body-size cap (cheap, do first to shed heavy payloads early).
            const std::size_t maxBody = isLogin ? m_config_.loginMaxBodyBytes 
                                            : isUpload ? m_config_.uploadMaxBodyBytes
                                                : m_config_.globalMaxBodyBytes;
            if (req.body.size() > maxBody)
            {
                security::SecurityAuditLog::Record({
                    security::AuditEvent::BodyTooLarge, ip, method, path, ua,
                    std::nullopt, 413, "request body exceeds cap",
                    static_cast<std::uint64_t>(maxBody),
                    static_cast<std::uint64_t>(req.body.size())});
                Reject(res, ctx, 413, "payload_too_large",
                       "Request body exceeds the permitted size.");
                return;
            }

            // 2) Login lockout: short-circuit sources already locked out.
            if (isLogin && m_loginThrottler_)
            {
                const auto lockedFor = m_loginThrottler_->LockedFor(ip);
                if (lockedFor.count() > 0)
                {
                    security::SecurityAuditLog::Record({
                        security::AuditEvent::LoginLockout, ip, method, path, ua,
                        std::nullopt, 429, "source temporarily locked out",
                        static_cast<std::uint64_t>(m_config_.loginMaxFailures),
                        std::nullopt});
                    res.set_header("Retry-After", std::to_string(lockedFor.count()));
                    Reject(res, ctx, 429, "too_many_attempts",
                           "Too many failed login attempts. Try again later.");
                    return;
                }
            }

            // 3) Rate limiting: stricter bucket for login, global otherwise.
            if (isLogin && m_loginLimiter_)
            {
                const auto d = m_loginLimiter_->Allow("login:" + ip);
                if (!d.allowed)
                {
                    security::SecurityAuditLog::Record({
                        security::AuditEvent::LoginRateLimitExceeded, ip, method,
                        path, ua, std::nullopt, 429, "login rate limit exceeded",
                        d.limit, d.observed});
                    res.set_header("Retry-After", std::to_string(d.retryAfter.count()));
                    Reject(res, ctx, 429, "rate_limited",
                           "Too many login attempts. Slow down.");
                    return;
                }
            }

            if (m_globalLimiter_)
            {
                const auto d = m_globalLimiter_->Allow("global:" + ip);
                if (!d.allowed)
                {
                    security::SecurityAuditLog::Record({
                        security::AuditEvent::RateLimitExceeded, ip, method, path,
                        ua, std::nullopt, 429, "request rate limit exceeded",
                        d.limit, d.observed});
                    res.set_header("Retry-After", std::to_string(d.retryAfter.count()));
                    Reject(res, ctx, 429, "rate_limited",
                           "Too many requests. Slow down.");
                    return;
                }
            }
        }

        /**
         * @brief No-op post-processing hook (required by the Crow contract).
         */
        void after_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {}

        /**
         * @brief Resolve the best-effort client IP for @p req.
         *
         * When @c trustForwardedFor is set, the left-most entry of
         * @c X-Forwarded-For is used (the original client as seen by the edge
         * proxy). Otherwise, or when the header is absent, Crow's reported
         * remote endpoint is used. Exposed as static so controllers can derive
         * the same key for their own audit records.
         *
         * @param req            The incoming request.
         * @param trustForwarded Whether to honour X-Forwarded-For.
         * @return A string suitable for use as a rate-limit / audit key.
         */
        static std::string ResolveClientIp(const crow::request& req, bool trustForwarded = true)
        {
            if (trustForwarded)
            {
                const std::string& xff = req.get_header_value("X-Forwarded-For");
                if (!xff.empty())
                {
                    // Left-most address is the original client.
                    const auto comma = xff.find(',');
                    std::string first = (comma == std::string::npos) ? xff : xff.substr(0, comma);
                    // Trim surrounding whitespace.
                    const auto b = first.find_first_not_of(" \t");
                    const auto e = first.find_last_not_of(" \t");
                    if (b != std::string::npos)
                        return first.substr(b, e - b + 1);
                }
            }
            return req.remote_ip_address.empty() ? std::string("unknown")
                                                 : req.remote_ip_address;
        }

    private:
        /**
         * @brief Strip a trailing query string from a URL path.
         *
         * Rate-limit and login classification are done on the path only, so
         * @c /api/v1/auth/login?foo=bar still matches the login route.
         *
         * @param url The raw request URL.
         * @return The path component without the query string.
         */
        static std::string NormalisePath(const std::string& url)
        {
            const auto q = url.find('?');
            return q == std::string::npos ? url : url.substr(0, q);
        }

        /**
         * @brief Fill @p res with a uniform JSON error and end the request.
         *
         * Mirrors the shape produced by @c idtx::dto::make_error so clients see
         * a consistent contract, but kept dependency-free here to avoid pulling
         * DTO headers into the middleware.
         *
         * @param res     Response to populate and end.
         * @param ctx     Middleware context (marked blocked).
         * @param code    HTTP status code (e.g. 413, 429).
         * @param error   Stable machine-readable error code.
         * @param message Human-readable explanation.
         */
        static void Reject(crow::response& res, context& ctx, int code,
                           const std::string& error, const std::string& message)
        {
            ctx.blocked = true;
            res.code = code;
            res.set_header("Content-Type", "application/json");
            res.body = std::string("{\"error\":\"") + error +
                       "\",\"message\":\"" + message + "\"}";
            res.end();
        }

        bool                                 m_enabled_ = false;
        Config                               m_config_{};
        std::shared_ptr<SlidingWindowLimiter> m_globalLimiter_;
        std::shared_ptr<SlidingWindowLimiter> m_loginLimiter_;
        std::shared_ptr<LoginThrottler>       m_loginThrottler_;

        static constexpr const char* c_loginPath_ = "/api/v1/auth/login";
        static constexpr const char* c_uploadPath_ = "/api/v1/upload";
    };
}
}
