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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

#include <idtx/proto/transform.pb.h>
#include <idtx/proto/prim.pb.h>

#include "FileWriteSlot.h"

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
    // The commit was not queued (queue full, session closing, or another
    // commit or upload still writing). Nothing was written; retrying later
    // may succeed.
    Unavailable,
    // The commit is queued and still runs, but did not finish while the
    // caller waited (for example because its reload waits for a thumbnail
    // render). Its outcome is not reported to the caller; retrying is not
    // needed.
    Pending,
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

    // The server_seq the client had received and applied when it created the
    // update. The update is rejected if it was made on an outdated state.
    std::uint64_t base_seq{};

    std::chrono::steady_clock::time_point received_at{
        std::chrono::steady_clock::now()};

    TransformCommand(std::unique_ptr<const idtxcore::TransformUpdate> update_value,
                     ConnectionId                                     origin_value,
                     std::uint64_t                                    request_id_value,
                     std::uint64_t                                    base_seq_value) noexcept
        : update(std::move(update_value))
        , origin(origin_value)
        , request_id(request_id_value)
        , base_seq(base_seq_value)
    {
    }

    TransformCommand(TransformCommand&&) noexcept            = default;
    TransformCommand& operator=(TransformCommand&&) noexcept = default;

    TransformCommand(const TransformCommand&)            = delete;
    TransformCommand& operator=(const TransformCommand&) = delete;

    ~TransformCommand() noexcept = default;
};

/**
 * @brief Send the current session state to one connection: when it joins,
 *        and when it asks for it with a SnapshotRequest.
 *
 * Because it runs on the consumer, the snapshot reflects exactly the state
 * after every command queued before it and before every command queued after
 * it.
 */
struct SnapshotCommand
{
    ConnectionId connection{};

    // Echoed in the SnapshotComplete that ends the snapshot, so a client that
    // asked for it can tell it apart from any other. 0 for a join.
    std::uint64_t request_id{};
};

/**
 * @brief Create a primitive requested by a websocket client.
 *
 * The consumer authors the prim described by @c request under its parent path
 * into the session layer — a built-in typed gprim/xform, or a typeless
 * reference/payload prim — optionally placed by the request's local
 * transform, assigns it a unique name, broadcasts the creation to every client
 * (including @c origin) and sends @c origin an Ack carrying @c request_id. The
 * request is owned via a unique_ptr so the command stays nothrow-movable as the
 * mailbox requires.
 */
struct CreatePrimCommand
{
    std::unique_ptr<const idtxcore::CreatePrimRequest> request;

    // Connection that sent the request; it receives the Ack and, like every
    // other client, the creation broadcast.
    ConnectionId origin{};

    // Client-assigned id echoed back in the Ack (0 if the client sent none).
    std::uint64_t request_id{};

    std::chrono::steady_clock::time_point received_at{
        std::chrono::steady_clock::now()};

    CreatePrimCommand(std::unique_ptr<const idtxcore::CreatePrimRequest> request_value,
                      ConnectionId                                       origin_value,
                      std::uint64_t                                      request_id_value) noexcept
        : request(std::move(request_value))
        , origin(origin_value)
        , request_id(request_id_value)
    {
    }

    CreatePrimCommand(CreatePrimCommand&&) noexcept            = default;
    CreatePrimCommand& operator=(CreatePrimCommand&&) noexcept = default;

    CreatePrimCommand(const CreatePrimCommand&)            = delete;
    CreatePrimCommand& operator=(const CreatePrimCommand&) = delete;

    ~CreatePrimCommand() noexcept = default;
};

/**
 * @brief Change the active state of an existing prim: a soft delete
 *        (@c active == false) or its undo (@c active == true).
 *
 * The consumer validates the prim (it must exist and be defined only in the
 * stage's local layers — the root layer and/or this session's own layer — not
 * introduced across a reference/payload or defined in a sublayer), authors the
 * `active` flag into the session layer, broadcasts a SetPrimActiveBroadcast to
 * every client (including @c origin) and sends @c origin an Ack carrying
 * @c request_id. Deactivation prunes the prim and its subtree from the composed
 * stage; the prim is physically removed from the file only on commit. The
 * members are plain values, so the command is trivially nothrow-movable as the
 * mailbox requires.
 *
 * Pruning or restoring a subtree also drops or composes again every layer it
 * references or payloads, so like creating ref/payload prims it runs holding
 * idtx::concurrency::SharedLayers() shared and refreshes
 * Session::used_layers.
 */
struct SetPrimActiveCommand
{
    std::string  prim_path;
    bool         active{false};

    // Connection that sent the request; it receives the Ack and, like every
    // other client, the broadcast.
    ConnectionId origin{};

    // Client-assigned id echoed back in the Ack (0 if the client sent none).
    std::uint64_t request_id{};

    std::chrono::steady_clock::time_point received_at{
        std::chrono::steady_clock::now()};
};

/**
 * @brief Commit the session's overrides into the original USD file.
 *
 * The consumer writes the file; the file's layer is then reloaded off the
 * worker pool, and @c result is set once that reload is done. The submitter
 * waits on the future obtained from @c result.
 */
struct CommitCommand
{
    std::promise<CommitResult> result;

    // Keeps the manager's single commit slot taken from submission until the
    // reload of the written file has finished, wherever the commit ends:
    // released when the last copy is destroyed.
    FileWriteSlot commit_slot;
};

/** @brief Save the session layer to its sidecar file if it is dirty. */
struct FlushCommand
{
};

/**
 * @brief A correction owed to one connection for one prim.
 *
 * Shared between the connection's io thread, which sets @c queued when it
 * submits a CorrectionCommand, and the consumer, which clears it before it
 * reads the prim's state. So at most one correction per (connection, prim)
 * is queued at a time, and a rejection after the state was read queues a new
 * one.
 */
struct PendingCorrection
{
    explicit PendingCorrection(std::string prim_path_value)
        : prim_path(std::move(prim_path_value))
    {
    }

    const std::string prim_path;
    std::atomic<bool> queued{false};
};

/**
 * @brief Send the server state of a prim to one connection whose valid update
 *        was not applied because the queue was full.
 *
 * Submitted with priority, so it runs before the commands already queued.
 */
struct CorrectionCommand
{
    ConnectionId                       connection{};
    std::shared_ptr<PendingCorrection> request;
};

using SessionCommand = std::variant<
    TransformCommand,
    SnapshotCommand,
    CreatePrimCommand,
    SetPrimActiveCommand,
    CommitCommand,
    FlushCommand,
    CorrectionCommand>;

static_assert(std::is_nothrow_move_constructible_v<SessionCommand>);
static_assert(std::is_nothrow_destructible_v<SessionCommand>);

} // namespace idtx::session
