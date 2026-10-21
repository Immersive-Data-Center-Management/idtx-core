#include "SessionManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
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
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>

#include <idtx/proto/base.pb.h>
#include <idtx/proto/transform.pb.h>
#include <idtx/proto/prim.pb.h>

#include "concurrency/SharedLayerLock.h"
#include "concurrency/WorkerExecutor.h"
#include "AuthoringContext.h"
#include "PrimDispatcher.h"
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

/// Build a BaseMessage(xform_broadcast) for @p prim_path from the resolved
/// Assemble a BaseMessage(xform_broadcast) for @p prim_path from already-read
/// transform values (no server_seq). The live path reads them off the stage;
/// the join snapshot reads them straight from the session layer via Sdf.
idtxcore::BaseMessage AssembleXformBroadcast(const Session& session,
                                             const std::string& prim_path,
                                             idtxcore::SeparateTransform sep,
                                             idtxcore::Matrix4dTransform mat,
                                             bool use_matrix)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session.id);
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

    return msg;
}

/// transform on @p session's stage, without a server_seq. Returns nullopt when
/// the prim has no readable transform.
std::optional<idtxcore::BaseMessage> BuildBroadcastMessage(const Session& session,
                                                           const std::string& prim_path)
{
    idtxcore::SeparateTransform sep;
    idtxcore::Matrix4dTransform mat;
    bool use_matrix = false;
    if (!ReadResolvedTransform(session.stage, prim_path, sep, mat, use_matrix))
        return std::nullopt;

    return AssembleXformBroadcast(session, prim_path, std::move(sep), std::move(mat), use_matrix);
}

/// Serialized BuildBroadcastMessage() stamped with @p server_seq. Shared by
/// the live broadcast and the join snapshot.
bool BuildBroadcastPayload(const Session& session,
                           const std::string& prim_path,
                           std::uint64_t server_seq,
                           std::string& out_payload)
{
    std::optional<idtxcore::BaseMessage> msg = BuildBroadcastMessage(session, prim_path);
    if (!msg) return false;
    msg->set_server_seq(server_seq);
    return msg->SerializeToString(&out_payload);
}

// Build a serialized BaseMessage(create_prim_broadcast) describing @p prim
// (created under @p parent_path with the given @p type) stamped with
// @p server_seq. Only the attribute fields that apply to the type are set.
std::string SerializeCreatePrimBroadcast(const Session& session,
                                         const std::string& parent_path,
                                         idtxcore::PrimType type,
                                         const CreatedPrim& prim,
                                         std::uint64_t server_seq)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session.id);
    msg.set_server_seq(server_seq);

    auto* bcast = msg.mutable_create_prim_broadcast();
    bcast->set_parent_path(parent_path);
    bcast->set_prim_name(prim.name);
    bcast->set_type(type);
    if (prim.has_size)   bcast->set_size(prim.size);
    if (prim.has_height) bcast->set_height(prim.height);
    if (prim.has_radius) bcast->set_radius(prim.radius);
    if (prim.has_axis)   bcast->set_axis(prim.axis);

    std::string payload;
    msg.SerializeToString(&payload);
    return payload;
}

// Build a serialized BaseMessage(set_prim_active_broadcast) for @p prim_path
// stamped with @p server_seq. Announces a soft delete (@p active == false) or
// its undo. Shared by the live broadcast and the join snapshot.
std::string SerializeSetPrimActiveBroadcast(const Session& session,
                                            const std::string& prim_path,
                                            bool active,
                                            std::uint64_t server_seq)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session.id);
    msg.set_server_seq(server_seq);

    auto* bcast = msg.mutable_set_prim_active_broadcast();
    bcast->set_prim_path(prim_path);
    bcast->set_active(active);

    std::string payload;
    msg.SerializeToString(&payload);
    return payload;
}

std::string SerializeAck(const Session& session,
                         std::uint64_t request_id,
                         std::uint64_t server_seq,
                         bool ok,
                         const char* error)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session.id);
    msg.set_request_id(request_id);
    msg.set_server_seq(server_seq);
    auto* ack = msg.mutable_ack();
    ack->set_ok(ok);
    if (!ok) ack->set_error(error);

    std::string payload;
    msg.SerializeToString(&payload);
    return payload;
}

SessionManager::SubmitStatus ToSubmitStatus(const CommandAdmissionStatus status)
{
    switch (status)
    {
        case CommandAdmissionStatus::Accepted:  return SessionManager::SubmitStatus::Accepted;
        case CommandAdmissionStatus::QueueFull: return SessionManager::SubmitStatus::QueueFull;
        case CommandAdmissionStatus::Stopping:
        default:                                return SessionManager::SubmitStatus::SessionClosing;
    }
}

/// True if the prim at @p path, which the session's own layer has specs for,
/// is no longer part of @p stage because the file defining it no longer does,
/// for example after another session committed its deletion. Only the
/// session's own `over` is left then, or nothing at all. A prim merely pruned
/// below an inactive (soft-deleted) ancestor is not removed: its specs must
/// stay so that undoing the delete restores it.
bool IsRemovedFromStage(const pxr::UsdStageRefPtr& stage, const pxr::SdfPath& path)
{
    if (!stage) return false;
    if (const pxr::UsdPrim prim = stage->GetPrimAtPath(path)) return !prim.IsDefined();

    // Not on the stage at all: removed, unless the nearest ancestor that is on
    // the stage is inactive and prunes it.
    for (pxr::SdfPath ancestor_path = path.GetParentPath();
         !ancestor_path.IsEmpty() && ancestor_path != pxr::SdfPath::AbsoluteRootPath();
         ancestor_path = ancestor_path.GetParentPath())
    {
        if (const pxr::UsdPrim ancestor = stage->GetPrimAtPath(ancestor_path))
            return ancestor.IsActive();
    }
    return true;
}

/// Remove from @p session's own layer the specs of every prim a reload
/// removed from its stage (see IsRemovedFromStage()), together with their
/// subtrees. Returns how many prim specs were removed. Must run while the
/// session's consumer is paused.
std::size_t DropSpecsOfRemovedPrims(Session& session)
{
    if (!session.stage) return 0;
    const pxr::SdfLayerHandle session_layer = session.stage->GetSessionLayer();
    if (!session_layer) return 0;

    std::vector<pxr::SdfPath> removed;
    session_layer->Traverse(
        pxr::SdfPath::AbsoluteRootPath(),
        [&](const pxr::SdfPath& p)
        {
            if (p.IsPrimPath() && IsRemovedFromStage(session.stage, p)) removed.push_back(p);
        });

    // Ancestors first, so a removed prim's descendants go with its spec.
    std::sort(removed.begin(), removed.end(),
              [](const pxr::SdfPath& a, const pxr::SdfPath& b)
              {
                  return a.GetPathElementCount() < b.GetPathElementCount();
              });

    std::vector<pxr::SdfPath> dropped;
    for (const pxr::SdfPath& path : removed)
    {
        const bool below_dropped = std::any_of(
            dropped.begin(), dropped.end(),
            [&](const pxr::SdfPath& d) { return path.HasPrefix(d); });
        if (below_dropped) continue;

        const pxr::SdfPrimSpecHandle spec = session_layer->GetPrimAtPath(path);
        if (!spec) continue;
        const pxr::SdfPath parent_path = path.GetParentPath();
        const pxr::SdfPrimSpecHandle parent =
            parent_path == pxr::SdfPath::AbsoluteRootPath()
                ? session_layer->GetPseudoRoot()
                : session_layer->GetPrimAtPath(parent_path);
        if (parent && parent->RemoveNameChild(spec)) dropped.push_back(path);
    }
    return dropped.size();
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

    m_layer_thread_ = std::thread(&SessionManager::LayerThreadRun, this);

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
    // manager (for example a commit that queues its reload). Until their
    // teardown finishes they stay in m_closing_, where a reload finds them.
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::unique_lock lk(m_mutex_);
        for (auto& [id, session] : m_sessions_)
        {
            (void)id;
            sessions.push_back(session);
            m_closing_.push_back(session);
        }
        m_sessions_.clear();
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
    for (auto& session : sessions)
    {
        TeardownSession(session);
    }
    sessions.clear();

    // Reloads queued by commits that finished during the teardown still run;
    // the thread exits once its queue is empty.
    {
        std::lock_guard lk(m_layer_mutex_);
        m_layer_stop_ = true;
    }
    m_layer_cv_.notify_all();
    if (m_layer_thread_.joinable()) m_layer_thread_.join();

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
    // From here on no command of this session runs, so this thread has the
    // stage to itself, apart from a reload that pauses the session (which
    // still finds it in m_closing_) - held off by the shared lock below.

    if (session->listener) session->listener->Revoke();

    // Order: commit slot, then SharedLayers(), as everywhere else.
    std::shared_ptr<void> commit_slot;
    if (session->auto_commit)
    {
        m_commit_slot_.acquire();
        commit_slot = CommitSlotToken();
    }
    std::shared_lock layers(idtx::concurrency::SharedLayers());

    const pxr::SdfLayerHandle session_layer =
        session->stage ? session->stage->GetSessionLayer() : pxr::SdfLayerHandle();
    const bool has_overrides = session_layer && !session_layer->IsEmpty();

    // Auto-commit on destroy folds the sidecar overrides back into the original
    // file (preserving composition arcs). The file's layer is then reloaded
    // like after any commit, so the other sessions using it show the
    // committed content and a later commit of theirs flattens it rather than
    // overwriting it with the old baseline.
    bool committed = false;
    pxr::SdfLayerRefPtr committed_root;
    if (session->auto_commit && session->stage && has_overrides)
    {
        const CommitResult result = CommitOverrides(*session);
        committed = result.status == CommitStatus::Ok;
        if (committed) committed_root = session->stage->GetRootLayer();
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

    // Release the stage while no reload can run, so it is never recomposed
    // by one while it is being destroyed.
    session->used_layers.clear();
    session->stage = pxr::UsdStageRefPtr();
    layers.unlock();

    // The reload runs on the layer thread, like a commit's. The job keeps the
    // commit slot until it has finished, so the next commit or upload of the
    // file starts from the reloaded baseline. During shutdown the layer
    // thread still runs: it is stopped only after every teardown.
    if (committed_root)
    {
        PostLayerJob([this, committed_root, label = session->usd_file, commit_slot]
        {
            ReloadLayerPausingUsers(committed_root, label);
        });
    }
    commit_slot.reset();

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

    std::unique_lock lk(m_mutex_);
    std::erase(m_closing_, session);
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

    // The root layer, and every layer the stage composes, may already be in
    // use by other stages. Held until the session is registered, so a reload
    // either finds it with its used layers or happens entirely before it
    // opened anything. Declared first so it outlives the local handles.
    std::shared_lock layers(idtx::concurrency::SharedLayers());

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
    //      ReloadSharedLayer when a client uploads a replacement file)
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
    RefreshUsedLayers(*session);

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
        m_closing_.push_back(victim);
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

bool SessionManager::ReloadSharedLayer(const std::string& usd_file, std::shared_ptr<void> hold)
{
    std::filesystem::path resolved;
    if (m_locator_.Resolve(usd_file, resolved) != idtx::utils::UsdFileLocator::Status::Ok)
    {
        IDTX_LOG(IDTX_WARN, "ReloadSharedLayer: cannot resolve '{}'.", usd_file);
        return false;
    }

    // `hold` lives in the job and is released when the job is destroyed,
    // after the reload.
    return PostLayerJob([this, usd_file, path = resolved.string(), hold = std::move(hold)]
    {
        // Resolved exactly as Create() opens it, so this finds the same
        // layer. The registry lookup is thread-safe, and all reloads run on
        // this thread, so none is in progress.
        const pxr::SdfLayerRefPtr layer = pxr::SdfLayer::Find(path);
        if (!layer)
        {
            IDTX_LOG(IDTX_DEBUG, "ReloadSharedLayer('{}'): no stage has it loaded.", usd_file);
            return;
        }
        ReloadLayerPausingUsers(layer, usd_file);
    });
}

std::shared_ptr<void> SessionManager::AcquireFileWriteSlot(std::chrono::milliseconds timeout)
{
    if (!m_commit_slot_.try_acquire_for(timeout)) return nullptr;
    return CommitSlotToken();
}

std::shared_ptr<void> SessionManager::CommitSlotToken()
{
    return std::shared_ptr<void>(nullptr, [this](void*) { m_commit_slot_.release(); });
}

bool SessionManager::PostLayerJob(std::function<void()> job)
{
    {
        std::lock_guard lk(m_layer_mutex_);
        if (m_layer_stop_) return false;
        m_layer_jobs_.push_back(std::move(job));
    }
    m_layer_cv_.notify_one();
    return true;
}

void SessionManager::LayerThreadRun()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lk(m_layer_mutex_);
            m_layer_cv_.wait(lk, [this] { return m_layer_stop_ || !m_layer_jobs_.empty(); });
            if (m_layer_jobs_.empty()) return; // stopped and drained
            job = std::move(m_layer_jobs_.front());
            m_layer_jobs_.pop_front();
        }

        try
        {
            job();
        }
        catch (const std::exception& e)
        {
            IDTX_LOG(IDTX_ERROR, "Layer job threw: {}", e.what());
        }
        catch (...)
        {
            IDTX_LOG(IDTX_ERROR, "Layer job threw an unknown exception.");
        }
    }
}

std::vector<std::shared_ptr<Session>>
SessionManager::SessionsUsingLayer(const pxr::SdfLayerHandle& layer) const
{
    std::vector<std::shared_ptr<Session>> users;
    const auto uses = [&layer](const std::shared_ptr<Session>& session)
    {
        return session
            && std::find(session->used_layers.begin(), session->used_layers.end(), layer)
                   != session->used_layers.end();
    };

    std::shared_lock lk(m_mutex_);
    for (const auto& [id, session] : m_sessions_)
    {
        (void)id;
        if (uses(session)) users.push_back(session);
    }
    for (const auto& session : m_closing_)
    {
        if (uses(session)) users.push_back(session);
    }
    return users;
}

void SessionManager::RefreshUsedLayers(Session& session)
{
    // Value clips are deliberately not listed. Clip files only supply time
    // samples, are opened lazily on the first time-sampled read, and are not
    // subscribed to by the stage, so a reload of one neither recomposes the
    // stage nor notifies its listener. Sessions currently read values only at
    // UsdTimeCode::Default(), which never opens a clip file, so no consumer
    // ever reads a clip file a reload could rewrite. The thumbnail renderer
    // may read clips, but holds SharedLayers() for the whole render.
    //
    // Supporting time-sampled reads in sessions (playback, scrubbing) would
    // need:
    //  - includeClipLayers=true here, and a refresh after every command that
    //    may resolve time-sampled values, since that is when clips open. Such
    //    commands then change the used layers and, like payload and
    //    active-state changes, must take SharedLayers() in ProcessCommand.
    //  - Explicit change propagation after reloading a clip file: the stage
    //    is not notified, so the listener sends nothing, and affected clients
    //    must be told some other way (for example re-broadcast or refetch).
    session.used_layers = session.stage
        ? session.stage->GetUsedLayers(/*includeClipLayers=*/false)
        : pxr::SdfLayerHandleVector{};
}

bool SessionManager::ReloadLayerPausingUsers(const pxr::SdfLayerHandle& layer,
                                             const std::string& usd_file)
{
    if (!layer) return false;

    // Exclusive: no stage is being created and no session is adding or
    // dropping layers, so the used-layer lists read below are complete.
    std::unique_lock layers(idtx::concurrency::SharedLayers());

    // Only these sessions' stages compose the layer and are recomposed by
    // the reload; every other session keeps running. Pause() returns once
    // their running command, if any, has finished. No command waits on
    // anything, so that is bounded by one command.
    const std::vector<std::shared_ptr<Session>> users = SessionsUsingLayer(layer);
    std::vector<const Session*> paused;
    paused.reserve(users.size());
    for (const auto& session : users)
    {
        if (session->commands) session->commands->Pause();
        paused.push_back(session.get());
    }

    bool ok = false;
    try
    {
        // Every stage using the layer recomposes and runs its listener on
        // this thread when the change block closes, inside this scope.
        AuthoringContext context;
        context.reload = true;
        context.paused = &paused;
        const AuthoringScope authoring(context);
        pxr::SdfChangeBlock block;
        ok = layer->Reload(/*force=*/true);
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "SdfLayer::Reload threw for '{}': {}", usd_file, e.what());
    }

    if (ok)
    {
        for (const auto& session : users)
        {
            // A session's own edits of a prim the file no longer has are
            // dropped. Its notices belong to this reload, so they run under
            // the same context.
            try
            {
                AuthoringContext context;
                context.reload = true;
                context.paused = &paused;
                const AuthoringScope authoring(context);
                pxr::SdfChangeBlock block;
                if (DropSpecsOfRemovedPrims(*session) > 0)
                    session->dirty.store(true, std::memory_order_relaxed);
            }
            catch (const std::exception& e)
            {
                IDTX_LOG(IDTX_ERROR, "Session {}: dropping edits of prims removed by '{}' threw: {}",
                         session->id, usd_file, e.what());
            }

            AnnounceLayerReloaded(*session, usd_file);
        }
    }

    // The reload may have added or removed sublayers, references or payloads.
    for (const auto& session : users) RefreshUsedLayers(*session);

    layers.unlock();
    for (const auto& session : users)
    {
        if (session->commands) session->commands->Resume();
    }

    if (ok)
        IDTX_LOG(IDTX_INFO, "Reloaded layer '{}' ({} session(s) paused).", usd_file, users.size());
    else
        IDTX_LOG(IDTX_DEBUG, "SdfLayer::Reload reported no change for '{}'.", usd_file);
    return ok;
}

void SessionManager::AnnounceLayerReloaded(Session& session, const std::string& usd_file)
{
    // The session's version after the reload, including what its listener
    // recorded for it. Nothing else advances it while the session is paused.
    const std::uint64_t seq = session.server_seq.load();

    idtxcore::BaseMessage msg;
    msg.set_session_id(session.id);
    msg.set_server_seq(seq);
    auto* reloaded = msg.mutable_layer_reloaded();
    reloaded->set_usd_path(usd_file);
    reloaded->set_usd_uri(DownloadUri(usd_file));
    std::string payload;
    if (!msg.SerializeToString(&payload)) return;

    // No snapshot is pushed: a client asks for one once it has rebuilt its
    // stage, so the snapshot never arrives while it is still reloading.
    std::shared_lock lk(session.clients_mutex);
    for (const auto& [id, conn] : session.clients)
    {
        conn->send_binary(payload);
        session.versions.RecordSent(id, seq);
    }
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

void SessionManager::DetachClient(Session& session, ConnectionId connection_id)
{
    std::unique_lock lk(session.clients_mutex);
    session.clients.erase(connection_id);
    if (session.clients.empty())
        session.empty_since = std::chrono::steady_clock::now();
    IDTX_LOG(IDTX_INFO, "Client detached from session {} (now {} clients).",
             session.id, session.clients.size());
}

// ---------------------------------------------------------------------------
// Command submission
// ---------------------------------------------------------------------------

SessionManager::SubmitStatus SessionManager::Submit(Session& session, SessionCommand command)
{
    if (!session.commands) return SubmitStatus::SessionClosing;
    return ToSubmitStatus(session.commands->TrySubmit(std::move(command)));
}

SessionManager::SubmitStatus SessionManager::SubmitPriority(Session& session, SessionCommand command)
{
    if (!session.commands) return SubmitStatus::SessionClosing;
    return ToSubmitStatus(session.commands->TrySubmitPriority(std::move(command)));
}

SessionManager::SubmitStatus SessionManager::SubmitTransformCommand(
    const std::string& session_id,
    std::unique_ptr<const idtxcore::TransformUpdate> update,
    ConnectionId origin,
    std::uint64_t request_id,
    std::uint64_t base_seq) const {
    auto session = Get(session_id);
    if (!session)
    {
        IDTX_LOG(IDTX_WARN, "SubmitTransformCommand: unknown session {}.", session_id);
        return SubmitStatus::UnknownSession;
    }
    if (!update) return SubmitStatus::Accepted; // nothing to apply

    const auto status = Submit(*session,
                               TransformCommand(std::move(update), origin, request_id, base_seq));
    if (status == SubmitStatus::QueueFull)
    {
        IDTX_LOG(IDTX_WARN, "Session {} command queue is full; rejecting update.", session_id);
    }
    return status;
}

void SessionManager::SubmitCorrectionCommand(const std::string& session_id,
                                             ConnectionId connection_id,
                                             std::shared_ptr<PendingCorrection> request) const
{
    if (!request) return;
    auto session = Get(session_id);
    if (!session) return;

    // One queued correction per request is enough: it reads the state when it
    // runs, which covers every rejection before that.
    if (request->queued.exchange(true)) return;

    const auto status = SubmitPriority(*session, CorrectionCommand{connection_id, request});
    if (status != SubmitStatus::Accepted)
    {
        request->queued.store(false);
        if (status == SubmitStatus::QueueFull)
            IDTX_LOG(IDTX_WARN, "Session {}: correction queue is full; dropping correction for '{}'.",
                     session_id, request->prim_path);
    }
}

SessionManager::SubmitStatus SessionManager::SubmitJoinSnapshotCommand(
    const std::string& session_id,
    ConnectionId connection_id)
{
    auto session = Get(session_id);
    if (!session) return SubmitStatus::UnknownSession;
    return Submit(*session, SnapshotCommand{connection_id, /*request_id=*/0});
}

SessionManager::SubmitStatus SessionManager::SubmitSnapshotRequestCommand(
    const std::string& session_id,
    ConnectionId connection_id,
    std::uint64_t request_id)
{
    auto session = Get(session_id);
    if (!session) return SubmitStatus::UnknownSession;
    return Submit(*session, SnapshotCommand{connection_id, request_id});
}

SessionManager::SubmitStatus SessionManager::SubmitCreatePrimCommand(
    const std::string& session_id,
    std::string parent_path,
    idtxcore::PrimType type,
    ConnectionId origin,
    std::uint64_t request_id) const
{
    auto session = Get(session_id);
    if (!session)
    {
        IDTX_LOG(IDTX_WARN, "SubmitCreatePrimCommand: unknown session {}.", session_id);
        return SubmitStatus::UnknownSession;
    }

    CreatePrimCommand command;
    command.parent_path = std::move(parent_path);
    command.type        = type;
    command.origin      = origin;
    command.request_id  = request_id;

    const auto status = Submit(*session, std::move(command));
    if (status == SubmitStatus::QueueFull)
    {
        IDTX_LOG(IDTX_WARN, "Session {} command queue is full; rejecting prim creation.",
                 session_id);
    }
    return status;
}

SessionManager::SubmitStatus SessionManager::SubmitSetPrimActiveCommand(
    const std::string& session_id,
    std::string prim_path,
    bool active,
    ConnectionId origin,
    std::uint64_t request_id) const
{
    auto session = Get(session_id);
    if (!session)
    {
        IDTX_LOG(IDTX_WARN, "SubmitSetPrimActiveCommand: unknown session {}.", session_id);
        return SubmitStatus::UnknownSession;
    }

    SetPrimActiveCommand command;
    command.prim_path  = std::move(prim_path);
    command.active     = active;
    command.origin     = origin;
    command.request_id = request_id;

    const auto status = Submit(*session, std::move(command));
    if (status == SubmitStatus::QueueFull)
    {
        IDTX_LOG(IDTX_WARN, "Session {} command queue is full; rejecting prim active-state change.",
                 session_id);
    }
    return status;
}

SessionManager::SubmitStatus SessionManager::SubmitPayloadCommand(
    const std::string& session_id,
    std::string prim_path,
    std::string asset_path,
    bool remove,
    ConnectionId origin,
    std::uint64_t request_id) const
{
    auto session = Get(session_id);
    if (!session)
    {
        IDTX_LOG(IDTX_WARN, "SubmitPayloadCommand: unknown session {}.", session_id);
        return SubmitStatus::UnknownSession;
    }

    PayloadCommand command;
    command.prim_path  = std::move(prim_path);
    command.asset_path = std::move(asset_path);
    command.remove     = remove;
    command.origin     = origin;
    command.request_id = request_id;

    const auto status = Submit(*session, std::move(command));
    if (status == SubmitStatus::QueueFull)
    {
        IDTX_LOG(IDTX_WARN, "Session {} command queue is full; rejecting payload change.",
                 session_id);
    }
    return status;
}

// ---------------------------------------------------------------------------
// Command processing (runs on the session's consumer)
// ---------------------------------------------------------------------------

void SessionManager::ProcessCommand(Session& session, SessionCommand&& command)
{
    try
    {
        // A payload change, and an active-state change (which prunes or
        // restores a subtree together with whatever it references), starts or
        // stops using layers, so it must not overlap a reload, which decides
        // from used_layers which sessions to pause. A worker never waits for
        // that: while a reload runs or waits, the command is handed back and
        // the session stays paused until the reload has released the lock.
        // Every other command only works on layers its stage already uses,
        // and a reload of one of those pauses this session instead.
        std::shared_lock<idtx::concurrency::SharedLayerLock> layers;
        // Only the commands checked here will make the scheduler try to take the lock.
        if (std::holds_alternative<PayloadCommand>(command)
            || std::holds_alternative<SetPrimActiveCommand>(command))
        {
            const std::weak_ptr<SessionCommandScheduler> scheduler = session.commands;
            if (!idtx::concurrency::SharedLayers().try_lock_shared_or_notify(
                    [scheduler]
                    {
                        if (auto resumed = scheduler.lock()) resumed->Resume();
                    }))
            {
                // Commands for which the scheduler needs the lock but cannot have it yet are deferred.
                session.commands->DeferCurrent(std::move(command));
                return;
            }
            // Transfer ownership of the lock without another lock attempt (std::adopt_lock) to layers,
            // so it is not locked again later, until layers is destroyed.
            // std::shared_lock RAII ensures the lock is released at the end of this try block,
            // when layers is destroyed, i.e. after the command has been handled.
            // Fwiw: If the handler below throws, the same destructor runs during stack unwinding,
            // before control reaches the catch block.
            layers = std::shared_lock(idtx::concurrency::SharedLayers(), std::adopt_lock);
        }

        const AuthoringScope authoring({&session});
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
    const std::string& prim_path = command.update->prim_path();

    // An update made on an outdated state is rejected before the stage is
    // touched. No correction follows: for a stale update the newer state was
    // already sent to the client, and an invalid base is a client bug.
    switch (session.versions.CheckUpdate(command.origin, prim_path, command.base_seq))
    {
        case UpdateBaseStatus::Stale:
            IDTX_LOG(IDTX_DEBUG, "Session {}: stale update of '{}' (base {}).",
                     session.id, prim_path, command.base_seq);
            SendAck(session, command.origin, command.request_id, false, "stale");
            return;
        case UpdateBaseStatus::InvalidBase:
            IDTX_LOG(IDTX_WARN,
                     "Session {}: update of '{}' has base {}, which was never sent to the client.",
                     session.id, prim_path, command.base_seq);
            SendAck(session, command.origin, command.request_id, false, "invalid_base");
            return;
        case UpdateBaseStatus::Current:
            break;
    }

    bool ok = false;
    try
    {
        // Lets the listener suppress the echo back to the origin.
        const AuthoringScope authoring({&session, command.origin});
        ok = TransformDispatcher::Apply(session.stage, *command.update);
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "TransformUpdate threw in session {}: {}", session.id, e.what());
    }

    // Mark the session dirty so the SessionFlusher persists the session layer
    // to its sidecar on its next tick.
    if (ok) session.dirty.store(true, std::memory_order_relaxed);

    // The Ack is sent after the broadcasts triggered by this update, and its
    // server_seq reflects the state after the update.
    SendAck(session, command.origin, command.request_id, ok, "apply_failed");

    // The client already shows its update. The prim is unchanged, so tell the
    // client what the server has instead.
    if (!ok) SendCorrection(session, command.origin, prim_path);
}

void SessionManager::HandleCommand(Session& session, CreatePrimCommand& command)
{
    if (!session.stage)
    {
        SendAck(session, command.origin, command.request_id, false, "apply_failed");
        return;
    }

    // Author the prim. The origin is set for consistency with the transform
    // path; `creating` makes the TfNotice listener skip the resync this
    // authoring fires, because the creation is broadcast explicitly below.
    CreatedPrim created;
    bool ok = false;
    try
    {
        AuthoringContext context;
        context.session  = &session;
        context.origin   = command.origin;
        context.creating = true;
        const AuthoringScope authoring(context);
        ok = PrimDispatcher::Create(session.stage, command.parent_path, command.type, created);
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "CreatePrim threw in session {}: {}", session.id, e.what());
    }

    if (!ok)
    {
        // Bad type, or a missing/forbidden parent (including gprim nesting).
        SendAck(session, command.origin, command.request_id, false, "invalid_parent");
        return;
    }

    session.dirty.store(true, std::memory_order_relaxed);

    // One state version for the creation, stamped on the broadcast and the Ack.
    const std::uint64_t seq = session.server_seq.fetch_add(1) + 1;
    session.versions.RecordChange(created.prim_path, seq, command.origin);

    const std::string payload =
        SerializeCreatePrimBroadcast(session, command.parent_path, command.type, created, seq);
    BroadcastToAll(session, payload, seq);

    // Ack after the broadcast; SendAck stamps the current server_seq (== seq).
    SendAck(session, command.origin, command.request_id, true, nullptr);

    IDTX_LOG(IDTX_INFO, "Session {}: created '{}' under '{}'.",
             session.id, created.prim_path, command.parent_path);
}

void SessionManager::HandleCommand(Session& session, SetPrimActiveCommand& command)
{
    if (!session.stage)
    {
        SendAck(session, command.origin, command.request_id, false, "apply_failed");
        return;
    }

    // Author the active flag. The origin is set for consistency with the
    // other authoring paths; `setting_active` makes the TfNotice listener
    // skip the resync this fires, because the change is announced explicitly
    // with a SetPrimActiveBroadcast below (an inactive prim is still readable,
    // so without it the generic path would broadcast a stale transform).
    PrimDispatcher::SetActiveStatus status = PrimDispatcher::SetActiveStatus::Failed;
    try
    {
        AuthoringContext context;
        context.session        = &session;
        context.origin         = command.origin;
        context.setting_active = true;
        const AuthoringScope authoring(context);
        status = PrimDispatcher::SetActive(session.stage, command.prim_path, command.active);
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "SetPrimActive threw in session {}: {}", session.id, e.what());
    }

    // Deactivating prunes the prim's subtree, with every layer it references
    // or payloads, from the composed stage; reactivating composes them again,
    // opening them through the shared registry. A reload has to find this
    // session for each layer it now uses; still under SharedLayers() (see
    // ProcessCommand()), so no reload reads the list meanwhile.
    RefreshUsedLayers(session);

    switch (status)
    {
        case PrimDispatcher::SetActiveStatus::InvalidPath:
        case PrimDispatcher::SetActiveStatus::NotFound:
            SendAck(session, command.origin, command.request_id, false, "unknown_prim");
            return;
        case PrimDispatcher::SetActiveStatus::NotDeletable:
            SendAck(session, command.origin, command.request_id, false, "not_deletable");
            return;
        case PrimDispatcher::SetActiveStatus::Failed:
            SendAck(session, command.origin, command.request_id, false, "apply_failed");
            return;
        case PrimDispatcher::SetActiveStatus::AlreadyInState:
            // Idempotent: the prim already had the requested state, so there is
            // nothing to broadcast. Ack ok with the current (unchanged) seq.
            SendAck(session, command.origin, command.request_id, true, nullptr);
            return;
        case PrimDispatcher::SetActiveStatus::Ok:
            break;
    }

    session.dirty.store(true, std::memory_order_relaxed);

    // One state version for the change, stamped on the broadcast and the Ack.
    const std::uint64_t seq = session.server_seq.fetch_add(1) + 1;
    session.versions.RecordChange(command.prim_path, seq, command.origin);

    const std::string payload =
        SerializeSetPrimActiveBroadcast(session, command.prim_path, command.active, seq);
    BroadcastToAll(session, payload, seq);

    SendAck(session, command.origin, command.request_id, true, nullptr);

    IDTX_LOG(IDTX_INFO, "Session {}: set active={} on '{}'.",
             session.id, command.active, command.prim_path);
}

void SessionManager::HandleCommand(Session& session, PayloadCommand& command)
{
    // Runs with SharedLayers() held shared (see ProcessCommand()).
    if (!session.stage)
    {
        SendAck(session, command.origin, command.request_id, false, "apply_failed");
        return;
    }

    bool ok = false;
    {
        const AuthoringScope authoring({&session, command.origin});

        // TODO(payload prims): author the arc into the session layer, for
        // example AddPayload() / ClearPayloads() on the prim's UsdPayloads.
        // For the implementation:
        //  - Asset paths authored into the session layer are anchored to the
        //    sidecar in the sessions directory, not to the root file. Resolve
        //    the client's path against the root file and author it absolute
        //    or relative to the sidecar.
        //  - Loading the payload parses its file on this worker. Open it off
        //    the worker pool first (for example keep a UsdStage on it alive
        //    in the command), so only the recompose runs here.
        //  - What the listener broadcasts for the resulting resync, and the
        //    message clients receive, is not defined yet.
        IDTX_LOG(IDTX_WARN, "Session {}: {} payload on '{}' is not implemented yet.",
                 session.id, command.remove ? "removing a" : "adding a", command.prim_path);
    }

    // Whatever was authored, the stage may now compose different layers. A
    // reload has to find this session for each of them; still under
    // SharedLayers(), so no reload reads the list meanwhile.
    RefreshUsedLayers(session);

    if (ok) session.dirty.store(true, std::memory_order_relaxed);
    SendAck(session, command.origin, command.request_id, ok, "not_implemented");
}

void SessionManager::HandleCommand(Session& session, SnapshotCommand& command)
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
        //
        // The whole snapshot is built from the session layer via Sdf, never the
        // composed stage: a soft-deleted prim is pruned from the stage (so its
        // created children are not retrievable there), but every spec still
        // lives in the layer. Reading at the Sdf level lets us replay the full
        // state — including a deleted prim and its subtree — so a later undo is
        // a one-prim active-flag toggle for the late joiner too, with no
        // subtree snapshot needed.
        std::set<pxr::SdfPath> prim_paths;
        session_layer->Traverse(
            pxr::SdfPath::AbsoluteRootPath(),
            [&prim_paths](const pxr::SdfPath& p)
            {
                if (p.IsPrimPath())          prim_paths.insert(p);
                else if (p.IsPropertyPath()) prim_paths.insert(p.GetPrimPath());
            });

        // A prim the file no longer defines has no place in the snapshot: the
        // client's copy of the file does not have it either. A reload drops
        // such specs already; this covers a sidecar resumed from disk.
        std::erase_if(prim_paths, [&session](const pxr::SdfPath& p)
                      {
                          return IsRemovedFromStage(session.stage, p);
                      });

        // Late joiners must re-create prims authored into the session layer
        // (they are absent from the on-disk root file a joiner fetches via
        // REST). A session-layer `def` with a built-in type is such a creation;
        // a created-then-deleted prim keeps its def and is replayed too, then
        // deactivated by its active frame below. Emit all creation frames first,
        // ordered ancestor-before-descendant by path depth, so a nested prim's
        // parent exists before it is created. Collect soft-deleted paths as we
        // go; their active frames come last.
        std::vector<std::pair<std::size_t, std::string>> create_frames;
        std::vector<std::string> deleted_paths;
        for (const auto& p : prim_paths)
        {
            const pxr::SdfPrimSpecHandle spec = session_layer->GetPrimAtPath(p);
            if (!spec) continue;

            if (spec->HasActive() && !spec->GetActive())
                deleted_paths.push_back(p.GetString());

            if (spec->GetSpecifier() != pxr::SdfSpecifierDef) continue;

            const idtxcore::PrimType type =
                PrimDispatcher::TypeFromName(spec->GetTypeName().GetString());
            if (type == idtxcore::PRIM_TYPE_UNSPECIFIED) continue;

            CreatedPrim created;
            created.name      = p.GetName();
            created.prim_path = p.GetString();
            PrimDispatcher::ReadSchemaDefaults(type, created);
            create_frames.emplace_back(
                p.GetPathElementCount(),
                SerializeCreatePrimBroadcast(
                    session, p.GetParentPath().GetString(), type, created, seq));
        }
        std::stable_sort(create_frames.begin(), create_frames.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& cf : create_frames) frames.push_back(std::move(cf.second));

        // Transform frames for every session-layer prim that authored xform
        // ops, read straight from the layer (see
        // TransformDispatcher::ReadAuthoredLocalTransform). A prim with no
        // authored ops (e.g. a bare ancestor `over`, or a just-created prim at
        // the origin) emits none; the client already holds its base transform.
        // By now all created prims have been announced.
        for (const auto& p : prim_paths)
        {
            const pxr::SdfPrimSpecHandle spec = session_layer->GetPrimAtPath(p);
            if (!spec) continue;

            idtxcore::SeparateTransform sep;
            idtxcore::Matrix4dTransform mat;
            bool use_matrix = false;
            if (!TransformDispatcher::ReadAuthoredLocalTransform(spec, sep, mat, use_matrix))
                continue;

            idtxcore::BaseMessage msg = AssembleXformBroadcast(
                session, p.GetString(), std::move(sep), std::move(mat), use_matrix);
            msg.set_server_seq(seq);
            std::string payload;
            if (msg.SerializeToString(&payload)) frames.push_back(std::move(payload));
        }

        // Active frames last: every path now exists on the client (from REST +
        // the create frames above), so authoring active=false prunes each
        // subtree on the client exactly as it is pruned on the server.
        for (const auto& path : deleted_paths)
            frames.push_back(SerializeSetPrimActiveBroadcast(session, path, /*active=*/false, seq));
    }

    // Terminal marker: the client now has the full server state as of seq.
    idtxcore::BaseMessage done;
    done.set_session_id(session.id);
    done.set_server_seq(seq);
    done.set_request_id(command.request_id); // 0 after a join
    done.mutable_snapshot_complete();
    std::string done_payload;
    if (done.SerializeToString(&done_payload)) frames.push_back(std::move(done_payload));

    {
        std::shared_lock lk(session.clients_mutex);
        auto it = session.clients.find(command.connection);
        if (it == session.clients.end()) return; // left before the snapshot ran
        for (const auto& f : frames) it->second->send_binary(f);
        session.versions.RecordSent(command.connection, seq);
    }

    IDTX_LOG(IDTX_INFO, "Sent {} snapshot ({} prim frame(s)) to a client of session {}.",
             command.request_id != 0 ? "requested" : "join", frames.size() - 1, session.id);
}

void SessionManager::HandleCommand(Session& session, CommitCommand& command)
{
    // The file is written here, in order with the session's commands. Its
    // layer is reloaded on the layer thread, because the reload pauses every
    // session using the layer, this one included, which a worker must never
    // wait for. The result is reported once the reload is done, so a caller
    // that sees success, and a session created afterwards, see the committed
    // content. The commit slot stays taken until then.
    CommitResult result = CommitOverrides(session);
    if (result.status != CommitStatus::Ok)
    {
        command.result.set_value(std::move(result));
        return;
    }
    IDTX_LOG(IDTX_INFO, "Committed session {} to '{}'.", session.id, session.usd_file);

    const pxr::SdfLayerRefPtr root_layer = session.stage->GetRootLayer();
    auto pending  = std::make_shared<CommitCommand>(std::move(command));
    auto reported = std::make_shared<CommitResult>(std::move(result));

    const bool posted = PostLayerJob([this, root_layer, label = session.usd_file, pending, reported]
    {
        ReloadLayerPausingUsers(root_layer, label);
        pending->result.set_value(std::move(*reported));
    });
    if (!posted)
    {
        IDTX_LOG(IDTX_WARN,
                 "Commit of session {}: shutting down, '{}' was written but not reloaded.",
                 session.id, session.usd_file);
        pending->result.set_value(std::move(*reported));
    }
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

void SessionManager::HandleCommand(Session& session, CorrectionCommand& command)
{
    if (!command.request) return;
    // Clear the mark before reading the state, so an update rejected from now
    // on queues a correction of its own.
    command.request->queued.store(false);
    SendCorrection(session, command.connection, command.request->prim_path);
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

bool SessionManager::SendToConnection(const Session& session,
                                      ConnectionId connection_id,
                                      const std::string& payload)
{
    if (connection_id == 0 || payload.empty()) return false;
    std::shared_lock lk(session.clients_mutex);
    auto it = session.clients.find(connection_id);
    if (it == session.clients.end()) return false;
    it->second->send_binary(payload);
    return true;
}

void SessionManager::SendAck(Session& session,
                             ConnectionId connection_id,
                             std::uint64_t request_id,
                             bool ok,
                             const char* error)
{
    const std::uint64_t seq = session.server_seq.load();
    if (SendToConnection(session, connection_id,
                         SerializeAck(session, request_id, seq, ok, error)))
    {
        session.versions.RecordSent(connection_id, seq);
    }
}

void SessionManager::SendCorrection(Session& session,
                                    ConnectionId connection_id,
                                    const std::string& prim_path)
{
    if (!session.stage || connection_id == 0) return;

    // A prim that was soft-deleted after the straggler's update was created is
    // still readable (inactive) via GetPrimAtPath, so a transform correction
    // would wrongly re-place it. Tell the straggler to deactivate instead, with
    // the same message the deletion used, so it converges on the delete.
    if (pxr::SdfPath::IsValidPathString(prim_path))
    {
        const pxr::UsdPrim prim = session.stage->GetPrimAtPath(pxr::SdfPath(prim_path));
        if (prim && !prim.IsActive())
        {
            std::shared_lock lk(session.clients_mutex);
            auto it = session.clients.find(connection_id);
            if (it == session.clients.end()) return;

            const std::uint64_t seq = session.server_seq.fetch_add(1) + 1;
            const std::string payload =
                SerializeSetPrimActiveBroadcast(session, prim_path, /*active=*/false, seq);
            it->second->send_binary(payload);
            session.versions.RecordCorrection(connection_id, prim_path, seq);
            return;
        }
    }

    std::optional<idtxcore::BaseMessage> msg = BuildBroadcastMessage(session, prim_path);
    if (!msg) return; // unknown prim or no readable transform: nothing to correct

    std::shared_lock lk(session.clients_mutex);
    auto it = session.clients.find(connection_id);
    if (it == session.clients.end()) return;

    // Updates the client sent before it receives this version are stale, so
    // the correction is the last state it gets for the prim.
    const std::uint64_t seq = session.server_seq.fetch_add(1) + 1;
    msg->set_server_seq(seq);
    std::string payload;
    if (!msg->SerializeToString(&payload)) return;
    it->second->send_binary(payload);
    session.versions.RecordCorrection(connection_id, prim_path, seq);
}

// ---------------------------------------------------------------------------
// Broadcast
// ---------------------------------------------------------------------------

void SessionManager::BroadcastResolvedTransform(Session& session,
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
        if (id == origin) continue;
        conn->send_binary(payload);
        session.versions.RecordSent(id, server_seq);
    }
}

void SessionManager::BroadcastToAll(Session& session,
                                    const std::string& payload,
                                    std::uint64_t server_seq)
{
    if (payload.empty()) return;

    std::shared_lock lk(session.clients_mutex);
    for (const auto& [id, conn] : session.clients)
    {
        conn->send_binary(payload);
        session.versions.RecordSent(id, server_seq);
    }
}

SessionManager::CommitStatus SessionManager::SubmitCommitSessionCommand(const std::string& session_id,
                                                                        std::string& out_error)
{
    // The stage is not checked here: a teardown may be releasing it. The
    // command reports a session without a stage as unknown.
    auto session = Get(session_id);
    if (!session)
    {
        out_error = "unknown session";
        return CommitStatus::UnknownSession;
    }

    // One commit at a time, from here until the reload of its file is done,
    // so the next one flattens the baseline this one reloaded. Taken on this
    // (REST) thread, before anything is queued, so no worker ever waits for it.
    if (!m_commit_slot_.try_acquire_for(kCommitTimeout))
    {
        out_error = "another commit or upload is still writing";
        return CommitStatus::Unavailable;
    }

    CommitCommand command;
    command.commit_slot = CommitSlotToken();
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
        // The commit stays queued and still runs; only the caller stops
        // waiting. Reporting a failure here would invite a retry of a commit
        // that is going to happen anyway.
        out_error = "commit is still in progress";
        IDTX_LOG(IDTX_WARN, "Commit of session {} still running after {}s; reported as pending.",
                 session_id, static_cast<long long>(kCommitTimeout.count()));
        return CommitStatus::Pending;
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
                m_closing_.push_back(session);
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

std::size_t SessionManager::SubmitFlushDirtySessionsCommand()
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
