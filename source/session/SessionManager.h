/**
 * @file SessionManager.h
 * @brief Owns all collaboration sessions and serves as the single entry
 *        point for both REST (SessionController) and websocket
 *        (WebSocketController) layers.
 *
 * Responsibilities:
 *   - validate the requested USD file using utils::UsdFileLocator
 *   - open the UsdStage and register a TfNotice listener (Variant B)
 *   - track connected websocket clients per session
 *   - turn every stage access into a SessionCommand and process each
 *     session's commands serially on a shared WorkerExecutor
 *   - perform per-session broadcast (called from the listener)
 *
 * Threading model: producers (websocket io threads, REST handlers, the
 * flusher and the reaper) only submit commands, which never blocks. Each
 * session's commands are executed one at a time, in submission order, by the
 * session's consumer. All stage reads and writes, and all per-session
 * outbound frames (acks, broadcasts, snapshots, corrections), happen on
 * that consumer.
 *
 * Shared layers: the USD layer registry hands every stage that uses a file
 * the same SdfLayer, whether the file is the stage's root or reached through
 * a sublayer, reference or payload. Reloading such a layer recomposes every
 * stage using it, and runs their listeners, on the reloading thread. Reloads
 * therefore run on a dedicated layer thread, never on a worker. It holds
 * idtx::concurrency::SharedLayers() exclusively and pauses exactly the
 * sessions whose Session::used_layers contain the layer, so their consumers
 * are not running while it recomposes their stages; every other session keeps
 * running. No worker ever blocks on this: a paused session simply is not
 * drained, and a command that needs SharedLayers() while a reload is pending
 * defers itself instead of waiting.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <crow/websocket.h>
#include <nlohmann/json.hpp>

#include <idtx/utils/Logger.h>

#include "Session.h"
#include "SessionCommand.h"
#include "dto/SessionDto.h"
#include "utils/UsdFileLocator.h"

namespace idtxcore { class TransformUpdate; }
namespace idtx::concurrency { class WorkerExecutor; }

namespace idtx
{
namespace session
{

class SessionManager
{
    IDTX_LOG_CATEGORY("SessionManager")

public:
    /**
     * @brief Outcome of a session creation request.
     */
    enum class CreateStatus
    {
        Ok,
        FileNotFound,       // path is invalid or file does not exist
        StageOpenFailed     // path is fine, but UsdStage::Open returned null
    };

    /**
     * @brief Construct a session manager.
     *
     * @param file_locator  Resolves request-supplied paths to on-disk USD files.
     * @param idle_timeout  How long a session may have zero connected clients
     *        before the background reaper destroys it. A value of 0 (the
     *        default) disables the reaper entirely, so sessions persist until an
     *        explicit Destroy() or manager shutdown.
     * @param worker_count  Number of worker threads shared by all sessions to
     *        process their commands. Values below 1 are raised to 1.
     */
    explicit SessionManager(idtx::utils::UsdFileLocator file_locator,
                            std::chrono::seconds idle_timeout = std::chrono::seconds{0},
                            std::size_t worker_count = 1);
    ~SessionManager();

    SessionManager(const SessionManager&)            = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // ------------------------------------------------------------------
    // REST-side API
    // ------------------------------------------------------------------

    /**
     * @brief Create a new session for the given USD file.
     * @param usd_file Relative path under the uploads root.
     * @param out_error Filled in with a human-readable error on failure.
     * @return The new Session on success, nullptr otherwise. Also returns
     *         the granular reason via @p out_status.
     */
    std::shared_ptr<Session> Create(const std::string& usd_file,
                                    idtx::dto::SessionMode mode,
                                    std::string& out_error,
                                    CreateStatus& out_status,
                                    bool auto_commit = false);

    std::shared_ptr<Session> Get(const std::string& id) const;
    std::vector<std::shared_ptr<Session>> List() const;
    bool Exists(const std::string& id) const;

    /**
     * @brief Remove the session, let its already queued commands finish, then
     *        tear it down. Blocks until the session's consumer is idle. Must
     *        not be called from a command handler.
     */
    bool Destroy(const std::string& id);

    /**
     * @brief Return every live session whose @c usd_file matches
     *        @p usd_file exactly (as the uploads-relative string used
     *        when the session was created).
     *
     * Walks @c m_sessions_ under a shared lock; session counts are small.
     *
     * @param usd_file  Uploads-relative path to look up.
     * @return All matching sessions, or an empty vector when none exist.
     */
    std::vector<std::shared_ptr<Session>> FindByUsdFile(const std::string& usd_file) const;

    /**
     * @brief Reload the in-memory layer of @p usd_file after the file was
     *        replaced on disk.
     *
     * Queues the reload on the layer thread and returns immediately. If no
     * stage has the file loaded by then there is nothing to do. Otherwise the
     * layer is reloaded once, inside a single change block, and every stage
     * that uses it is notified: sessions on the file itself and sessions on
     * files that sublayer, reference or payload it. Each session's listener
     * takes its reload branch and drops the broadcast for any prim whose
     * session-layer opinion still wins by composition strength.
     *
     * Never blocks. Safe to call from any thread.
     *
     * @param usd_file  Uploads-relative path of the file that was replaced.
     * @param hold      Released only once the reload has finished; pass the
     *                  slot from AcquireFileWriteSlot() so no commit or other
     *                  upload writes the file before its layer is reloaded.
     * @return true if the reload was queued.
     */
    bool ReloadSharedLayer(const std::string& usd_file, std::shared_ptr<void> hold = {});

    /**
     * @brief Take the single slot that commits hold while they write a file
     *        and until its layer is reloaded.
     *
     * An upload that replaces an existing file holds it while it writes, so
     * an upload and a commit never write the same file at once and neither
     * flattens or reloads a half-finished state of the other. The slot is
     * shared by all files: commits and replacing uploads run one at a time.
     *
     * Blocks for at most @p timeout. Must not be called from a command
     * handler or while holding SharedLayers().
     *
     * @return The slot, released when the last copy is destroyed, or null if
     *         it did not become free in time.
     */
    std::shared_ptr<void> AcquireFileWriteSlot(std::chrono::milliseconds timeout);

    // ------------------------------------------------------------------
    // Websocket-side API
    // ------------------------------------------------------------------

    /**
     * @brief Outcome of an AttachClient request.
     */
    enum class AttachStatus
    {
        Ok,
        UnknownSession,
        SingleEditBusy
    };

    /**
     * @brief Register @p conn under @p connection_id with the session.
     *
     * @p connection_id must not be reused while the session exists. It is
     * what commands carry to refer to the connection.
     */
    AttachStatus AttachClient(const std::string& session_id,
                              ConnectionId connection_id,
                              crow::websocket::connection* conn);

    /**
     * @brief Remove @p connection_id from @p session's client registry.
     *
     * Takes the session itself, not its id: a session that is being
     * destroyed is no longer found by id, but its remaining commands still
     * send to its clients until the teardown finished, so a closing
     * connection must still be removed.
     */
    void DetachClient(Session& session, ConnectionId connection_id);

    /**
     * @brief Returns true if the session exists, is in SingleEdit mode and
     *        already has at least one attached client. Used by
     *        WebSocketController::OnAccept to reject a second editor with an
     *        HTTP 4xx before the upgrade completes. Cheap; takes a shared
     *        lock on the session's client registry.
     */
    bool IsSingleEditBusy(const std::string& session_id) const;

    // ------------------------------------------------------------------
    // Command submission (called by WebSocketController)
    // ------------------------------------------------------------------

    /**
     * @brief Outcome of submitting a command to a session.
     */
    enum class SubmitStatus
    {
        Accepted,
        UnknownSession,
        QueueFull,      // the session's queue is full; retry later
        SessionClosing  // the session is being destroyed
    };

    /**
     * @brief Queue a TransformUpdate for the session's consumer.
     *
     * Never blocks. When accepted, the consumer first checks @p base_seq: an
     * update made on an outdated state is acked with "stale" or
     * "invalid_base" and not applied. Otherwise the consumer applies the
     * update, the TfNotice listener broadcasts the result to every other
     * client, and @p origin receives an Ack carrying @p request_id. If the
     * apply fails, the Ack is followed by a correction to @p origin. When not
     * accepted, no Ack is sent; the caller is responsible for reporting the
     * failure and, for QueueFull, for calling SubmitCorrectionCommand().
     */
    SubmitStatus SubmitTransformCommand(const std::string& session_id,
                                        std::unique_ptr<const idtxcore::TransformUpdate> update,
                                        ConnectionId origin,
                                        std::uint64_t request_id,
                                        std::uint64_t base_seq) const;

    /**
     * @brief Queue a correction of @p request->prim_path for @p connection_id
     *        after one of its updates was rejected with QueueFull.
     *
     * The consumer sends the prim's server state to that connection only,
     * before any normal command still queued. Does nothing if the same
     * request is already queued. Never blocks; if the correction cannot be
     * queued it is dropped and a later rejection retries.
     */
    void SubmitCorrectionCommand(const std::string& session_id,
                                 ConnectionId connection_id,
                                 std::shared_ptr<PendingCorrection> request) const;

    /**
     * @brief Queue a join snapshot for a freshly attached connection.
     *
     * The consumer pushes the current server-side stage state to
     * @p connection_id so a late joiner is not left with a stale
     * REST-fetched file: one @c kXformBroadcast per prim with authored
     * opinions in the session (sidecar) layer, then a terminal
     * @c kSnapshotComplete frame. Because it runs in order with all other
     * commands, the snapshot is consistent with the broadcasts that follow.
     */
    SubmitStatus SubmitJoinSnapshotCommand(const std::string& session_id,
                                           ConnectionId connection_id);

    /**
     * @brief Queue a built-in primitive creation requested by a client.
     *
     * Never blocks. When accepted, the consumer authors a new gprim of @p type
     * under @p parent_path into the session layer with its schema defaults,
     * assigns it a unique name, broadcasts the creation to every client of the
     * session (including @p origin) and sends @p origin an Ack carrying
     * @p request_id. A request with an unknown type or an invalid/forbidden
     * parent (missing, or a gprim/descendant of one) is acked as not ok and
     * nothing is authored. When not accepted, no Ack is sent; the caller
     * reports the failure. Creation is additive with a server-chosen unique
     * name, so there is no base_seq/staleness check.
     */
    SubmitStatus SubmitCreatePrimCommand(const std::string& session_id,
                                         std::string parent_path,
                                         idtxcore::PrimType type,
                                         ConnectionId origin,
                                         std::uint64_t request_id) const;

    /**
     * @brief Queue a prim active-state change (soft delete / undo-delete)
     *        requested by a client.
     *
     * Never blocks. When accepted, the consumer authors the `active` flag into
     * the session layer (active=false soft-deletes the prim and prunes its
     * subtree; active=true undoes it), broadcasts a SetPrimActiveBroadcast to
     * every client (including @p origin) and sends @p origin an Ack carrying
     * @p request_id. A request for an unknown prim, or a delete of a prim that
     * is not locally deletable (introduced across a reference/payload or defined
     * in a sublayer), is acked not-ok and nothing is authored. A request that is
     * already in the target state is acked ok with no broadcast (idempotent).
     * When not accepted, no Ack is sent; the caller reports the failure.
     */
    SubmitStatus SubmitSetPrimActiveCommand(const std::string& session_id,
                                            std::string prim_path,
                                            bool active,
                                            ConnectionId origin,
                                            std::uint64_t request_id) const;

    /**
     * @brief Queue adding (or, with @p remove, removing) a payload arc on
     *        @p prim_path. Placeholder: the locking, deferral and layer
     *        bookkeeping are in place, the authoring is not implemented yet
     *        and the request is acked as "not_implemented".
     *
     * Never blocks. Not yet reachable from the websocket protocol.
     */
    SubmitStatus SubmitPayloadCommand(const std::string& session_id,
                                      std::string prim_path,
                                      std::string asset_path,
                                      bool remove,
                                      ConnectionId origin,
                                      std::uint64_t request_id) const;

    // ------------------------------------------------------------------
    // Broadcast (invoked from StageNoticeListener, on the session's consumer
    // or on the layer thread while the session is paused)
    // ------------------------------------------------------------------

    /**
     * @brief Broadcast a TransformBroadcast for a single prim path to all
     *        connected clients of @p session, except @p origin.
     *        Reads the resolved transform from the stage at default time and
     *        stamps the frame with @p server_seq.
     */
    void BroadcastResolvedTransform(Session& session,
                                    const std::string& prim_path,
                                    ConnectionId origin,
                                    std::uint64_t server_seq);

    /**
     * @brief Send an already-serialized frame to every connected client of
     *        @p session (including the originator) and record @p server_seq as
     *        sent to each. Used for prim-creation broadcasts, which — unlike
     *        transforms — are echoed back to the client that triggered them.
     */
    void BroadcastToAll(Session& session,
                        const std::string& payload,
                        std::uint64_t server_seq);

    using CommitStatus = idtx::session::CommitStatus;

    /// How long SubmitCommitSessionCommand() waits for the session's consumer.
    static constexpr std::chrono::seconds kCommitTimeout{30};

    /**
     * @brief Merge the session's sidecar overrides back into the original USD
     *        file (preserving composition arcs via UsdUtilsFlattenLayerStack),
     *        then reload the file's layer, which every stage using it sees.
     *        Does not destroy the session.
     *
     * The commit is queued behind every command the session has already
     * accepted, so it contains all previously acknowledged updates. The
     * session's consumer writes the file; the layer thread then reloads the
     * file's layer, pausing only the sessions that use it. Before this
     * returns success, every session using the layer, on this file or
     * another, has been notified. Commits run one at a time from submission
     * until their reload is done, so each flattens the baseline the previous
     * one reloaded. Blocks the calling thread for at most kCommitTimeout for
     * the commit slot and again for the result. Returns
     * CommitStatus::Unavailable if nothing was queued (slot not free in
     * time, queue full, session closing), and CommitStatus::Pending if the
     * commit was queued but has not finished in time: it still runs, only
     * its outcome is not reported. Safe to call from the REST thread; must
     * not be called from a command handler.
     */
    CommitStatus SubmitCommitSessionCommand(const std::string& session_id, std::string& out_error);

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    /// Build a JSON description of @p session that is safe to return from REST.
    static nlohmann::json ToJson(const Session& session);

    const idtx::utils::UsdFileLocator& GetLocator() const noexcept { return m_locator_; }

    /**
     * @brief Destroy every session that has had zero connected clients for at
     *        least the configured idle timeout, and return how many were
     *        reaped. Normally driven by the background reaper thread; exposed
     *        so tests can trigger a sweep deterministically without waiting.
     *        A no-op that returns 0 when the idle timeout is disabled (0).
     */
    std::size_t ReapIdleSessions();

    /**
     * @brief Queue a flush command for every session currently flagged
     *        dirty. The consumer saves the session (sidecar) layer and clears
     *        the flag on success, so the save never races client-driven
     *        authoring. Driven by the background SessionFlusher. Returns the
     *        number of flush commands accepted.
     */
    std::size_t SubmitFlushDirtySessionsCommand();

private:
    /// Submit @p command to @p session and map the admission result.
    static SubmitStatus Submit(Session& session, SessionCommand command);
    /// Like Submit(), but the command overtakes the session's normal queue.
    static SubmitStatus SubmitPriority(Session& session, SessionCommand command);

    /// Command handler installed on every session's scheduler. Runs on the
    /// session's consumer.
    void ProcessCommand(Session& session, SessionCommand&& command);

    void HandleCommand(Session& session, TransformCommand& command);
    void HandleCommand(Session& session, JoinCommand& command);
    void HandleCommand(Session& session, CreatePrimCommand& command);
    void HandleCommand(Session& session, SetPrimActiveCommand& command);
    void HandleCommand(Session& session, PayloadCommand& command);
    void HandleCommand(Session& session, CommitCommand& command);
    void HandleCommand(Session& session, FlushCommand& command);
    void HandleCommand(Session& session, CorrectionCommand& command);

    /// Save the sidecar and fold the session's overrides into the original
    /// file. Does not reload. Requires exclusive access to the session's
    /// stage (the consumer, or a teardown after the consumer went idle) and
    /// the commit slot.
    CommitResult CommitOverrides(Session& session);

    /// Layer thread: reload @p layer with SharedLayers() held exclusively,
    /// pausing every session that uses it for the duration. @p label names
    /// the file in log messages. Returns true if the layer changed.
    bool ReloadLayerPausingUsers(const pxr::SdfLayerHandle& layer, const std::string& label);

    /// Every session, live or being torn down, whose used_layers contain
    /// @p layer. The caller holds SharedLayers() exclusively.
    std::vector<std::shared_ptr<Session>> SessionsUsingLayer(const pxr::SdfLayerHandle& layer) const;

    /// Recompute @p session's used_layers from its stage. The caller holds
    /// SharedLayers() and the session's consumer is not running elsewhere.
    static void RefreshUsedLayers(Session& session);

    /// Ownership of the already acquired m_commit_slot_: releases it when the
    /// last copy is destroyed, on whichever thread that happens.
    std::shared_ptr<void> CommitSlotToken();

    /// Queue @p job on the layer thread. Returns false once it has stopped.
    bool PostLayerJob(std::function<void()> job);

    /// Layer thread loop: runs queued jobs in order until stopped and empty.
    void LayerThreadRun();

    /// Send @p payload to one connection of @p session, if still attached.
    /// Returns true if it was sent.
    static bool SendToConnection(const Session& session,
                                 ConnectionId connection_id,
                                 const std::string& payload);

    /// Send an Ack stamped with the current server_seq to @p connection_id.
    static void SendAck(Session& session,
                        ConnectionId connection_id,
                        std::uint64_t request_id,
                        bool ok,
                        const char* error);

    /// Send the current state of @p prim_path to @p connection_id only, as a
    /// TransformBroadcast with a new server_seq. The stage is not changed.
    /// Nothing is sent if the prim has no readable transform or the
    /// connection has detached.
    static void SendCorrection(Session& session,
                               ConnectionId connection_id,
                               const std::string& prim_path);

    /// Close admission, wait for the session's consumer to finish every
    /// accepted command, then revoke the listener and clear the stage's
    /// session layer before the last reference is dropped. Clearing avoids
    /// racing the thumbnail worker's SdfLayer::FindOrOpen on the same on-disk
    /// path. An auto-commit queues a reload of the written file on the layer
    /// thread, holding the commit slot until it is done. Must be called
    /// without m_mutex_ held and never from a worker.
    void TeardownSession(const std::shared_ptr<Session>& session);

    /// Background reaper loop: periodically calls ReapIdleSessions() until the
    /// manager is destroyed. Only started when m_idle_timeout_ > 0.
    void ReaperRun();

    // Shared by all sessions' command schedulers, which only hold weak
    // references. Stopped in the destructor after every session is idle.
    std::shared_ptr<idtx::concurrency::WorkerExecutor>         m_executor_;

    // Held by one commit at a time, from SubmitCommitSessionCommand() until
    // the reload of the written file has finished, and by a teardown's
    // auto-commit. A semaphore rather than a mutex because the commit's
    // phases run on three threads.
    std::binary_semaphore                                      m_commit_slot_{1};

    idtx::utils::UsdFileLocator                                m_locator_;
    std::filesystem::path                                      m_sessions_dir_;
    mutable std::shared_mutex                                  m_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>>  m_sessions_;

    // Sessions removed from m_sessions_ whose teardown has not finished.
    // Their stages still use layers and their consumers may still run, so a
    // reload must find and pause them too. Guarded by m_mutex_.
    std::vector<std::shared_ptr<Session>>                      m_closing_;

    // Layer thread: runs every reload, so that no executor worker ever holds
    // SharedLayers() exclusively or waits for a session to pause.
    std::mutex                                                 m_layer_mutex_;
    std::condition_variable                                    m_layer_cv_;
    std::deque<std::function<void()>>                          m_layer_jobs_;
    bool                                                       m_layer_stop_ = false;
    std::thread                                                m_layer_thread_;

    // Idle-session reaper. m_idle_timeout_ == 0 disables it and leaves the
    // thread unstarted. m_reaper_mutex_/m_reaper_cv_ drive the sleep/wake of
    // the loop; m_reaper_stop_ signals shutdown from the destructor.
    std::chrono::seconds                                       m_idle_timeout_{0};
    std::atomic<bool>                                          m_reaper_stop_{false};
    std::mutex                                                 m_reaper_mutex_;
    std::condition_variable                                    m_reaper_cv_;
    std::thread                                                m_reaper_thread_;
};

} // namespace session
} // namespace idtx
