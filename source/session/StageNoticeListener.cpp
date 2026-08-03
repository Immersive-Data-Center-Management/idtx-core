#include "StageNoticeListener.h"

#include <set>
#include <string>
#include <vector>

#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

#include "Session.h"
#include "SessionManager.h"

namespace idtx
{
namespace session
{

namespace
{

// Heuristic to identify a property change that affects a prim's local
// transform. Covers both UsdGeomXformCommonAPI ops and arbitrary xformOps,
// as well as the xformOpOrder attribute itself.
bool IsXformAttribute(const pxr::SdfPath& path)
{
    if (!path.IsPropertyPath()) return false;
    const std::string name = path.GetName();
    if (name == "xformOpOrder") return true;
    return name.rfind("xformOp:", 0) == 0;
}

// Return true when @p prim_spec (which lives on the session layer) authors
// any transform-related attribute opinion for the prim, so the client
// already sees a value that wins by composition strength over anything the
// root layer might supply after a reload.
//
// Transform-related means either the ordering attribute (`xformOpOrder`) or
// any concrete op (`xformOp:*`). This mirrors IsXformAttribute() above so
// the "reload" branch stays consistent with the "authored on stage" branch.
bool SessionLayerHasXformOverride(const pxr::SdfPrimSpecHandle& prim_spec)
{
    if (!prim_spec) return false;
    for (const auto& attr_spec : prim_spec->GetAttributes())
    {
        if (!attr_spec) continue;
        const std::string name = attr_spec->GetName();
        if (name == "xformOpOrder") return true;
        if (name.rfind("xformOp:", 0) == 0) return true;
    }
    return false;
}

} // namespace

StageNoticeListener::StageNoticeListener(SessionManager* manager,
                                         std::weak_ptr<Session> session)
    : m_manager_(manager)
    , m_session_(std::move(session))
{}

StageNoticeListener::~StageNoticeListener()
{
    Revoke();
}

void StageNoticeListener::Register()
{
    if (m_registered_) return;
    auto session = m_session_.lock();
    if (!session || !session->stage)
    {
        IDTX_LOG(IDTX_WARN, "StageNoticeListener::Register called without a valid session/stage.");
        return;
    }

    // Register against the specific stage so other sessions' notices don't
    // reach this listener.
    m_key_ = pxr::TfNotice::Register(
        pxr::TfWeakPtr<StageNoticeListener>(this),
        &StageNoticeListener::OnObjectsChanged,
        session->stage);
    m_registered_ = true;
    IDTX_LOG(IDTX_DEBUG, "TfNotice listener registered for session {}.", session->id);
}

void StageNoticeListener::Revoke()
{
    if (!m_registered_) return;
    pxr::TfNotice::Revoke(m_key_);
    m_registered_ = false;
}

void StageNoticeListener::OnObjectsChanged(const pxr::UsdNotice::ObjectsChanged& notice,
                                           const pxr::UsdStageWeakPtr& /*sender*/)
{
    auto session = m_session_.lock();
    if (!session || !m_manager_) return;

    // Capture and immediately clear the origin so a re-entrant authoring path
    // (should not happen, but be safe) doesn't suppress unrelated broadcasts.
    crow::websocket::connection* origin = session->last_origin;
    session->last_origin = nullptr;

    // -----------------------------------------------------------------
    // Reload branch (§3.4 of docs/plans/reload-on-upload.md).
    //
    // SessionManager::ReloadSessionsForFile flipped `reload_in_progress`
    // right before calling SdfLayer::Reload(force=true) on the root
    // layer. In that notice the affected paths reflect *authored* changes
    // on the root layer, but the resolved value on the composed stage may
    // still be masked by an opinion on the session layer (where every
    // client-driven edit lives — see §3.1). We must therefore consult the
    // session layer directly and drop any broadcast whose value the
    // client already holds by composition strength.
    //
    // The branch is attribute-agnostic by design: it decides "propagate
    // vs. suppress" based on the presence of a session-layer opinion for
    // the prim, and only relies on the transform-specific
    // BroadcastResolvedTransform() at the very last step. That keeps the
    // door open for non-transform attributes later, at which point only
    // the final broadcast call has to grow a switch.
    // -----------------------------------------------------------------
    if (session->reload_in_progress)
    {
        const pxr::SdfLayerHandle session_layer =
            session->stage ? session->stage->GetSessionLayer() : pxr::SdfLayerHandle();

        std::set<std::string> reload_prims;
        for (const pxr::SdfPath& p : notice.GetResyncedPaths())
        {
            if (p.IsPrimPath())              reload_prims.insert(p.GetString());
            else if (p.IsPropertyPath())     reload_prims.insert(p.GetPrimPath().GetString());
        }
        for (const pxr::SdfPath& p : notice.GetChangedInfoOnlyPaths())
        {
            if (p.IsPrimPath())              reload_prims.insert(p.GetString());
            else if (p.IsPropertyPath())     reload_prims.insert(p.GetPrimPath().GetString());
        }

        // Consume the flag before we start broadcasting. If a nested notice
        // fires from within a broadcast (should not, but defensive), it
        // would then travel the normal branch.
        session->reload_in_progress = false;

        if (reload_prims.empty())
        {
            IDTX_LOG(IDTX_DEBUG,
                     "Reload notice in session {} carried no affected prim paths.",
                     session->id);
            return;
        }

        IDTX_LOG(IDTX_DEBUG,
                 "Reload notice in session {} affects {} prim(s); filtering against session layer.",
                 session->id, reload_prims.size());

        for (const auto& prim_path_str : reload_prims)
        {
            // If the session layer authors a transform override for the
            // prim, the client already holds the winning opinion — skip.
            if (session_layer && pxr::SdfPath::IsValidPathString(prim_path_str))
            {
                const pxr::SdfPrimSpecHandle prim_spec =
                    session_layer->GetPrimAtPath(pxr::SdfPath(prim_path_str));
                if (SessionLayerHasXformOverride(prim_spec))
                {
                    IDTX_LOG(IDTX_DEBUG,
                             "Suppress reload broadcast for '{}' in session {}: "
                             "session layer holds stronger xform opinion.",
                             prim_path_str, session->id);
                    continue;
                }
            }

            // No conflicting override → propagate the current resolved
            // transform, exactly like the normal branch would.
            m_manager_->BroadcastResolvedTransform(session, prim_path_str, /*origin=*/nullptr);
        }
        return;
    }

    // Collect the affected prim paths once. xformOp authoring shows up under
    // GetChangedInfoOnlyPaths() (it is a property value/metadata change) but
    // some workflows (re-creating ops from scratch) can also produce resyncs.
    std::set<std::string> affected_prims;

    for (const pxr::SdfPath& p : notice.GetChangedInfoOnlyPaths())
    {
        if (IsXformAttribute(p))
        {
            affected_prims.insert(p.GetPrimPath().GetString());
        }
    }
    for (const pxr::SdfPath& p : notice.GetResyncedPaths())
    {
        if (p.IsPrimPath())
        {
            // We don't know which property changed in a resync; broadcasting
            // the prim's resolved transform is safe and idempotent.
            affected_prims.insert(p.GetString());
        }
        else if (IsXformAttribute(p))
        {
            affected_prims.insert(p.GetPrimPath().GetString());
        }
    }

    if (affected_prims.empty()) return;

    IDTX_LOG(IDTX_DEBUG, "Stage change in session {} affects {} prim(s); broadcasting.",
             session->id, affected_prims.size());

    for (const auto& prim_path : affected_prims)
    {
        m_manager_->BroadcastResolvedTransform(session, prim_path, origin);
    }
}

} // namespace session
} // namespace idtx