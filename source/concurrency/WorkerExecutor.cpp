#include "WorkerExecutor.h"

#include <stdexcept>
#include <utility>

namespace idtx::concurrency
{

WorkerExecutor::WorkerExecutor(const std::size_t worker_count)
    : worker_count_(worker_count)
{
    if (worker_count == 0)
    {
        throw std::invalid_argument(
            "WorkerExecutor requires at least one worker.");
    }

    workers_.reserve(worker_count);

    try
    {
        for (std::size_t index = 0; index < worker_count; ++index)
        {
            workers_.emplace_back(
                [this]
                {
                    WorkerLoop();
                });
        }
    }
    catch (...)
    {
        Stop();
        throw;
    }
}

WorkerExecutor::~WorkerExecutor()
{
    Stop();
}

bool WorkerExecutor::Post(Task task)
{
    if (!task)
    {
        return false;
    }

    {
        std::lock_guard lock(mutex_);

        if (stopping_)
        {
            return false;
        }

        tasks_.emplace_back(std::move(task));
    }

    work_available_.notify_one();
    return true;
}

void WorkerExecutor::Stop() noexcept
{
    {
        std::lock_guard lock(mutex_);

        if (stopping_)
        {
            return;
        }

        stopping_ = true;
    }

    work_available_.notify_all();

    for (std::thread& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

std::size_t WorkerExecutor::WorkerCount() const noexcept
{
    return worker_count_;
}

void WorkerExecutor::WorkerLoop() noexcept
{
    for (;;)
    {
        Task task;

        {
            std::unique_lock lock(mutex_);

            work_available_.wait(
                lock,
                [this]
                {
                    return stopping_ || !tasks_.empty();
                });

            if (tasks_.empty())
            {
                // Only reached when stopping_ is set and all accepted tasks
                // have been handed out.
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        try
        {
            task();
        }
        catch (...)
        {
            // Tasks are expected to do their own logging and error isolation.
            // An escaping exception must not terminate the worker thread or
            // the process.
        }
    }
}

} // namespace idtx::concurrency
