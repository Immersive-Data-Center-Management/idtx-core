/**
 * @file SessionCommand.h
 * @brief Commands processed by a session's single consumer.
 *
 * Every operation that reads or writes a session's UsdStage is expressed as a
 * command, submitted to the session's command queue and executed serially by
 * one consumer at a time. Commands own all data they need, are move-only and
 * must be nothrow-movable so they can live in the lock-free mailbox.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

#include <idtx/proto/transform.pb.h>

namespace idtx::session
{

/**
 * @brief Server-local identifier of a websocket connection.
 *
 * Commands refer to connections by id instead of by pointer. The consumer
 * resolves the id against the session's client registry at send time, so a
 * connection that closed while a command was queued is simply skipped.
 * 0 means "no connection".
 */
using ConnectionId = std::uint64_t;

/** @brief Outcome of committing a session's overrides to the original file. */
enum class CommitStatus
{
    Ok,
    UnknownSession,
    NothingToCommit,
    WriteFailed,
    // The commit could not be scheduled or did not complete in time (queue
    // full, session closing, or timeout). Retrying later may succeed.
    Unavailable,
};

struct CommitResult
{
    CommitStatus status = CommitStatus::WriteFailed;
    std::string  error;
};

/** @brief Apply one TransformUpdate received from a websocket client. */
struct TransformCommand
{
    std::unique_ptr<const idtxcore::TransformUpdate> update;

    // Connection that sent the update. Its echo broadcast is suppressed and
    // it receives the Ack.
    ConnectionId origin{};

    // Client-assigned id echoed back in the Ack (0 if the client sent none).
    std::uint64_t request_id{};

    std::chrono::steady_clock::time_point received_at{
        std::chrono::steady_clock::now()};

    TransformCommand(std::unique_ptr<const idtxcore::TransformUpdate> update_value,
                     ConnectionId                                     origin_value,
                     std::uint64_t                                    request_id_value) noexcept
        : update(std::move(update_value))
        , origin(origin_value)
        , request_id(request_id_value)
    {
    }

    TransformCommand(TransformCommand&&) noexcept            = default;
    TransformCommand& operator=(TransformCommand&&) noexcept = default;

    TransformCommand(const TransformCommand&)            = delete;
    TransformCommand& operator=(const TransformCommand&) = delete;

    ~TransformCommand() noexcept = default;
};

/**
 * @brief Send the current session state to a newly joined connection.
 *
 * Because it runs on the consumer, the snapshot reflects exactly the state
 * after every command queued before it and before every command queued after
 * it.
 */
struct JoinCommand
{
    ConnectionId connection{};
};

/**
 * @brief Commit the session's overrides into the original USD file.
 *
 * The submitter waits on the future obtained from @c result.
 */
struct CommitCommand
{
    std::promise<CommitResult> result;
};

/** @brief Reload the root layer from disk after the file changed. */
struct ReloadCommand
{
};

/** @brief Save the session layer to its sidecar file if it is dirty. */
struct FlushCommand
{
};

using SessionCommand = std::variant<
    TransformCommand,
    JoinCommand,
    CommitCommand,
    ReloadCommand,
    FlushCommand>;

static_assert(std::is_nothrow_move_constructible_v<SessionCommand>);
static_assert(std::is_nothrow_destructible_v<SessionCommand>);

} // namespace idtx::session
