/**
 * @file SharedLayerLock.h
 * @brief Process-wide reader/writer lock for USD layers that may be shared.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace idtx::concurrency
{

/**
 * @brief Excludes changes to the layer registry's contents while a shared
 *        layer is reloaded.
 *
 * The SdfLayer registry hands every opener of a path the same layer object:
 * sessions on the same file, sessions on files that sublayer, reference or
 * payload it, the thumbnail generator and REST handlers. Sdf sends change
 * notices on the thread that changes a layer, and TfNotice invokes listeners
 * synchronously on that thread, so every stage composing the layer
 * recomposes and runs its listeners there.
 *
 * Exclusive: held by the thread that reloads a shared layer, and never by an
 * executor worker.
 *
 * Shared: held by everything that opens layers or starts or stops using one
 * outside the sessions that a reload pauses: stage creation, REST reads,
 * thumbnails, and session commands that add or drop layers. Session
 * commands that only work on the layers their stage already uses do not take
 * it; a reload pauses those sessions instead.
 *
 * Threads that may block (REST, thumbnails, teardown) use lock_shared().
 * Executor workers must never wait on it, so they use
 * try_lock_shared_or_notify(): it fails while a reload is running or
 * waiting, and the callback runs once the last such reload has released the
 * lock.
 *
 * Writers are preferred, so neither readers nor deferred commands can starve
 * a reload. Shared acquisition is reentrant per thread, also while the same
 * thread holds the lock exclusively. Requesting the lock exclusively while
 * already holding it throws, because an upgrade would deadlock.
 *
 * Satisfies SharedLockable and Lockable, so use std::shared_lock and
 * std::unique_lock.
 */
class SharedLayerLock final
{
public:
    SharedLayerLock() = default;

    SharedLayerLock(const SharedLayerLock&)            = delete;
    SharedLayerLock& operator=(const SharedLayerLock&) = delete;

    void lock_shared()
    {
        if (t_depth_ > 0)
        {
            ++t_depth_;
            return;
        }

        // Pass through the turnstile so a waiting writer blocks new readers.
        {
            std::lock_guard turnstile(m_turnstile_);
        }
        m_rw_.lock_shared();
        t_depth_ = 1;
    }

    /**
     * @brief Take the lock shared without waiting, or arrange to be told when
     *        it is worth trying again.
     *
     * Returns true with the lock held shared. Returns false if a writer holds
     * or waits for the lock; @p on_released is then invoked, on the writer's
     * thread, after the last pending writer has released it. Never blocks.
     */
    [[nodiscard]]
    bool try_lock_shared_or_notify(std::function<void()> on_released)
    {
        if (t_depth_ > 0)
        {
            ++t_depth_;
            return true;
        }

        for (;;)
        {
            {
                std::lock_guard waiters(m_waiters_mutex_);
                if (m_writers_ > 0)
                {
                    m_waiters_.push_back(std::move(on_released));
                    return false;
                }
            }

            if (m_rw_.try_lock_shared())
            {
                t_depth_ = 1;
                return true;
            }

            // A writer acquired the lock after the check above; the next
            // check sees it. try_lock_shared() may also fail spuriously.
            std::this_thread::yield();
        }
    }

    void unlock_shared()
    {
        if (--t_depth_ > 0) return;
        m_rw_.unlock_shared();
    }

    void lock()
    {
        if (t_depth_ != 0)
        {
            throw std::logic_error(
                "SharedLayerLock: exclusive request from a thread that already holds the lock");
        }

        // Counted before waiting, so try_lock_shared_or_notify() defers from
        // now on rather than competing with this writer.
        {
            std::lock_guard waiters(m_waiters_mutex_);
            ++m_writers_;
        }

        // Holding the turnstile until the exclusive lock is acquired stops
        // blocking readers that arrive in the meantime.
        std::lock_guard turnstile(m_turnstile_);
        m_rw_.lock();
        t_depth_     = 1;
        t_exclusive_ = true;
    }

    void unlock()
    {
        t_depth_     = 0;
        t_exclusive_ = false;
        m_rw_.unlock();

        std::vector<std::function<void()>> released;
        {
            std::lock_guard waiters(m_waiters_mutex_);
            if (--m_writers_ == 0) released.swap(m_waiters_);
        }
        for (auto& callback : released)
        {
            if (callback) callback();
        }
    }

    /// True if the calling thread holds the lock exclusively.
    [[nodiscard]]
    static bool HeldExclusivelyByThisThread() noexcept
    {
        return t_exclusive_;
    }

private:
    std::mutex        m_turnstile_;
    std::shared_mutex m_rw_;

    // Writers holding or waiting for the lock, and the callbacks of
    // try_lock_shared_or_notify() calls that found one. Both under
    // m_waiters_mutex_, so a callback registered while a writer is counted is
    // always collected by that writer's unlock().
    std::mutex                         m_waiters_mutex_;
    std::size_t                        m_writers_ = 0;
    std::vector<std::function<void()>> m_waiters_;

    // Per thread, not per instance: there is one instance, see SharedLayers().
    static inline thread_local std::size_t t_depth_     = 0;
    static inline thread_local bool        t_exclusive_ = false;
};

/// The single lock guarding all layers in the process-wide SdfLayer registry.
[[nodiscard]]
inline SharedLayerLock& SharedLayers() noexcept
{
    static SharedLayerLock lock;
    return lock;
}

} // namespace idtx::concurrency
