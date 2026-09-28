#include "SessionManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
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

#include "concurrency/WorkerExecutor.h"
#include "StageCommitter.h"
#include "StageNoticeListener.h"
#include "TransformDispatcher.h"
#include "utils/Uuid.h"

namespace idtx
{
namespace session
{

namespace
{

// Helper for std::visit over the command variant.
template<typename... Handlers>
struct Overloaded : Handlers...
{
    using Handlers::operator()...;
};

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
/// resolved transform on @p session's stage, stamped with @p server_seq.
/// Returns false when the prim has no readable transform. Shared by the live
/// broadcast and the join snapshot.
bool BuildBroadcastPayload(const Session& session,
                           const std::string& prim_path,
                           std::uint64_t server_seq,
                           std::string& out_payload)
{
    idtxcore::SeparateTransform sep;
    idtxcore::Matrix4dTransform mat;
    bool use_matrix = false;
    if (!ReadResolvedTransform(session.stage, prim_path, sep, mat, use_matrix))
        return false;

    idtxcore::BaseMessage msg;
    msg.set_session_id(session.id);
    msg.set_server_seq(server_seq);
    auto* bcast = msg.mutable_xform_broadcast();
    bcast->set_client_id(""); // server-originated

    auto* upd = bcast->mutable_update();
    upd->set_session_id(session.id);
    upd->set_usd_file(session.usd_file);
    upd->set_prim_path(prim_path);
    upd->set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (use_matrix) *upd->mutable_matrix()   = std::move(mat);
    else            *upd->mutable_seperate() = std::move(sep);

    return msg.SerializeToString(&out_payload);
}

std::string SerializeAck(const Session& session,
                         std::uint64_t request_id,
                         bool ok,
                         const std::string& error)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session.id);
    msg.set_request_id(request_id);
    msg.set_server_seq(session.server_seq.load());
    auto* ack = msg.mutable_ack();
    ack->set_ok(ok);
    if (!ok) ack->set_error(error);

    std::string payload;
    msg.SerializeToString(&payload);
    return payload;
}

} // namespace

SessionManager::SessionManager(idtx::utils::UsdFileLocator file_locator,
                               std::chrono::seconds idle_timeout,
                               std::size_t worker_count)
    : m_executor_(std::make_shared<idtx::concurrency::WorkerExecutor>(
          std::max<std::size_t>(worker_count, 1)))
    , m_locator_(std::move(file_locator))
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

    IDTX_LOG(IDTX_INFO, "Session command executor started with {} worker(s).",
             m_executor_->WorkerCount());

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

    // Take the sessions out of the map first and tear them down without
    // m_mutex_ held: a command that is still draining may call back into the
    // manager (for example a commit that schedules reloads).
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
    {
        std::unique_lock lk(m_mutex_);
        sessions.swap(m_sessions_);
    }

    // Deterministic teardown, per session:
    //   1. Stop admission and let the consumer finish accepted commands.
    //   2. Revoke the StageNoticeListener so no notice can fire against a
    //      half-destroyed Session while we're clearing state.
    //   3. Clear the stage's session layer to drop the overrides before the
    //      last UsdStage refcount goes away. Without this, the USD teardown
    //      path can race with a concurrent SdfLayer::FindOrOpen() on the same
    //      on-disk path from the thumbnail worker: the layer registry may
    //      briefly observe a partially-torn-down layer and hand it back to the
    //      caller, which then crashes when the last strong reference drops.
    //   4. Only then drop the shared_ptrs, releasing the UsdStage handles
    //      and, transitively, the SdfLayers.
    for (auto& [id, session] : sessions)
    {
        (void)id;
        TeardownSession(session);
    }
    sessions.clear();

    // Every session is idle, so no drain task can be pending. Joining here
    // guarantees no worker thread outlives the manager.
    m_executor_->Stop();
}

void SessionManager::TeardownSession(const std::shared_ptr<Session>& session)
{
    if (!session) return;

    if (session->commands)
    {
        session->commands->CloseAdmission();
        session->commands->WaitIdle();
    }
    // From here on this thread has exclusive access to the stage.

    if (session->listener) session->listener->Revoke();

    const pxr::SdfLayerHandle session_layer =
        session->stage ? session->stage->GetSessionLayer() : pxr::SdfLayerHandle();
    const bool has_overrides = session_layer && !session_layer->IsEmpty();

    // Auto-commit on destroy folds the sidecar overrides back into the original
    // file (preserving composition arcs). We deliberately do NOT trigger a
    // ReloadSessionsForFile() here: the session is being dropped anyway.
    bool committed = false;
    if (session->auto_commit && session->stage && has_overrides)
    {
        const CommitResult result = CommitOverrides(*session);
        committed = result.status == CommitStatus::Ok;
        if (!committed)
            IDTX_LOG(IDTX_ERROR,
                     "Auto-commit failed for session {} -> '{}': {}",
                     session->id, session->usd_file, result.error);
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

    // The handler holds a plain pointer: TeardownSession() waits for the
    // consumer to go idle before the session can be released, so the pointer
    // never outlives a running command.
    session->commands = SessionCommandScheduler::Create(
        m_executor_,
        [this, raw = session.get()](SessionCommand&& command)
        {
            ProcessCommand(*raw, std::move(command));
        });

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

    // Teardown (drain, final commit/flush, warnings, sidecar removal,
    // session-layer clear) runs outside m_mutex_ so the map lock is not held
    // across USD and filesystem work.
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

    std::size_t accepted = 0;
    for (const auto& session : sessions)
    {
        if (!session) continue;
        const auto status = Submit(*session, ReloadCommand{});
        if (status == SubmitStatus::Accepted)
        {
            ++accepted;
        }
        else
        {
            IDTX_LOG(IDTX_WARN,
                     "ReloadSessionsForFile: session {} did not accept the reload ({}).",
                     session->id,
                     status == SubmitStatus::QueueFull ? "queue full" : "closing");
        }
    }
    return accepted;
}

// ---------------------------------------------------------------------------
// Websocket-side API
// ---------------------------------------------------------------------------

SessionManager::AttachStatus SessionManager::AttachClient(
    const std::string& session_id,
    const ConnectionId connection_id,
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
    session->clients.insert_or_assign(connection_id, conn);
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
                                  ConnectionId connection_id)
{
    auto session = Get(session_id);
    if (!session) return;
    std::unique_lock lk(session->clients_mutex);
    session->clients.erase(connection_id);
    if (session->clients.empty())
        session->empty_since = std::chrono::steady_clock::now();
    IDTX_LOG(IDTX_INFO, "Client detached from session {} (now {} clients).",
             session_id, session->clients.size());
}

// ---------------------------------------------------------------------------
// Command submission
// ---------------------------------------------------------------------------

SessionManager::SubmitStatus SessionManager::Submit(Session& session, SessionCommand command)
{
    if (!session.commands) return SubmitStatus::SessionClosing;

    switch (session.commands->TrySubmit(std::move(command)))
    {
        case CommandAdmissionStatus::Accepted:  return SubmitStatus::Accepted;
        case CommandAdmissionStatus::QueueFull: return SubmitStatus::QueueFull;
        case CommandAdmissionStatus::Stopping:
        default:                                return SubmitStatus::SessionClosing;
    }
}

SessionManager::SubmitStatus SessionManager::SubmitTransformUpdate(
    const std::string& session_id,
    std::unique_ptr<const idtxcore::TransformUpdate> update,
    ConnectionId origin,
    std::uint64_t request_id) const {
    auto session = Get(session_id);
    if (!session)
    {
        IDTX_LOG(IDTX_WARN, "SubmitTransformUpdate: unknown session {}.", session_id);
        return SubmitStatus::UnknownSession;
    }
    if (!update) return SubmitStatus::Accepted; // nothing to apply

    const auto status = Submit(*session, TransformCommand(std::move(update), origin, request_id));
    if (status == SubmitStatus::QueueFull)
    {
        IDTX_LOG(IDTX_WARN, "Session {} command queue is full; rejecting update.", session_id);
    }
    return status;
}

SessionManager::SubmitStatus SessionManager::RequestJoinSnapshot(
    const std::string& session_id,
    ConnectionId connection_id)
{
    auto session = Get(session_id);
    if (!session) return SubmitStatus::UnknownSession;
    return Submit(*session, JoinCommand{connection_id});
}

// ---------------------------------------------------------------------------
// Command processing (runs on the session's consumer)
// ---------------------------------------------------------------------------

void SessionManager::ProcessCommand(Session& session, SessionCommand&& command)
{
    try
    {
        std::visit([this, &session](auto& cmd) { HandleCommand(session, cmd); }, command);
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "Command failed in session {}: {}", session.id, e.what());
    }
    catch (...)
    {
        IDTX_LOG(IDTX_ERROR, "Command failed in session {} (unknown exception).", session.id);
    }
}

void SessionManager::HandleCommand(Session& session, TransformCommand& command)
{
    bool ok = false;
    session.current_origin.store(command.origin, std::memory_order_relaxed);
    try
    {
        ok = TransformDispatcher::Apply(session.stage, *command.update);
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "TransformUpdate threw in session {}: {}", session.id, e.what());
    }
    session.current_origin.store(0, std::memory_order_relaxed);

    // Mark the session dirty so the SessionFlusher persists the session layer
    // to its sidecar on its next tick.
    if (ok) session.dirty.store(true, std::memory_order_relaxed);

    // The Ack is sent after the broadcasts triggered by this update, and its
    // server_seq reflects the state after the update.
    SendToConnection(session, command.origin,
                     SerializeAck(session, command.request_id, ok, "apply_failed"));
}

void SessionManager::HandleCommand(Session& session, JoinCommand& command)
{
    if (!session.stage) return;

    const std::uint64_t seq = session.server_seq.load();

    std::vector<std::string> frames;
    const pxr::SdfLayerHandle session_layer = session.stage->GetSessionLayer();
    if (session_layer)
    {
        // Gather every prim path with an authored spec in the session
        // (sidecar) layer. These are exactly the prims a late joiner would
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
            if (BuildBroadcastPayload(session, p.GetString(), seq, payload))
                frames.push_back(std::move(payload));
        }
    }

    // Terminal marker: the client now has the full server state as of seq.
    idtxcore::BaseMessage done;
    done.set_session_id(session.id);
    done.set_server_seq(seq);
    done.mutable_snapshot_complete();
    std::string done_payload;
    if (done.SerializeToString(&done_payload)) frames.push_back(std::move(done_payload));

    {
        std::shared_lock lk(session.clients_mutex);
        auto it = session.clients.find(command.connection);
        if (it == session.clients.end()) return; // left before the snapshot ran
        for (const auto& f : frames) it->second->send_binary(f);
    }

    IDTX_LOG(IDTX_INFO, "Sent join snapshot ({} prim frame(s)) to a client of session {}.",
             frames.size() - 1, session.id);
}

void SessionManager::HandleCommand(Session& session, CommitCommand& command)
{
    CommitResult result = CommitOverrides(session);
    if (result.status == CommitStatus::Ok)
    {
        IDTX_LOG(IDTX_INFO, "Committed session {} to '{}'.", session.id, session.usd_file);

        // Let every live session bound to the same file observe the new
        // baseline (mirrors the upload-replacement reload path). Only queues
        // commands, so this is safe from the consumer.
        ReloadSessionsForFile(session.usd_file);
    }
    command.result.set_value(std::move(result));
}

void SessionManager::HandleCommand(Session& session, ReloadCommand& /*command*/)
{
    if (!session.stage) return;

    const pxr::SdfLayerHandle root_layer = session.stage->GetRootLayer();
    if (!root_layer)
    {
        IDTX_LOG(IDTX_WARN, "Reload: session {} has no root layer; skipping.", session.id);
        return;
    }

    std::lock_guard shared_layer_lock(m_shared_layer_mutex_);

    // Signal to StageNoticeListener that the imminent
    // UsdNotice::ObjectsChanged is a reload, not a client-authored change,
    // so the listener can consult the session layer to decide whether the
    // reload is visible to attached clients. The notice fires synchronously
    // when the change block closes, so the flag can be cleared right after.
    session.reload_in_progress.store(true, std::memory_order_relaxed);
    session.current_origin.store(0, std::memory_order_relaxed);

    bool ok = false;
    try
    {
        pxr::SdfChangeBlock block;
        ok = root_layer->Reload(/*force=*/true);
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR,
                 "SdfLayer::Reload threw for session {} ('{}'): {}",
                 session.id, session.usd_file, e.what());
    }
    session.reload_in_progress.store(false, std::memory_order_relaxed);

    if (ok)
        IDTX_LOG(IDTX_INFO, "Reloaded root layer for session {} ('{}').",
                 session.id, session.usd_file);
    else
        IDTX_LOG(IDTX_DEBUG, "SdfLayer::Reload reported no change for session {} ('{}').",
                 session.id, session.usd_file);
}

void SessionManager::HandleCommand(Session& session, FlushCommand& /*command*/)
{
    if (!session.stage) return;
    // Clear the flag before saving so an update applied after this flush is
    // picked up by the next one.
    if (!session.dirty.exchange(false, std::memory_order_relaxed)) return;

    try
    {
        auto sl = session.stage->GetSessionLayer();
        if (sl && !sl->Save())
        {
            IDTX_LOG(IDTX_WARN,
                     "Flush: SdfLayer::Save() reported failure for session {} ('{}').",
                     session.id, session.sidecar_path.string());
            session.dirty.store(true, std::memory_order_relaxed); // retry next tick
        }
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "Flush threw for session {}: {}", session.id, e.what());
        session.dirty.store(true, std::memory_order_relaxed);
    }
}

CommitResult SessionManager::CommitOverrides(Session& session)
{
    CommitResult result;
    if (!session.stage)
    {
        result.status = CommitStatus::UnknownSession;
        result.error  = "unknown session";
        return result;
    }

    const pxr::SdfLayerHandle session_layer = session.stage->GetSessionLayer();
    if (!session_layer || session_layer->IsEmpty())
    {
        result.status = CommitStatus::NothingToCommit;
        result.error  = "no authored overrides to commit";
        return result;
    }

    std::lock_guard shared_layer_lock(m_shared_layer_mutex_);

    // Best-effort persist of the sidecar so its on-disk state matches what
    // we are about to fold into the original file.
    session_layer->Save();
    if (!StageCommitter::Commit(session.stage, session.usd_file_absolute.string(), result.error))
    {
        result.status = CommitStatus::WriteFailed;
        return result;
    }

    session.dirty.store(false, std::memory_order_relaxed);
    result.status = CommitStatus::Ok;
    return result;
}

void SessionManager::SendToConnection(const Session& session,
                                      ConnectionId connection_id,
                                      const std::string& payload)
{
    if (connection_id == 0 || payload.empty()) return;
    std::shared_lock lk(session.clients_mutex);
    auto it = session.clients.find(connection_id);
    if (it != session.clients.end()) it->second->send_binary(payload);
}

// ---------------------------------------------------------------------------
// Broadcast
// ---------------------------------------------------------------------------

void SessionManager::BroadcastResolvedTransform(const Session& session,
                                                const std::string& prim_path,
                                                ConnectionId origin,
                                                std::uint64_t server_seq)
{
    if (!session.stage) return;

    std::string payload;
    if (!BuildBroadcastPayload(session, prim_path, server_seq, payload))
    {
        IDTX_LOG(IDTX_DEBUG, "Skip broadcast: cannot read transform on '{}'.", prim_path);
        return;
    }

    // send_binary only queues the frame on the connection's io thread, so it
    // is cheap to call under the shared lock, and holding the lock keeps a
    // concurrently closing connection registered until the call returns.
    std::shared_lock lk(session.clients_mutex);
    for (const auto& [id, conn] : session.clients)
    {
        if (id != origin) conn->send_binary(payload);
    }
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

    CommitCommand command;
    std::future<CommitResult> future = command.result.get_future();

    const auto status = Submit(*session, std::move(command));
    if (status != SubmitStatus::Accepted)
    {
        out_error = status == SubmitStatus::QueueFull
            ? "session command queue is full"
            : "session is closing";
        return CommitStatus::Unavailable;
    }

    if (future.wait_for(kCommitTimeout) != std::future_status::ready)
    {
        // The commit stays queued and still runs; only the caller gives up.
        out_error = "commit did not complete in time";
        IDTX_LOG(IDTX_WARN, "Commit of session {} timed out after {}s.",
                 session_id, static_cast<long long>(kCommitTimeout.count()));
        return CommitStatus::Unavailable;
    }

    try
    {
        CommitResult result = future.get();
        out_error = std::move(result.error);
        return result.status;
    }
    catch (const std::future_error&)
    {
        // The handler failed before producing a result; it logged the cause.
        out_error = "commit failed";
        return CommitStatus::WriteFailed;
    }
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
                // emptiness reading below is stable through the erase (no
                // check-then-act race with a client reconnecting).
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
    std::size_t submitted = 0;
    for (const auto& session : sessions)
    {
        // Only queue work when there is something to save. The consumer
        // re-checks and clears the flag.
        if (!session || !session->dirty.load(std::memory_order_relaxed)) continue;
        if (Submit(*session, FlushCommand{}) == SubmitStatus::Accepted) ++submitted;
    }
    return submitted;
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
