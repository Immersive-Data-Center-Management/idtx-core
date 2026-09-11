#include "SessionManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <set>
#include <utility>
#include <vector>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>

#include <idtx/proto/base.pb.h>
#include <idtx/proto/transform.pb.h>

#include "StageCommitter.h"
#include "StageNoticeListener.h"
#include "TransformDispatcher.h"
#include "utils/Uuid.h"

namespace idtx
{
namespace session
{

SessionManager::SessionManager(idtx::utils::UsdFileLocator file_locator,
                               std::chrono::seconds idle_timeout)
    : m_locator_(std::move(file_locator))
    , m_sessions_dir_(std::filesystem::path(m_locator_.GetSessionRoot()))
    , m_idle_timeout_(idle_timeout)
{
    // Per-session sidecar layers live under "<sessions_root>/". Create
    // it up front so Create() can drop sidecars without a per-call mkdir race.
    std::error_code ec;
    std::filesystem::create_directories(m_sessions_dir_, ec);
    if (ec)
    {
        IDTX_LOG(IDTX_WARN,
                 "Could not create sessions dir '{}': {}. Sidecar persistence may fail.",
                 m_sessions_dir_.string(), ec.message());
    }

    if (m_idle_timeout_ > std::chrono::seconds{0})
    {
        m_reaper_thread_ = std::thread(&SessionManager::ReaperRun, this);
        IDTX_LOG(IDTX_INFO,
                 "Idle-session reaper enabled (timeout {}s).",
                 static_cast<long long>(m_idle_timeout_.count()));
    }
}

SessionManager::~SessionManager()
{
    // Stop the reaper thread before touching m_sessions_ so it cannot race the
    // teardown below. Mirrors the ThumbnailWorker shutdown handshake.
    {
        std::lock_guard<std::mutex> lk(m_reaper_mutex_);
        m_reaper_stop_ = true;
    }
    m_reaper_cv_.notify_all();
    if (m_reaper_thread_.joinable()) m_reaper_thread_.join();

    // Deterministic teardown:
    //   1. Revoke every StageNoticeListener first, so no notice can fire
    //      against a half-destroyed Session while we're clearing state.
    //   2. Clear each stage's session layer to drop the anonymous overrides
    //      before the last UsdStage refcount goes away. Without this, the
    //      USD teardown path can race with a concurrent
    //      SdfLayer::FindOrOpen() on the same on-disk path from the
    //      thumbnail worker: the layer registry may briefly observe a
    //      partially-torn-down layer and hand it back to the caller, which
    //      then crashes when the last strong reference drops.
    //   3. Only then drop the shared_ptrs, releasing the UsdStage handles
    //      and, transitively, the SdfLayers.
    std::unique_lock lk(m_mutex_);
    for (auto& [id, session] : m_sessions_)
    {
        (void)id;
        TeardownSession(session);
    }
    m_sessions_.clear();
}

void SessionManager::TeardownSession(const std::shared_ptr<Session>& session)
{
    if (!session) return;
    if (session->listener) session->listener->Revoke();

    const pxr::SdfLayerHandle session_layer =
        session->stage ? session->stage->GetSessionLayer() : pxr::SdfLayerHandle();
    const bool has_overrides = session_layer && !session_layer->IsEmpty();

    // Auto-commit on destroy folds the sidecar overrides back into the original
    // file (preserving composition arcs). We deliberately do NOT trigger a
    // ReloadSessionsForFile() here: teardown may run from ~SessionManager while
    // m_mutex_ is held, and the session is being dropped anyway.
    bool committed = false;
    if (session->auto_commit && session->stage && has_overrides)
    {
        std::lock_guard lk(session->stage_mutex);
        if (auto sl = session->stage->GetSessionLayer()) sl->Save(); // best-effort flush
        std::string err;
        committed = StageCommitter::Commit(
            session->stage, session->usd_file_absolute.string(), err);
        if (!committed)
            IDTX_LOG(IDTX_ERROR,
                     "Auto-commit failed for session {} -> '{}': {}",
                     session->id, session->usd_file, err);
    }

    // Make data-loss events visible in monitoring before we drop the state.
    if (has_overrides && !committed)
        IDTX_LOG(IDTX_WARN,
                 "Session {} destroyed with uncommitted overrides in sidecar '{}'. "
                 "Changes have NOT been merged back to original stage '{}'.",
                 session->id, session->sidecar_path.string(), session->usd_file);
    if (session->dirty.load(std::memory_order_relaxed))
        IDTX_LOG(IDTX_WARN,
                 "Session {} had in-memory changes not yet flushed to sidecar at teardown.",
                 session->id);

    if (session->stage)
    {
        try
        {
            // Clear the session layer before the last UsdStage refcount goes
            // away. Without this, USD teardown can race with a concurrent
            // SdfLayer::FindOrOpen() on the same on-disk path from the
            // thumbnail worker and hand back a partially-torn-down layer.
            if (auto sl = session->stage->GetSessionLayer())
            {
                sl->Clear();
            }
        }
        catch (const std::exception& e)
        {
            IDTX_LOG(IDTX_WARN,
                     "TeardownSession: failed to clear session layer for {}: {}",
                     session->id, e.what());
        }
        catch (...)
        {
            IDTX_LOG(IDTX_WARN,
                     "TeardownSession: failed to clear session layer for {} (unknown exception).",
                     session->id);
        }
    }

    // Remove the on-disk sidecar so per-session files do not accumulate after
    // reaping. Best-effort; a failure is logged but not fatal.
    if (!session->sidecar_path.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(session->sidecar_path, ec))
        {
            std::filesystem::remove(session->sidecar_path, ec);
            if (ec)
                IDTX_LOG(IDTX_ERROR, "Failed to remove sidecar '{}': {}",
                         session->sidecar_path.string(), ec.message());
        }
    }
}

// ---------------------------------------------------------------------------
// REST-side API
// ---------------------------------------------------------------------------

std::shared_ptr<Session> SessionManager::Create(const std::string& usd_file,
                                                idtx::dto::SessionMode mode,
                                                std::string& out_error,
                                                CreateStatus& out_status,
                                                bool auto_commit)
{
    std::filesystem::path resolved;
    auto vstatus = m_locator_.Resolve(usd_file, resolved);
    if (vstatus != idtx::utils::UsdFileLocator::Status::Ok)
    {
        out_error  = std::string("USD file not found or invalid: ")
                   + idtx::utils::UsdFileLocator::StatusToString(vstatus);
        out_status = CreateStatus::FileNotFound;
        IDTX_LOG(IDTX_WARN, "Session creation rejected ({}) for '{}'.",
                 idtx::utils::UsdFileLocator::StatusToString(vstatus), usd_file);
        return nullptr;
    }

    const std::string session_id  = idtx::utils::GenerateUuidV4();
    const std::filesystem::path sidecar_path =
        m_sessions_dir_ / (session_id + ".usda");

    // Open the root layer and a named sidecar layer, then open the stage with
    // the sidecar as its session layer. The sidecar IS the composition's
    // override layer: all client edits (authored into the session layer via
    // TransformDispatcher) land in this on-disk file, so persistence is a
    // plain SdfLayer::Save() and a crashed/restarted session resumes simply by
    // re-opening the same sidecar. If a sidecar from a previous run exists we
    // reopen it (state resume); otherwise we create a fresh one.
    pxr::SdfLayerRefPtr root_layer = pxr::SdfLayer::FindOrOpen(resolved.string());
    if (!root_layer)
    {
        out_error  = "Failed to open USD root layer";
        out_status = CreateStatus::StageOpenFailed;
        IDTX_LOG(IDTX_ERROR, "SdfLayer::FindOrOpen returned null for '{}'.", resolved.string());
        return nullptr;
    }

    const bool resuming = std::filesystem::exists(sidecar_path);
    pxr::SdfLayerRefPtr sidecar_layer = resuming
        ? pxr::SdfLayer::FindOrOpen(sidecar_path.string())
        : pxr::SdfLayer::CreateNew(sidecar_path.string());
    if (!sidecar_layer)
    {
        out_error  = "Failed to open session sidecar layer";
        out_status = CreateStatus::StageOpenFailed;
        IDTX_LOG(IDTX_ERROR, "Could not {} sidecar layer '{}'.",
                 resuming ? "reopen" : "create", sidecar_path.string());
        return nullptr;
    }

    pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(root_layer, sidecar_layer);
    if (!stage)
    {
        out_error  = "Failed to open USD stage";
        out_status = CreateStatus::StageOpenFailed;
        IDTX_LOG(IDTX_ERROR, "UsdStage::Open returned null for '{}'.", resolved.string());
        return nullptr;
    }

    // Redirect all subsequent stage authoring (e.g. TransformDispatcher::Apply)
    // to the session layer (the named sidecar) so client edits compose *above*
    // the root layer. This gives us:
    //   1. `SdfLayer::Reload(force=true)` on the root layer (used by
    //      ReloadSessionsForFile when a client uploads a replacement file)
    //      does not clobber live session-authored overrides.
    //   2. StageNoticeListener can decide whether the on-disk change is
    //      visible to the client by inspecting the session layer directly.
    //   3. Persistence and resume come for free from the named sidecar.
    stage->SetEditTarget(pxr::UsdEditTarget(stage->GetSessionLayer()));

    auto session               = std::make_shared<Session>();
    session->id                = session_id;
    session->usd_file          = usd_file;
    session->usd_file_absolute = resolved;
    session->stage             = stage;
    session->mode              = mode;
    session->auto_commit       = auto_commit;
    session->sidecar_path      = sidecar_path;
    session->created_at        = std::chrono::system_clock::now();

    session->listener = std::make_shared<StageNoticeListener>(this, std::weak_ptr<Session>(session));
    session->listener->Register();

    {
        std::unique_lock lk(m_mutex_);
        m_sessions_.emplace(session->id, session);
    }

    out_status = CreateStatus::Ok;
    IDTX_LOG(IDTX_INFO, "Created session {} for '{}'{}.",
             session->id, usd_file, resuming ? " (resumed from sidecar)" : "");
    return session;
}

std::shared_ptr<Session> SessionManager::Get(const std::string& id) const
{
    std::shared_lock lk(m_mutex_);
    auto it = m_sessions_.find(id);
    return it != m_sessions_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<Session>> SessionManager::List() const
{
    std::shared_lock lk(m_mutex_);
    std::vector<std::shared_ptr<Session>> result;
    result.reserve(m_sessions_.size());
    for (auto& [id, session] : m_sessions_) result.push_back(session);
    return result;
}

bool SessionManager::Exists(const std::string& id) const
{
    std::shared_lock lk(m_mutex_);
    return m_sessions_.find(id) != m_sessions_.end();
}

bool SessionManager::Destroy(const std::string& id)
{
    std::shared_ptr<Session> victim;
    {
        std::unique_lock lk(m_mutex_);
        auto it = m_sessions_.find(id);
        if (it == m_sessions_.end()) return false;
        victim = it->second;
        m_sessions_.erase(it);
    }

    // Teardown (final commit/flush, warnings, sidecar removal, session-layer
    // clear) runs outside m_mutex_ so the map lock is not held across USD and
    // filesystem work.
    TeardownSession(victim);
    IDTX_LOG(IDTX_INFO, "Destroyed session {}.", id);
    return true;
}

std::vector<std::shared_ptr<Session>>
SessionManager::FindByUsdFile(const std::string& usd_file) const
{
    std::vector<std::shared_ptr<Session>> matches;
    std::shared_lock lk(m_mutex_);
    matches.reserve(m_sessions_.size());
    for (const auto& [id, session] : m_sessions_)
    {
        if (session && session->usd_file == usd_file)
            matches.push_back(session);
    }
    return matches;
}

std::size_t SessionManager::ReloadSessionsForFile(const std::string& usd_file)
{
    const auto sessions = FindByUsdFile(usd_file);
    if (sessions.empty())
    {
        IDTX_LOG(IDTX_DEBUG,
                 "ReloadSessionsForFile('{}'): no live sessions to reload.",
                 usd_file);
        return 0;
    }

    std::size_t reloaded = 0;
    for (const auto& session : sessions)
    {
        if (!session || !session->stage) continue;

        const pxr::SdfLayerHandle root_layer = session->stage->GetRootLayer();
        if (!root_layer)
        {
            IDTX_LOG(IDTX_WARN,
                     "ReloadSessionsForFile: session {} has no root layer; skipping.",
                     session->id);
            continue;
        }

        {
            // Serialise against ApplyTransformUpdate so the reload notice
            // fires atomically w.r.t. client-driven authoring.
            std::lock_guard lk(session->stage_mutex);

            // Signal to StageNoticeListener that the imminent
            // UsdNotice::ObjectsChanged is a reload, not a client-authored
            // change, so the listener can consult the session layer to
            // decide whether the reload is visible to attached clients.
            session->reload_in_progress = true;
            // No client-authored change to attribute here; make sure the
            // "suppress echo to origin" mechanism cannot spuriously fire.
            session->last_origin        = nullptr;

            bool ok = false;
            try
            {
                pxr::SdfChangeBlock block;
                ok = root_layer->Reload(/*force=*/true);
            }
            catch (const std::exception& e)
            {
                session->reload_in_progress = false;
                IDTX_LOG(IDTX_ERROR,
                         "SdfLayer::Reload threw for session {} ('{}'): {}",
                         session->id, usd_file, e.what());
                continue;
            }

            if (!ok)
            {
                // Reload can legitimately return false when the file hasn't
                // changed on disk. In that case no notice will fire, so we
                // must clear the flag ourselves.
                session->reload_in_progress = false;
                IDTX_LOG(IDTX_DEBUG,
                         "SdfLayer::Reload reported no change for session {} ('{}').",
                         session->id, usd_file);
                continue;
            }
        }

        ++reloaded;
        IDTX_LOG(IDTX_INFO,
                 "Reloaded root layer for session {} ('{}').",
                 session->id, usd_file);
    }
    return reloaded;
}

// ---------------------------------------------------------------------------
// Websocket-side API
// ---------------------------------------------------------------------------

SessionManager::AttachStatus SessionManager::AttachClient(
    const std::string& session_id,
    crow::websocket::connection* conn)
{
    auto session = Get(session_id);
    if (!session)
    {
        IDTX_LOG(IDTX_WARN, "AttachClient: unknown session {}.", session_id);
        return AttachStatus::UnknownSession;
    }
    std::unique_lock lk(session->clients_mutex);
    if (session->mode == idtx::dto::SessionMode::SingleEdit
        && !session->clients.empty())
    {
        IDTX_LOG(IDTX_WARN,
                 "AttachClient: rejecting second editor on single_edit session {}.",
                 session_id);
        return AttachStatus::SingleEditBusy;
    }
    session->clients.insert(conn);
    session->empty_since.reset();
    IDTX_LOG(IDTX_INFO, "Client attached to session {} (now {} clients).",
             session_id, session->clients.size());
    return AttachStatus::Ok;
}

bool SessionManager::IsSingleEditBusy(const std::string& session_id) const
{
    auto session = Get(session_id);
    if (!session) return false;
    if (session->mode != idtx::dto::SessionMode::SingleEdit) return false;
    std::shared_lock lk(session->clients_mutex);
    return !session->clients.empty();
}

void SessionManager::DetachClient(const std::string& session_id,
                                  crow::websocket::connection* conn)
{
    auto session = Get(session_id);
    if (!session) return;
    std::unique_lock lk(session->clients_mutex);
    session->clients.erase(conn);
    if (session->clients.empty())
        session->empty_since = std::chrono::steady_clock::now();
    IDTX_LOG(IDTX_INFO, "Client detached from session {} (now {} clients).",
             session_id, session->clients.size());
}

// ---------------------------------------------------------------------------
// Stage authoring entrypoint
// ---------------------------------------------------------------------------

bool SessionManager::ApplyTransformUpdate(const std::string& session_id,
                                          const idtxcore::TransformUpdate& upd,
                                          crow::websocket::connection* origin)
{
    auto session = Get(session_id);
    if (!session)
    {
        IDTX_LOG(IDTX_WARN, "ApplyTransformUpdate: unknown session {}.", session_id);
        return false;
    }

    bool ok = false;
    {
        std::lock_guard lk(session->stage_mutex);
        session->last_origin = origin;
        ok = TransformDispatcher::Apply(session->stage, upd);
        if (!ok) session->last_origin = nullptr;
    }
    // Mark the session dirty so the SessionFlusher persists the session layer
    // to its sidecar on its next tick. Set outside the stage_mutex critical
    // section (atomic) — the flusher takes the mutex itself when it saves.
    if (ok) session->dirty.store(true, std::memory_order_relaxed);
    return ok;
}

// ---------------------------------------------------------------------------
// Broadcast
// ---------------------------------------------------------------------------

namespace
{

bool ReadResolvedTransform(const pxr::UsdStageRefPtr& stage,
                           const std::string& prim_path_str,
                           idtxcore::SeparateTransform& out_sep,
                           idtxcore::Matrix4dTransform& out_mat,
                           bool& out_use_matrix)
{
    if (!stage || !pxr::SdfPath::IsValidPathString(prim_path_str)) return false;
    pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(prim_path_str));
    if (!prim) return false;

    pxr::UsdGeomXformable xformable(prim);
    if (!xformable) return false;

    pxr::UsdGeomXformCommonAPI api(prim);
    if (api)
    {
        pxr::GfVec3d t;
        pxr::GfVec3f r;
        pxr::GfVec3f s;
        pxr::GfVec3f pivot;
        pxr::UsdGeomXformCommonAPI::RotationOrder order;
        if (api.GetXformVectorsByAccumulation(&t, &r, &s, &pivot, &order,
                                              pxr::UsdTimeCode::Default()))
        {
            out_sep.mutable_translation()->set_x(t[0]);
            out_sep.mutable_translation()->set_y(t[1]);
            out_sep.mutable_translation()->set_z(t[2]);
            out_sep.mutable_rotation()->set_x(r[0]);
            out_sep.mutable_rotation()->set_y(r[1]);
            out_sep.mutable_rotation()->set_z(r[2]);
            out_sep.mutable_scale()->set_x(s[0]);
            out_sep.mutable_scale()->set_y(s[1]);
            out_sep.mutable_scale()->set_z(s[2]);
            out_use_matrix = false;
            return true;
        }
    }

    pxr::GfMatrix4d local(1.0);
    bool resets = false;
    if (!xformable.GetLocalTransformation(&local, &resets, pxr::UsdTimeCode::Default()))
        return false;

    out_mat.set_m00(local[0][0]); out_mat.set_m01(local[0][1]); out_mat.set_m02(local[0][2]); out_mat.set_m03(local[0][3]);
    out_mat.set_m10(local[1][0]); out_mat.set_m11(local[1][1]); out_mat.set_m12(local[1][2]); out_mat.set_m13(local[1][3]);
    out_mat.set_m20(local[2][0]); out_mat.set_m21(local[2][1]); out_mat.set_m22(local[2][2]); out_mat.set_m23(local[2][3]);
    out_mat.set_m30(local[3][0]); out_mat.set_m31(local[3][1]); out_mat.set_m32(local[3][2]); out_mat.set_m33(local[3][3]);
    out_use_matrix = true;
    return true;
}

/// Build a serialized BaseMessage(xform_broadcast) for @p prim_path from the
/// resolved transform on @p session's stage. Returns false when the prim has no
/// readable transform. Shared by the live broadcast and the join snapshot.
bool BuildBroadcastPayload(const std::shared_ptr<Session>& session,
                           const std::string& prim_path,
                           std::string& out_payload)
{
    idtxcore::SeparateTransform sep;
    idtxcore::Matrix4dTransform mat;
    bool use_matrix = false;
    if (!ReadResolvedTransform(session->stage, prim_path, sep, mat, use_matrix))
        return false;

    idtxcore::BaseMessage msg;
    msg.set_session_id(session->id);
    auto* bcast = msg.mutable_xform_broadcast();
    bcast->set_client_id(""); // server-originated

    auto* upd = bcast->mutable_update();
    upd->set_session_id(session->id);
    upd->set_usd_file(session->usd_file);
    upd->set_prim_path(prim_path);
    upd->set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (use_matrix) *upd->mutable_matrix()   = std::move(mat);
    else            *upd->mutable_seperate() = std::move(sep);

    return msg.SerializeToString(&out_payload);
}

} // namespace

void SessionManager::BroadcastResolvedTransform(const std::shared_ptr<Session>& session,
                                                const std::string& prim_path,
                                                crow::websocket::connection* origin)
{
    if (!session || !session->stage) return;

    std::string payload;
    if (!BuildBroadcastPayload(session, prim_path, payload))
    {
        IDTX_LOG(IDTX_DEBUG, "Skip broadcast: cannot read transform on '{}'.", prim_path);
        return;
    }

    // Snapshot the client list under shared lock so we don't keep the lock
    // while sending (which could deadlock with detach paths).
    std::vector<crow::websocket::connection*> targets;
    {
        std::shared_lock lk(session->clients_mutex);
        targets.reserve(session->clients.size());
        for (auto* c : session->clients)
        {
            if (c != origin) targets.push_back(c);
        }
    }
    for (auto* c : targets) c->send_binary(payload);
}

void SessionManager::SendJoinSnapshot(const std::shared_ptr<Session>& session,
                                      crow::websocket::connection* conn)
{
    if (!session || !session->stage || !conn) return;

    // Collect the serialized broadcast frames under stage_mutex (the reads
    // touch the composed stage), then send outside the lock so a slow socket
    // write cannot stall concurrent authoring.
    std::vector<std::string> frames;
    {
        std::lock_guard lk(session->stage_mutex);
        const pxr::SdfLayerHandle session_layer = session->stage->GetSessionLayer();
        if (session_layer)
        {
            // Gather every prim path with an authored spec in the session
            // (sidecar) layer — these are exactly the prims a late joiner would
            // otherwise miss, since they are absent from the on-disk root file.
            std::set<pxr::SdfPath> prim_paths;
            session_layer->Traverse(
                pxr::SdfPath::AbsoluteRootPath(),
                [&prim_paths](const pxr::SdfPath& p)
                {
                    if (p.IsPrimPath())          prim_paths.insert(p);
                    else if (p.IsPropertyPath()) prim_paths.insert(p.GetPrimPath());
                });

            for (const auto& p : prim_paths)
            {
                std::string payload;
                if (BuildBroadcastPayload(session, p.GetString(), payload))
                    frames.push_back(std::move(payload));
            }
        }
    }

    for (const auto& f : frames) conn->send_binary(f);

    // Terminal marker: the client now has the full current server state.
    idtxcore::BaseMessage done;
    done.set_session_id(session->id);
    done.mutable_snapshot_complete();
    std::string done_payload;
    if (done.SerializeToString(&done_payload))
        conn->send_binary(done_payload);

    IDTX_LOG(IDTX_INFO, "Sent join snapshot ({} prim frame(s)) to a client of session {}.",
             frames.size(), session->id);
}

SessionManager::CommitStatus SessionManager::CommitSession(const std::string& session_id,
                                                           std::string& out_error)
{
    auto session = Get(session_id);
    if (!session || !session->stage)
    {
        out_error = "unknown session";
        return CommitStatus::UnknownSession;
    }

    bool committed = false;
    {
        std::lock_guard lk(session->stage_mutex);
        const pxr::SdfLayerHandle session_layer = session->stage->GetSessionLayer();
        if (!session_layer || session_layer->IsEmpty())
        {
            out_error = "no authored overrides to commit";
            return CommitStatus::NothingToCommit;
        }
        // Best-effort persist of the sidecar so its on-disk state matches what
        // we are about to fold into the original file.
        session_layer->Save();
        committed = StageCommitter::Commit(
            session->stage, session->usd_file_absolute.string(), out_error);
    }

    if (!committed) return CommitStatus::WriteFailed;

    session->dirty.store(false, std::memory_order_relaxed);

    // Let other live sessions bound to the same file observe the new baseline
    // (mirrors the upload-replacement reload path). Runs without the current
    // session's stage_mutex held, so no self-deadlock.
    ReloadSessionsForFile(session->usd_file);

    IDTX_LOG(IDTX_INFO, "Committed session {} to '{}'.", session_id, session->usd_file);
    return CommitStatus::Ok;
}

// ---------------------------------------------------------------------------
// JSON serialization helper for REST responses
// ---------------------------------------------------------------------------

nlohmann::json SessionManager::ToJson(const Session& session)
{
    std::size_t client_count = 0;
    {
        std::shared_lock lk(session.clients_mutex);
        client_count = session.clients.size();
    }
    auto created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        session.created_at.time_since_epoch()).count();
    // Reuse the DTO's JSON serialization for SessionMode so the field name
    // and string values stay in one place (single_edit / collaborative_edit
    // / ...).
    nlohmann::json mode_j = session.mode;
    return nlohmann::json{
        {"session_id",   session.id},
        {"usd_file",     session.usd_file},
        {"mode",         mode_j},
        {"client_count", client_count},
        {"created_at",   created_ms},
        {"ws_url",       std::string("/ws?sid=") + session.id},
        {"protocol",     "protobuf-binary"}
    };
}

void SessionManager::DestroyLocked(const std::string& id)
{
    auto it = m_sessions_.find(id);
    if (it == m_sessions_.end()) return;
    if (it->second && it->second->listener) it->second->listener->Revoke();
    m_sessions_.erase(it);
}

// ---------------------------------------------------------------------------
// Idle-session reaper
// ---------------------------------------------------------------------------

std::size_t SessionManager::ReapIdleSessions()
{
    if (m_idle_timeout_ <= std::chrono::seconds{0}) return 0;

    const auto now = std::chrono::steady_clock::now();

    // Collect the reaped sessions so their teardown (and the drop of the last
    // shared_ptr) happens *after* we release m_mutex_, keeping the map lock
    // hold short and away from USD teardown work.
    std::vector<std::shared_ptr<Session>> reaped;
    {
        std::unique_lock lk(m_mutex_);
        for (auto it = m_sessions_.begin(); it != m_sessions_.end(); )
        {
            const auto& session = it->second;
            bool idle = false;
            if (session)
            {
                // Holding m_mutex_ exclusively means no AttachClient/DetachClient
                // can pass Get() (which needs a shared lock on m_mutex_), so the
                // emptiness reading below is stable through the erase — no
                // check-then-act race with a client reconnecting.
                std::shared_lock clk(session->clients_mutex);
                idle = session->clients.empty()
                       && session->empty_since
                       && (now - *session->empty_since) >= m_idle_timeout_;
            }

            if (idle)
            {
                IDTX_LOG(IDTX_INFO,
                         "Reaping idle session {} (no clients for >= {}s).",
                         it->first,
                         static_cast<long long>(m_idle_timeout_.count()));
                reaped.push_back(session);
                it = m_sessions_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (const auto& session : reaped)
    {
        TeardownSession(session);
    }
    return reaped.size();
}

std::size_t SessionManager::FlushDirtySessions()
{
    const auto sessions = List();
    std::size_t saved = 0;
    for (const auto& session : sessions)
    {
        if (!session || !session->stage) continue;
        // Only exchange (and pay the Save cost) when there is pending work.
        // Clear the flag *before* saving so a concurrent author that sets it
        // again during the save is not lost: it will be picked up next tick.
        if (!session->dirty.exchange(false, std::memory_order_relaxed))
            continue;

        try
        {
            std::lock_guard lk(session->stage_mutex);
            if (auto sl = session->stage->GetSessionLayer())
            {
                if (sl->Save()) ++saved;
                else
                {
                    IDTX_LOG(IDTX_WARN,
                             "Flush: SdfLayer::Save() reported failure for session {} ('{}').",
                             session->id, session->sidecar_path.string());
                    session->dirty.store(true, std::memory_order_relaxed); // retry next tick
                }
            }
        }
        catch (const std::exception& e)
        {
            IDTX_LOG(IDTX_ERROR, "Flush threw for session {}: {}", session->id, e.what());
            session->dirty.store(true, std::memory_order_relaxed);
        }
    }
    return saved;
}

void SessionManager::ReaperRun()
{
    // Sweep often enough that shutdown is prompt and the effective deletion
    // delay stays close to the configured timeout, but never busy-loop.
    const auto interval =
        std::min<std::chrono::seconds>(m_idle_timeout_, std::chrono::seconds{30});
    const auto sweep = std::max<std::chrono::seconds>(interval, std::chrono::seconds{1});

    std::unique_lock<std::mutex> lk(m_reaper_mutex_);
    while (!m_reaper_stop_)
    {
        m_reaper_cv_.wait_for(lk, sweep, [this] { return m_reaper_stop_.load(); });
        if (m_reaper_stop_) break;

        // Run the sweep without holding m_reaper_mutex_ so a concurrent
        // shutdown can still signal us promptly.
        lk.unlock();
        try
        {
            ReapIdleSessions();
        }
        catch (const std::exception& e)
        {
            IDTX_LOG(IDTX_ERROR, "Idle-session reaper sweep threw: {}", e.what());
        }
        catch (...)
        {
            IDTX_LOG(IDTX_ERROR, "Idle-session reaper sweep threw unknown exception.");
        }
        lk.lock();
    }
}

} // namespace session
} // namespace idtx
