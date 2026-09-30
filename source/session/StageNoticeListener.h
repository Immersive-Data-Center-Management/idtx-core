/**
 * @file StageNoticeListener.h
 * @brief TfNotice-based observer that turns USD stage changes into
 *        TransformBroadcast messages for collaborating clients.
 *
 * This is the egress half of the collaboration loop. The listener is
 * registered against a single UsdStage when a session is created and
 * revoked when the session is destroyed. Whenever the stage authoring
 * results in a `UsdNotice::ObjectsChanged` notification, the listener
 * inspects the changed paths, classifies any transform-related changes,
 * reads the resolved transform back from the stage, and asks
 * SessionManager to broadcast it.
 *
 * Notices are delivered on the thread that authored the change, which is the
 * session's command consumer. Origin suppression uses Session::current_origin,
 * which the consumer sets while it applies a client's TransformUpdate; the
 * listener skips echoing the change back to that connection. Every notice
 * that affects at least one prim advances Session::server_seq once, and all
 * broadcasts for that notice carry the new value. Every broadcast prim is
 * recorded in Session::versions with that value and the origin as writer.
 */
#pragma once

#include <memory>

#include <pxr/pxr.h>
#include <pxr/base/tf/notice.h>
#include <pxr/base/tf/weakBase.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/stage.h>

#include <idtx/utils/Logger.h>

namespace idtx
{
namespace session
{

struct Session;
class  SessionManager;

class StageNoticeListener : public pxr::TfWeakBase
{
    IDTX_LOG_CATEGORY("StageNoticeListener")

public:
    StageNoticeListener(SessionManager* manager, std::weak_ptr<Session> session);
    ~StageNoticeListener();

    /// Register this listener against the session's stage. Stores the
    /// resulting TfNotice::Key on the listener so we can revoke it on
    /// destruction.
    void Register();

    /// Revoke the registration if currently active.
    void Revoke();

    /// TfNotice callback. Invoked on the thread that authored the change.
    void OnObjectsChanged(const pxr::UsdNotice::ObjectsChanged& notice,
                          const pxr::UsdStageWeakPtr& sender);

private:
    SessionManager*           m_manager_;     // non-owning, outlives the listener
    std::weak_ptr<Session>    m_session_;
    pxr::TfNotice::Key        m_key_;
    bool                      m_registered_ = false;
};

} // namespace session
} // namespace idtx