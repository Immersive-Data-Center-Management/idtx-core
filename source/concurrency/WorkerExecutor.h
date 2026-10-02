/**
 * @file WorkerExecutor.h
 * @brief Small fixed-size thread pool shared by all sessions.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace idtx::concurrency
{

/**
 * @brief Small shared worker executor.
 *
 * Session drain tasks are submitted to this executor. The executor owns a
 * fixed number of worker threads, independent of the number of sessions.
 *
 * The task queue is guarded by a mutex. This is intentional: callers post one
 * task per scheduled session drain, not one task per message, so the lock is
 * not on the per-message hot path.
 *
 * Post() is thread-safe.
 *
 * Stop() rejects new tasks, completes all previously accepted tasks, and
 * joins all worker threads.
 */
class WorkerExecutor final
{
public:
    using Task = std::function<void()>;

    /**
     * @brief Start @p worker_count worker threads.
     * @throws std::invalid_argument if @p worker_count is zero.
     */
    explicit WorkerExecutor(std::size_t worker_count);

    ~WorkerExecutor();

    WorkerExecutor(const WorkerExecutor&) = delete;
    WorkerExecutor& operator=(const WorkerExecutor&) = delete;

    WorkerExecutor(WorkerExecutor&&) = delete;
    WorkerExecutor& operator=(WorkerExecutor&&) = delete;

    /**
     * @brief Submit a task for asynchronous execution.
     *
     * @return true if the task was accepted, false if the executor is
     *         stopping or @p task is empty.
     */
    [[nodiscard]] bool Post(Task task);

    /**
     * @brief Stop accepting tasks, finish accepted tasks, and join workers.
     *
     * Safe to call more than once. Must not be called from one of the
     * executor's own worker threads (a worker cannot join itself), which also
     * means a task must never hold the last owning reference to the executor.
     */
    void Stop() noexcept;

    [[nodiscard]]
    std::size_t WorkerCount() const noexcept;

private:
    void WorkerLoop() noexcept;

    mutable std::mutex       mutex_;
    std::condition_variable  work_available_;
    std::deque<Task>         tasks_;
    std::vector<std::thread> workers_;
    std::size_t              worker_count_{0};

    bool stopping_{false};
};

} // namespace idtx::concurrency
