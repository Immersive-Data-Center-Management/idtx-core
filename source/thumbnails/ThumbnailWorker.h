/**
 * @file ThumbnailWorker.h
 * @brief Simple background queue that runs @c ThumbnailGenerator jobs off the
 *        request thread.
 *
 * Even the placeholder generator opens the USD file via @c SdfLayer::FindOrOpen
 * and writes an image to disk. Doing that inline in the HTTP handler would
 * stall uploads with visible latency (worse when we later swap in a real
 * imaging generator). This worker owns a single background thread and a
 * lock-guarded FIFO. On shutdown, in-flight jobs are allowed to finish; the
 * queue itself is drained.
 *
 * The worker is intentionally single-threaded so that concurrent thumbnails
 * for the same file don't race on the output path. If throughput becomes a
 * concern, this can be trivially upgraded to a small pool because each job
 * writes to a distinct output path derived from the USD filename.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <idtx/utils/Logger.h>

#include "thumbnails/ThumbnailGenerator.h"

namespace idtx
{
namespace thumbnails
{

class ThumbnailWorker
{
    IDTX_LOG_CATEGORY("ThumbnailWorker")

public:
    /**
     * @brief Construct a worker that dispatches jobs to @p generator.
     *
     * The worker owns the generator (shared ownership) so callers can safely
     * hand it in and let the worker outlive their scope.
     */
    explicit ThumbnailWorker(std::shared_ptr<ThumbnailGenerator> generator);
    ~ThumbnailWorker();

    ThumbnailWorker(const ThumbnailWorker&)            = delete;
    ThumbnailWorker& operator=(const ThumbnailWorker&) = delete;

    /**
     * @brief File extension produced by the underlying generator (with dot).
     */
    const char* Extension() const noexcept { return m_generator_->Extension(); }

    /**
     * @brief Schedule a thumbnail-generation job.
     *
     * Returns immediately. Failures are logged; there is no delivery
     * confirmation because the HTTP response has already been sent.
     */
    void Submit(std::filesystem::path usd_file, std::filesystem::path out_path);

    /**
     * @brief Approximate number of currently-queued jobs (excluding the one
     *        being processed). Useful for tests and health probes.
     */
    std::size_t PendingCount() const;

private:
    struct Job
    {
        std::filesystem::path usd_file;
        std::filesystem::path out_path;
    };

    void Run();

    std::shared_ptr<ThumbnailGenerator> m_generator_;

    mutable std::mutex           m_mutex_;
    std::condition_variable      m_cv_;
    std::deque<Job>              m_queue_;
    std::atomic<bool>            m_stop_{false};
    std::thread                  m_thread_;
};

} // namespace thumbnails
} // namespace idtx