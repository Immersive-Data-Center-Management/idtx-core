#include "SessionCommandScheduler.h"

#include <thread>
#include <utility>

#include "concurrency/WorkerExecutor.h"

namespace idtx::session
{

std::shared_ptr<SessionCommandScheduler> SessionCommandScheduler::Create(
    std::weak_ptr<idtx::concurrency::WorkerExecutor> executor,
    CommandHandler                                   command_handler)
{
    return std::make_shared<SessionCommandScheduler>(
        PrivateTag{}, std::move(executor), std::move(command_handler));
}

SessionCommandScheduler::SessionCommandScheduler(
    PrivateTag,
    std::weak_ptr<idtx::concurrency::WorkerExecutor> executor,
    CommandHandler                                   command_handler)
    : executor_(std::move(executor))
    , command_handler_(std::move(command_handler))
{
}

CommandAdmissionStatus SessionCommandScheduler::TrySubmit(SessionCommand command)
{
    return Submit(mailbox_, std::move(command));
}

CommandAdmissionStatus SessionCommandScheduler::TrySubmitPriority(SessionCommand command)
{
    return Submit(priority_mailbox_, std::move(command));
}

template<std::size_t Capacity>
CommandAdmissionStatus SessionCommandScheduler::Submit(
    BoundedMpscMailbox<SessionCommand, Capacity>& mailbox,
    SessionCommand&&                              command)
{
    // Announce the submission before checking admission. Together with the
    // sequentially consistent operations in CloseAdmission() and WaitIdle()
    // this guarantees that either this submission observes the closed gate,
    // or WaitIdle() observes the pending count and waits for it.
    pending_count_.fetch_add(1);

    if (!accepting_.load())
    {
        ReleasePending();
        return CommandAdmissionStatus::Stopping;
    }

    if (!mailbox.TryPush(std::move(command)))
    {
        ReleasePending();
        return CommandAdmissionStatus::QueueFull;
    }

    ScheduleDrainIfRequired();
    return CommandAdmissionStatus::Accepted;
}

void SessionCommandScheduler::CloseAdmission() noexcept
{
    accepting_.store(false);
}

void SessionCommandScheduler::WaitIdle() const noexcept
{
    for (;;)
    {
        const std::size_t pending = pending_count_.load();
        if (pending == 0)
        {
            break;
        }
        pending_count_.wait(pending);
    }

    // The last command has been processed; wait for the drain that processed
    // it to leave the handler loop.
    while (drain_scheduled_.load())
    {
        drain_scheduled_.wait(true);
    }
}

bool SessionCommandScheduler::IsAccepting() const noexcept
{
    return accepting_.load(std::memory_order_relaxed);
}

std::size_t SessionCommandScheduler::ApproximatePendingCount() const noexcept
{
    return pending_count_.load(std::memory_order_relaxed);
}

void SessionCommandScheduler::ScheduleDrainIfRequired()
{
    // A drain that is already scheduled or running re-checks the pending
    // count before it finishes, so a submission that loses this exchange is
    // still picked up.
    while (!drain_scheduled_.exchange(true))
    {
        if (auto executor = executor_.lock())
        {
            if (executor->Post([self = shared_from_this()] { self->RunDrainTask(); }))
            {
                return;
            }
        }

        // No executor available (shutdown in progress). Process the queue on
        // the calling thread so accepted commands are never stranded.
        if (!DrainBatch())
        {
            return;
        }
    }
}

void SessionCommandScheduler::RunDrainTask() noexcept
{
    if (DrainBatch())
    {
        ScheduleDrainIfRequired();
    }
}

bool SessionCommandScheduler::DrainBatch() noexcept
{
    std::size_t processed = 0;
    std::size_t retries   = 0;

    while (processed < kMaxCommandsPerDrain)
    {
        std::optional<SessionCommand> command = priority_mailbox_.TryPop();
        if (!command) command = mailbox_.TryPop();
        if (!command)
        {
            if (pending_count_.load() == 0)
            {
                break;
            }

            // A producer has announced a submission but not yet published it
            // (or is about to be rejected). Give it a moment; if it is still
            // not visible, end this batch and let the caller reschedule
            // instead of blocking the worker.
            if (++retries > kHeadPublicationRetries)
            {
                break;
            }
            std::this_thread::yield();
            continue;
        }

        retries = 0;
        try
        {
            command_handler_(std::move(*command));
        }
        catch (...)
        {
            // The handler is responsible for logging. One failing command
            // must not stop the session from processing the next one.
        }
        command.reset();

        ReleasePending();
        ++processed;
    }

    drain_scheduled_.store(false);
    drain_scheduled_.notify_all();

    // A producer that pushed while this batch was running saw
    // drain_scheduled_ == true and did not schedule. Its pending increment
    // happened before that, so it is visible here.
    return pending_count_.load() > 0;
}

void SessionCommandScheduler::ReleasePending() noexcept
{
    if (pending_count_.fetch_sub(1) == 1)
    {
        pending_count_.notify_all();
    }
}

} // namespace idtx::session
