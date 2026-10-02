/**
 * @file SessionFlusher.h
 * @brief Background worker that periodically persists dirty session layers to
 *        their on-disk sidecar files.
 *
 * Client edits are authored into each session's session layer (a named sidecar
 * file) but only held in memory until saved. Saving on every edit would stall
 * the authoring path with disk I/O; instead this worker wakes on a fixed
 * interval and calls SessionManager::SubmitFlushDirtySessionsCommand(), which queues a
 * flush command for every session flagged dirty. Each session's consumer then
 * saves its sidecar in order with its other commands. The worst-case data-loss
 * window on a crash is therefore about one flush interval.
 *
 * Owns a single background thread with the same start/stop handshake as
 * ThumbnailWorker. Construct after the SessionManager and let it be destroyed
 * before the manager so the final flush observes live sessions.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <idtx/utils/Logger.h>

namespace idtx
{
namespace session
{

class SessionManager;

class SessionFlusher
{
    IDTX_LOG_CATEGORY("SessionFlusher")

public:
    /**
     * @brief Start a flusher that drives @p manager on @p interval.
     * @param manager  Session manager to flush. Must outlive the flusher.
     * @param interval How often to flush dirty sessions (default 2 s).
     */
    explicit SessionFlusher(std::shared_ptr<SessionManager> manager,
                            std::chrono::milliseconds interval = std::chrono::milliseconds{2000});
    ~SessionFlusher();

    SessionFlusher(const SessionFlusher&)            = delete;
    SessionFlusher& operator=(const SessionFlusher&) = delete;

private:
    void Run();

    std::shared_ptr<SessionManager> m_manager_;
    std::chrono::milliseconds       m_interval_;
    std::atomic<bool>               m_stop_{false};
    std::mutex                      m_mutex_;
    std::condition_variable         m_cv_;
    std::thread                     m_thread_;
};

} // namespace session
} // namespace idtx
