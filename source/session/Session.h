/**
 * @file Session.h
 * @brief Per-collaboration session state owned by SessionManager.
 *
 * A Session bundles:
 *   - the immutable identity (UUID, USD file path under uploads/)
 *   - the authoritative server-side UsdStage
 *   - the set of currently connected websocket clients
 *   - the synchronization primitives needed to safely mutate either
 *
 * The class deliberately exposes its members directly: it is an aggregate
 * used by SessionManager and StageNoticeListener, not a public API. All
 * locking discipline is enforced by SessionManager.
 */
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_set>

#include <crow/websocket.h>

#include <pxr/pxr.h>
#include <pxr/base/tf/notice.h>
#include <pxr/usd/usd/stage.h>

#include "dto/SessionDto.h"

namespace idtx
{
namespace session
{

class StageNoticeListener; // forward decl, defined in StageNoticeListener.h

struct Session
{
    std::string                                       id;
    std::string                                       usd_file;       // relative to uploads/
    pxr::UsdStageRefPtr                               stage;
    std::chrono::system_clock::time_point             created_at = std::chrono::system_clock::now();

    // Editing/runtime mode of the session (see idtx::dto::SessionMode). This
    // is populated by SessionManager::Create() from the client's REST body
    // and is used by WebSocketController/SessionManager to enforce single-
    // editor semantics.
    idtx::dto::SessionMode                            mode = idtx::dto::SessionMode::SingleEdit;

    // Serialize stage writes (USD permits concurrent reads, but the
    // application is responsible for serializing writes).
    std::mutex                                        stage_mutex;

    // Protect the connected-client registry. Broadcast iterates under shared
    // lock; attach/detach take exclusive.
    mutable std::shared_mutex                         clients_mutex;
    std::unordered_set<crow::websocket::connection*>  clients;

    // The connection that last initiated a stage authoring action. This is
    // set under stage_mutex by SessionManager::ApplyTransformUpdate so that
    // the TfNotice listener can suppress the broadcast back to the origin.
    // Cleared after the listener has had a chance to observe it.
    crow::websocket::connection*                      last_origin = nullptr;

    // The listener that observes stage changes for this session and drives
    // the broadcast (Variant B). It owns the pxr::TfNotice::Key and revokes
    // the registration on destruction.
    std::shared_ptr<StageNoticeListener>              listener;

    // Set by SessionManager::ReloadSessionsForFile() immediately before it
    // calls SdfLayer::Reload(force=true) on the root layer, and cleared by
    // StageNoticeListener::OnObjectsChanged() after it has processed the
    // resulting notice. When the flag is true the listener takes the
    // "reload" branch: it compares each affected prim's authored opinions
    // on the session layer against those on the root layer and drops the
    // broadcast for any attribute whose session-layer opinion still wins
    // by composition strength. Reads/writes only happen while
    // stage_mutex is held (TfNotice fires synchronously on the authoring
    // thread), so no additional synchronisation is needed.
    bool                                              reload_in_progress = false;
};

} // namespace session
} // namespace idtx