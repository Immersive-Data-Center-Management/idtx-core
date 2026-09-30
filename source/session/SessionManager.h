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
 * Known limitation: sessions opened on the same USD file share the root
 * SdfLayer through the USD layer registry. Reloading or committing that layer
 * from one session's consumer also delivers change notices to the other
 * sessions' stages on that thread. Operations that mutate the shared layer
 * are serialized across sessions, but a concurrent read on another session's
 * consumer is not.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
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
     * Walks @c m_sessions_ under a shared lock; session counts are small
     * and this is only called on upload replacement and commit.
     *
     * @param usd_file  Uploads-relative path to look up.
     * @return All matching sessions, or an empty vector when none exist.
     */
    std::vector<std::shared_ptr<Session>> FindByUsdFile(const std::string& usd_file) const;

    /**
     * @brief Ask every live session whose @c usd_file matches @p usd_file to
     *        reload its root layer, in response to an on-disk replacement of
     *        the backing file.
     *
     * Submits one reload command per session and returns immediately. Each
     * command runs on the session's consumer, in order with the session's
     * other commands:
     *   1. sets @c Session::reload_in_progress so @c StageNoticeListener can
     *      distinguish the reload notice from a normal authoring notice,
     *   2. issues @c SdfLayer::Reload(force=true) on the stage's root
     *      layer inside a @c SdfChangeBlock so a single notice is fired,
     *   3. relies on the listener to broadcast the surviving changes to
     *      attached clients. The listener drops broadcasts for any
     *      attribute whose session-layer opinion still wins by
     *      composition strength.
     *
     * Safe to call from any thread, including a command handler. Failures
     * inside a single session are logged but do not affect other sessions.
     *
     * @param usd_file  Uploads-relative path of the file that was just
     *                  replaced on disk.
     * @return Number of sessions that accepted the reload command.
     */
    std::size_t ReloadSessionsForFile(const std::string& usd_file);

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
    void DetachClient(const std::string& session_id,
                      ConnectionId connection_id);

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
     * failure and, for QueueFull, for calling RequestCorrection().
     */
    SubmitStatus SubmitTransformUpdate(const std::string& session_id,
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
    void RequestCorrection(const std::string& session_id,
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
    SubmitStatus RequestJoinSnapshot(const std::string& session_id,
                                     ConnectionId connection_id);

    // ------------------------------------------------------------------
    // Broadcast (invoked from StageNoticeListener on the consumer)
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

    using CommitStatus = idtx::session::CommitStatus;

    /// How long CommitSession() waits for the session's consumer.
    static constexpr std::chrono::seconds kCommitTimeout{30};

    /**
     * @brief Merge the session's sidecar overrides back into the original USD
     *        file (preserving composition arcs via UsdUtilsFlattenLayerStack),
     *        then ask every live session bound to the same file to reload.
     *        Does not destroy the session.
     *
     * The commit is queued behind every command the session has already
     * accepted, so it contains all previously acknowledged updates. Blocks
     * the calling thread for at most kCommitTimeout and returns
     * CommitStatus::Unavailable if the commit could not be queued or did not
     * finish in time. Safe to call from the REST thread; must not be called
     * from a command handler.
     */
    CommitStatus CommitSession(const std::string& session_id, std::string& out_error);

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
    std::size_t FlushDirtySessions();

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
    void HandleCommand(Session& session, CommitCommand& command);
    void HandleCommand(Session& session, ReloadCommand& command);
    void HandleCommand(Session& session, FlushCommand& command);
    void HandleCommand(Session& session, CorrectionCommand& command);

    /// Save the sidecar and fold the session's overrides into the original
    /// file. Requires exclusive access to the session's stage (the consumer,
    /// or a teardown after the consumer went idle).
    CommitResult CommitOverrides(Session& session);

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
    /// path. Must be called without m_mutex_ held and never from a worker.
    void TeardownSession(const std::shared_ptr<Session>& session);

    /// Background reaper loop: periodically calls ReapIdleSessions() until the
    /// manager is destroyed. Only started when m_idle_timeout_ > 0.
    void ReaperRun();

    // Shared by all sessions' command schedulers, which only hold weak
    // references. Stopped in the destructor after every session is idle.
    std::shared_ptr<idtx::concurrency::WorkerExecutor>         m_executor_;

    // Serializes commands that mutate a root layer which may be shared with
    // other sessions on the same file (reload and commit).
    std::mutex                                                 m_shared_layer_mutex_;

    idtx::utils::UsdFileLocator                                m_locator_;
    std::filesystem::path                                      m_sessions_dir_;
    mutable std::shared_mutex                                  m_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>>  m_sessions_;

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
