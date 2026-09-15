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
 *   - serialize stage writes through Session::stage_mutex
 *   - perform per-session broadcast (called from the listener)
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
#include "dto/SessionDto.h"
#include "utils/UsdFileLocator.h"

namespace idtxcore { class TransformUpdate; }

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
     */
    explicit SessionManager(idtx::utils::UsdFileLocator file_locator,
                            std::chrono::seconds idle_timeout = std::chrono::seconds{0});
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
    bool Destroy(const std::string& id);

    /**
     * @brief Return every live session whose @c usd_file matches
     *        @p usd_file exactly (as the uploads-relative string used
     *        when the session was created).
     *
     * Walks @c m_sessions_ under a shared lock; session counts are small
     * and this is only called on upload replacement.
     *
     * @param usd_file  Uploads-relative path to look up.
     * @return All matching sessions, or an empty vector when none exist.
     */
    std::vector<std::shared_ptr<Session>> FindByUsdFile(const std::string& usd_file) const;

    /**
     * @brief Reload the root layer of every live session whose
     *        @c usd_file matches @p usd_file, in response to an on-disk
     *        replacement of the backing file.
     *
     * For each affected session the manager:
     *   1. acquires the session's @c stage_mutex,
     *   2. sets @c Session::reload_in_progress = true so
     *      @c StageNoticeListener can distinguish the reload notice from
     *      a normal authoring notice,
     *   3. issues @c SdfLayer::Reload(force=true) on the stage's root
     *      layer inside a @c SdfChangeBlock so a single notice is fired,
     *   4. relies on the listener to broadcast the surviving changes to
     *      attached clients — the listener drops broadcasts for any
     *      attribute whose session-layer opinion still wins by
     *      composition strength
     *
     * Safe to call from any thread. Failures inside a single session are
     * logged but do not affect other sessions.
     *
     * @param usd_file  Uploads-relative path of the file that was just
     *                  replaced on disk.
     * @return Number of sessions the manager asked to reload.
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

    AttachStatus AttachClient(const std::string& session_id,
                              crow::websocket::connection* conn);
    void DetachClient(const std::string& session_id,
                      crow::websocket::connection* conn);

    /**
     * @brief Returns true if the session exists, is in SingleEdit mode and
     *        already has at least one attached client. Used by
     *        WebSocketController::OnAccept to reject a second editor with an
     *        HTTP 4xx before the upgrade completes. Cheap; takes a shared
     *        lock on the session's client registry.
     */
    bool IsSingleEditBusy(const std::string& session_id) const;

    // ------------------------------------------------------------------
    // Stage authoring entrypoint (called by WebSocketController)
    // ------------------------------------------------------------------

    /**
     * @brief Apply a TransformUpdate to the session's stage. Mutates the
     *        stage under stage_mutex. The TfNotice listener will pick the
     *        change up and trigger a broadcast (Variant B).
     */
    bool ApplyTransformUpdate(const std::string& session_id,
                              const idtxcore::TransformUpdate& upd,
                              crow::websocket::connection* origin);

    // ------------------------------------------------------------------
    // Broadcast (invoked from StageNoticeListener)
    // ------------------------------------------------------------------

    /**
     * @brief Broadcast a TransformBroadcast for a single prim path to all
     *        connected clients of @p session, except @p origin.
     *        Reads the resolved transform from the stage at default time.
     */
    void BroadcastResolvedTransform(const std::shared_ptr<Session>& session,
                                    const std::string& prim_path,
                                    crow::websocket::connection* origin);

    /**
     * @brief Push the current server-side stage state to a single freshly
     *        joined connection so a late joiner is not left with a stale
     *        REST-fetched file. Iterates the prims with authored opinions in
     *        the session (sidecar) layer, unicasts one @c kXformBroadcast per
     *        prim to @p conn, then a terminal @c kSnapshotComplete frame.
     *        Safe to call after AttachClient; takes the session's stage_mutex
     *        briefly for the resolved-transform reads.
     */
    void SendJoinSnapshot(const std::shared_ptr<Session>& session,
                          crow::websocket::connection* conn);

    /**
     * @brief Outcome of a commit request.
     */
    enum class CommitStatus
    {
        Ok,
        UnknownSession,
        NothingToCommit,
        WriteFailed
    };

    /**
     * @brief Merge the session's sidecar overrides back into the original USD
     *        file (preserving composition arcs via UsdUtilsFlattenLayerStack),
     *        then reload other live sessions bound to the same file. Does not
     *        destroy the session. Safe to call from the REST thread.
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
     * @brief Save the session (sidecar) layer of every session currently
     *        flagged dirty, clearing the flag on success. Each save is done
     *        under the session's stage_mutex so it never races client-driven
     *        authoring. Driven by the background SessionFlusher; exposed so
     *        tests can trigger a flush deterministically. Returns the number of
     *        sessions actually saved.
     */
    std::size_t FlushDirtySessions();

private:
    void DestroyLocked(const std::string& id);

    /// Revoke @p session's listener and clear its stage's session layer, the
    /// shared "safe teardown" step used before dropping the last reference.
    /// Mirrors the ~SessionManager teardown to avoid racing the thumbnail
    /// worker's SdfLayer::FindOrOpen on the same on-disk path. Does not take
    /// m_mutex_ itself.
    static void TeardownSession(const std::shared_ptr<Session>& session);

    /// Background reaper loop: periodically calls ReapIdleSessions() until the
    /// manager is destroyed. Only started when m_idle_timeout_ > 0.
    void ReaperRun();

    idtx::utils::UsdFileLocator                                m_locator_;
    std::filesystem::path                                     m_sessions_dir_;
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