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

#include <memory>
#include <shared_mutex>
#include <string>
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

    explicit SessionManager(idtx::utils::UsdFileLocator file_locator);
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
                                    CreateStatus& out_status);

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
     *      composition strength (see §3.4 of
     *      docs/plans/reload-on-upload.md).
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

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    /// Build a JSON description of @p session that is safe to return from REST.
    static nlohmann::json ToJson(const Session& session);

    const idtx::utils::UsdFileLocator& GetLocator() const noexcept { return m_locator_; }

private:
    void DestroyLocked(const std::string& id);

    idtx::utils::UsdFileLocator                                m_locator_;
    mutable std::shared_mutex                                  m_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>>  m_sessions_;
};

} // namespace session
} // namespace idtx