/**
 * @file RateLimiter.h
 * @brief In-memory, thread-safe sliding-window rate limiter with lockout.
 *
 * Provides the enforcement primitives used by @c RateLimitHandler to protect
 * the public endpoints against volumetric abuse and credential stuffing,
 * without requiring any external/shared cache. All state lives in-process,
 * which is the correct scope for a per-replica "safety net" behind a
 * Kubernetes ingress / cloud load balancer that already provides the primary,
 * cluster-wide throttling.
 *
 * Two independent facilities are offered:
 *   - @c SlidingWindowLimiter — a fixed-quota-per-window counter keyed by an
 *     arbitrary string (typically the client IP, optionally namespaced per
 *     route). Used for general request-rate limiting.
 *   - @c LoginThrottler — tracks consecutive failed login attempts per key and
 *     imposes a temporary lockout once a threshold is exceeded. Used to blunt
 *     credential-stuffing / brute-force attempts against @c /auth/login.
 *
 * Both structures self-prune expired entries opportunistically to bound memory
 * under sustained attack (an attacker rotating through many source IPs cannot
 * grow the maps without bound because stale buckets are evicted on access and
 * during periodic sweeps).
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace idtx
{
namespace middleware
{
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /**
     * @brief Fixed-window request counter keyed by an arbitrary string.
     *
     * Each key (e.g. a client IP) accumulates a request count within the
     * current window. When the window elapses the count resets. Exceeding
     * @c maxRequests within a window causes @c Allow to return @c false until
     * the window rolls over.
     *
     * Thread-safe: all access is guarded by an internal mutex.
     */
    class SlidingWindowLimiter
    {
    public:
        /**
         * @brief Construct a limiter with a request quota per time window.
         *
         * @param maxRequests Maximum allowed requests per window per key.
         * @param window      Length of the fixed window.
         */
        SlidingWindowLimiter(std::uint64_t maxRequests, std::chrono::seconds window)
            : m_maxRequests_(maxRequests)
            , m_window_(window)
        {}

        /**
         * @brief Result of an @c Allow() decision.
         */
        struct Decision
        {
            bool          allowed;     ///< Whether the request may proceed.
            std::uint64_t observed;    ///< Request count in the current window (after this call).
            std::uint64_t limit;       ///< The configured per-window limit.
            std::chrono::seconds retryAfter; ///< Suggested Retry-After when blocked.
        };

        /**
         * @brief Account for one request from @p key and decide admission.
         *
         * Increments the key's counter for the current window and returns
         * whether it stays within quota. Expired windows are reset lazily.
         *
         * @param key Identity to rate-limit on (e.g. client IP, or IP+route).
         * @param now Current time (injectable for testing).
         * @return A @c Decision describing admission and observed counters.
         */
        Decision Allow(const std::string& key, TimePoint now = Clock::now())
        {
            std::lock_guard<std::mutex> lk(m_mutex_);
            MaybeSweep(now);

            auto& b = m_buckets_[key];
            if (now - b.windowStart >= m_window_)
            {
                b.windowStart = now;
                b.count       = 0;
            }
            ++b.count;

            const bool allowed = b.count <= m_maxRequests_;
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - b.windowStart);
            const auto retry   = allowed ? std::chrono::seconds(0)
                                         : (m_window_ - elapsed);
            return Decision{allowed, b.count, m_maxRequests_,
                            retry.count() > 0 ? retry : std::chrono::seconds(1)};
        }

    private:
        struct Bucket
        {
            TimePoint     windowStart{};
            std::uint64_t count{0};
        };

        /**
         * @brief Evict buckets whose window is well past expiry.
         *
         * Runs at most once per window to keep the map bounded under an
         * IP-rotation attack. Called with @c m_mutex_ held.
         */
        void MaybeSweep(TimePoint now)
        {
            if (now - m_lastSweep_ < m_window_) return;
            m_lastSweep_ = now;
            for (auto it = m_buckets_.begin(); it != m_buckets_.end();)
            {
                if (now - it->second.windowStart >= 2 * m_window_)
                    it = m_buckets_.erase(it);
                else
                    ++it;
            }
        }

        std::mutex          m_mutex_;
        std::uint64_t       m_maxRequests_;
        std::chrono::seconds m_window_;
        TimePoint           m_lastSweep_{Clock::now()};
        std::unordered_map<std::string, Bucket> m_buckets_;
    };

    /**
     * @brief Tracks failed login attempts and enforces temporary lockouts.
     *
     * A key (client IP and/or username) accrues consecutive failures. Once the
     * failure count reaches @c maxFailures within @c failureWindow, the key is
     * locked out for @c lockoutDuration. A successful login clears the counter.
     *
     * Thread-safe: all access is guarded by an internal mutex.
     */
    class LoginThrottler
    {
    public:
        /**
         * @brief Construct a login throttler.
         *
         * @param maxFailures     Consecutive failures tolerated before lockout.
         * @param failureWindow   Window within which failures accumulate.
         * @param lockoutDuration How long a key stays locked out once tripped.
         */
        LoginThrottler(std::uint64_t maxFailures,
                       std::chrono::seconds failureWindow,
                       std::chrono::seconds lockoutDuration)
            : m_maxFailures_(maxFailures)
            , m_failureWindow_(failureWindow)
            , m_lockout_(lockoutDuration)
        {}

        /**
         * @brief Whether @p key is currently locked out.
         *
         * @param key Identity to check (IP, username, or a composite).
         * @param now Current time (injectable for testing).
         * @return @c retryAfter > 0 seconds when locked out; 0 otherwise.
         */
        std::chrono::seconds LockedFor(const std::string& key, TimePoint now = Clock::now())
        {
            std::lock_guard<std::mutex> lk(m_mutex_);
            auto it = m_entries_.find(key);
            if (it == m_entries_.end()) return std::chrono::seconds(0);
            if (it->second.lockedUntil > now)
                return std::chrono::duration_cast<std::chrono::seconds>(it->second.lockedUntil - now);
            return std::chrono::seconds(0);
        }

        /**
         * @brief Record a failed login and return whether it triggers lockout.
         *
         * @param key Identity that failed authentication.
         * @param now Current time (injectable for testing).
         * @return @c true if this failure caused the key to become locked out.
         */
        bool RecordFailure(const std::string& key, TimePoint now = Clock::now())
        {
            std::lock_guard<std::mutex> lk(m_mutex_);
            MaybeSweep(now);

            auto& e = m_entries_[key];
            // Reset the failure streak if the window since first failure elapsed.
            if (e.failures == 0 || now - e.firstFailure >= m_failureWindow_)
            {
                e.firstFailure = now;
                e.failures     = 0;
            }
            ++e.failures;

            if (e.failures >= m_maxFailures_)
            {
                e.lockedUntil = now + m_lockout_;
                e.failures    = 0; // start a fresh streak after lockout
                return true;
            }
            return false;
        }

        /**
         * @brief Clear any failure/lockout state for @p key after success.
         *
         * @param key Identity that authenticated successfully.
         */
        void RecordSuccess(const std::string& key)
        {
            std::lock_guard<std::mutex> lk(m_mutex_);
            m_entries_.erase(key);
        }

    private:
        struct Entry
        {
            TimePoint     firstFailure{};
            std::uint64_t failures{0};
            TimePoint     lockedUntil{};
        };

        /**
         * @brief Evict entries that are neither locked nor within a streak.
         *
         * Called with @c m_mutex_ held; runs at most once per failure window.
         */
        void MaybeSweep(TimePoint now)
        {
            if (now - m_lastSweep_ < m_failureWindow_) return;
            m_lastSweep_ = now;
            for (auto it = m_entries_.begin(); it != m_entries_.end();)
            {
                const bool locked  = it->second.lockedUntil > now;
                const bool active  = it->second.failures > 0 &&
                                     now - it->second.firstFailure < m_failureWindow_;
                if (!locked && !active)
                    it = m_entries_.erase(it);
                else
                    ++it;
            }
        }

        std::mutex           m_mutex_;
        std::uint64_t        m_maxFailures_;
        std::chrono::seconds m_failureWindow_;
        std::chrono::seconds m_lockout_;
        TimePoint            m_lastSweep_{Clock::now()};
        std::unordered_map<std::string, Entry> m_entries_;
    };
}
}