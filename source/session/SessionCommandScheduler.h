/**
 * @file SessionCommandScheduler.h
 * @brief Per-session command queue with a single-consumer drain on a shared
 *        WorkerExecutor.
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "BoundedMpscMailbox.h"
#include "SessionCommand.h"

namespace idtx::concurrency
{
class WorkerExecutor;
}

namespace idtx::session
{

enum class CommandAdmissionStatus : std::uint8_t
{
    Accepted,
    QueueFull,
    Stopping,
};

/**
 * @brief Per-session command scheduling state.
 *
 * Any number of threads may submit commands through TrySubmit(). Enqueueing
 * is lock-free and never waits for the consumer: a full queue is reported as
 * QueueFull. Only the submission that finds the session idle schedules a
 * drain task, which takes the executor's lock briefly; submissions while a
 * drain is scheduled or running stay lock-free.
 *
 * Accepted commands are handed to the command handler one at a time, in the
 * order their mailbox positions were reserved. Commands submitted with
 * TrySubmitPriority() go into a small second mailbox that the consumer
 * empties before every normal command, so they overtake the normal queue. At most one drain task is
 * scheduled or running on the executor at any time, so the handler is the
 * single logical consumer of the session and needs no locking. A drain
 * processes at most kMaxCommandsPerDrain commands before yielding the worker
 * back to the executor, so busy sessions cannot starve others.
 *
 * Instances must be owned by a std::shared_ptr (use Create()). A queued drain
 * task keeps the scheduler alive until it has finished.
 *
 * The scheduler only holds a weak reference to the executor so that a drain
 * task can never become the last owner of the executor. If the executor is
 * gone or stopped when a drain has to be scheduled, the drain runs inline on
 * the submitting thread instead - only happens during shutdown.
 */
class SessionCommandScheduler final
    : public std::enable_shared_from_this<SessionCommandScheduler>
{
public:
    static constexpr std::size_t kMailboxCapacity         = 1024;
    static constexpr std::size_t kPriorityMailboxCapacity = 128;
    static constexpr std::size_t kMaxCommandsPerDrain     = 64;
    static constexpr std::size_t kHeadPublicationRetries  = 16;

    using CommandHandler = std::function<void(SessionCommand&&)>;

    /**
     * @param executor        Executor that runs drain tasks.
     * @param command_handler Called serially for every accepted command.
     *                        Exceptions are caught and dropped; the handler is
     *                        expected to log its own failures.
     */
    [[nodiscard]]
    static std::shared_ptr<SessionCommandScheduler> Create(
        std::weak_ptr<idtx::concurrency::WorkerExecutor> executor,
        CommandHandler                                   command_handler);

    ~SessionCommandScheduler() = default;

    SessionCommandScheduler(const SessionCommandScheduler&)            = delete;
    SessionCommandScheduler& operator=(const SessionCommandScheduler&) = delete;

    SessionCommandScheduler(SessionCommandScheduler&&)            = delete;
    SessionCommandScheduler& operator=(SessionCommandScheduler&&) = delete;

    /**
     * @brief Attempt to submit a command.
     *
     * On QueueFull or Stopping the command is not consumed and is destroyed
     * with the argument.
     */
    [[nodiscard]]
    CommandAdmissionStatus TrySubmit(SessionCommand command);

    /**
     * @brief Attempt to submit a command that runs before every normal command
     *        that has not started yet.
     *
     * Same admission rules as TrySubmit(); QueueFull refers to the priority
     * mailbox. Priority commands run in submission order among themselves.
     */
    [[nodiscard]]
    CommandAdmissionStatus TrySubmitPriority(SessionCommand command);

    /**
     * @brief Reject all further submissions.
     *
     * Already accepted commands remain queued and are processed normally.
     */
    void CloseAdmission() noexcept;

    /**
     * @brief Block until every accepted command has been processed and no
     *        drain is running.
     *
     * Called after CloseAdmission() before tearing down the state
     * the command handler touches. Must not be called from the command
     * handler or from any executor worker thread.
     */
    void WaitIdle() const noexcept;

    [[nodiscard]]
    bool IsAccepting() const noexcept;

    /**
     * @brief Approximate number of accepted or in-flight submissions.
     *
     * Intended for telemetry and shutdown diagnostics only.
     */
    [[nodiscard]]
    std::size_t ApproximatePendingCount() const noexcept;

private:
    struct PrivateTag
    {
    };

public:
    SessionCommandScheduler(PrivateTag,
                            std::weak_ptr<idtx::concurrency::WorkerExecutor> executor,
                            CommandHandler                                   command_handler);

private:
    template<std::size_t Capacity>
    CommandAdmissionStatus Submit(BoundedMpscMailbox<SessionCommand, Capacity>& mailbox,
                                  SessionCommand&&                              command);

    void ScheduleDrainIfRequired();
    void RunDrainTask() noexcept;

    // Process up to kMaxCommandsPerDrain commands, then clear
    // drain_scheduled_. Returns true if more work is pending and a new drain
    // must be scheduled.
    [[nodiscard]] bool DrainBatch() noexcept;
    void ReleasePending() noexcept;

    BoundedMpscMailbox<SessionCommand, kMailboxCapacity>         mailbox_;
    BoundedMpscMailbox<SessionCommand, kPriorityMailboxCapacity> priority_mailbox_;

    std::weak_ptr<idtx::concurrency::WorkerExecutor> executor_;
    CommandHandler                                   command_handler_;

    // Counts submissions that have entered TrySubmit() but have not yet been
    // processed or rejected, in both mailboxes. It is incremented before the
    // admission check and before TryPush(), so a producer that has reserved but not yet published
    // a mailbox position is already visible to the consumer and to WaitIdle().
    std::atomic<std::size_t> pending_count_{0};

    // true while a drain is scheduled on the executor or running.
    std::atomic<bool> drain_scheduled_{false};

    // Once false, new submissions are rejected. Closing admission does not
    // cancel commands that were already accepted.
    std::atomic<bool> accepting_{true};
};

} // namespace idtx::session
