#include "SessionManager.h"

#include <chrono>
#include <filesystem>
#include <utility>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>

#include <idtx/proto/base.pb.h>
#include <idtx/proto/transform.pb.h>

#include "StageNoticeListener.h"
#include "TransformDispatcher.h"
#include "utils/Uuid.h"

namespace idtx
{
namespace session
{

SessionManager::SessionManager(idtx::utils::UsdFileLocator file_locator)
    : m_locator_(std::move(file_locator))
{}

SessionManager::~SessionManager()
{
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
        if (!session) continue;
        if (session->listener) session->listener->Revoke();

        if (session->stage)
        {
            try
            {
                if (auto session_layer = session->stage->GetSessionLayer())
                {
                    session_layer->Clear();
                }
            }
            catch (const std::exception& e)
            {
                IDTX_LOG(IDTX_WARN,
                         "SessionManager dtor: failed to clear session layer for {}: {}",
                         id, e.what());
            }
            catch (...)
            {
                IDTX_LOG(IDTX_WARN,
                         "SessionManager dtor: failed to clear session layer for {} (unknown exception).",
                         id);
            }
        }
    }
    m_sessions_.clear();
}

// ---------------------------------------------------------------------------
// REST-side API
// ---------------------------------------------------------------------------

std::shared_ptr<Session> SessionManager::Create(const std::string& usd_file,
                                                idtx::dto::SessionMode mode,
                                                std::string& out_error,
                                                CreateStatus& out_status)
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

    pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(resolved.string());
    if (!stage)
    {
        out_error  = "Failed to open USD stage";
        out_status = CreateStatus::StageOpenFailed;
        IDTX_LOG(IDTX_ERROR, "UsdStage::Open returned null for '{}'.", resolved.string());
        return nullptr;
    }

    // Redirect all subsequent stage authoring (e.g. TransformDispatcher::Apply)
    // to the anonymous session layer so client edits compose *above* the root
    // layer. This gives us two things:
    //   1. `SdfLayer::Reload(force=true)` on the root layer (used by
    //      ReloadSessionsForFile when a client uploads a replacement file)
    //      does not clobber live session-authored overrides.
    //   2. StageNoticeListener can decide whether the on-disk change is
    //      visible to the client by inspecting the session layer directly
    stage->SetEditTarget(pxr::UsdEditTarget(stage->GetSessionLayer()));

    auto session        = std::make_shared<Session>();
    session->id         = idtx::utils::GenerateUuidV4();
    session->usd_file   = usd_file;
    session->stage      = stage;
    session->mode       = mode;
    session->created_at = std::chrono::system_clock::now();

    session->listener = std::make_shared<StageNoticeListener>(this, std::weak_ptr<Session>(session));
    session->listener->Register();

    {
        std::unique_lock lk(m_mutex_);
        m_sessions_.emplace(session->id, session);
    }

    out_status = CreateStatus::Ok;
    IDTX_LOG(IDTX_INFO, "Created session {} for '{}'.", session->id, usd_file);
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
    std::unique_lock lk(m_mutex_);
    auto it = m_sessions_.find(id);
    if (it == m_sessions_.end()) return false;

    if (it->second && it->second->listener) it->second->listener->Revoke();
    m_sessions_.erase(it);
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

} // namespace

void SessionManager::BroadcastResolvedTransform(const std::shared_ptr<Session>& session,
                                                const std::string& prim_path,
                                                crow::websocket::connection* origin)
{
    if (!session || !session->stage) return;

    idtxcore::SeparateTransform sep;
    idtxcore::Matrix4dTransform mat;
    bool use_matrix = false;
    if (!ReadResolvedTransform(session->stage, prim_path, sep, mat, use_matrix))
    {
        IDTX_LOG(IDTX_DEBUG, "Skip broadcast: cannot read transform on '{}'.", prim_path);
        return;
    }

    idtxcore::BaseMessage msg;
    msg.set_session_id(session->id);
    auto* bcast = msg.mutable_xform_broadcast();
    bcast->set_client_id(""); // server-originated; clients ignore based on origin connection

    auto* upd = bcast->mutable_update();
    upd->set_session_id(session->id);
    upd->set_usd_file(session->usd_file);
    upd->set_prim_path(prim_path);
    upd->set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (use_matrix) *upd->mutable_matrix()  = std::move(mat);
    else            *upd->mutable_seperate()= std::move(sep);

    std::string payload;
    if (!msg.SerializeToString(&payload))
    {
        IDTX_LOG(IDTX_ERROR, "Failed to serialize TransformBroadcast for '{}'.", prim_path);
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

} // namespace session
} // namespace idtx
