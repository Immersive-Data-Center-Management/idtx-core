/**
 * @file Session.h
 * @brief Per-collaboration session state owned by SessionManager.
 *
 * A Session bundles:
 *   - the immutable identity (UUID, USD file path under uploads/)
 *   - the authoritative server-side UsdStage
 *   - the set of currently connected websocket clients
 *   - the command queue through which every stage access is serialized
 *
 * The class deliberately exposes its members directly: it is an aggregate
 * used by SessionManager and StageNoticeListener, not a public API.
 *
 * Threading model: the stage and every field documented as "consumer-only"
 * are touched exclusively by the session's command consumer (the handler
 * driven by @c commands), which runs at most one command at a time. They
 * therefore need no locking. The client registry is shared with the
 * websocket threads and is guarded by @c clients_mutex.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <crow/websocket.h>

#include <pxr/pxr.h>
#include <pxr/base/tf/notice.h>
#include <pxr/usd/usd/stage.h>

#include "dto/SessionDto.h"
#include "SessionCommand.h"
#include "SessionCommandScheduler.h"

namespace idtx
{
namespace session
{

class StageNoticeListener; // forward decl, defined in StageNoticeListener.h

struct Session
{
    std::string                                       id;
    std::string                                       usd_file;       // relative to uploads/
    std::filesystem::path                             usd_file_absolute; // resolved on-disk path
    pxr::UsdStageRefPtr                               stage;          // consumer-only
    std::chrono::system_clock::time_point             created_at = std::chrono::system_clock::now();

    // Editing/runtime mode of the session (see idtx::dto::SessionMode). This
    // is populated by SessionManager::Create() from the client's REST body
    // and is used by WebSocketController/SessionManager to enforce single-
    // editor semantics.
    idtx::dto::SessionMode                            mode = idtx::dto::SessionMode::SingleEdit;

    // Per-session command queue. Every read or write of the stage is
    // submitted here and executed by a single consumer on the shared worker
    // executor, in submission order.
    std::shared_ptr<SessionCommandScheduler>          commands;

    // Protect the connected-client registry. Senders look connections up
    // under a shared lock; attach/detach take it exclusively. A connection is
    // only ever dereferenced while this lock is held, so a connection that
    // has been detached can never be written to.
    mutable std::shared_mutex                         clients_mutex;
    std::unordered_map<ConnectionId, crow::websocket::connection*> clients;

    // Set to steady_clock::now() when the client set becomes empty (and at
    // creation, since a new session starts with no clients); reset whenever a
    // client attaches. Read by the SessionManager idle reaper to decide when a
    // session has been unused long enough to destroy. Guarded by clients_mutex.
    std::optional<std::chrono::steady_clock::time_point> empty_since =
        std::chrono::steady_clock::now();

    // The connection whose TransformUpdate is currently being applied, so the
    // TfNotice listener can suppress the echo broadcast back to it. 0 while
    // no client-authored change is in progress. Written by the consumer only;
    // atomic because a notice from a root layer shared with another session
    // can be delivered on that session's consumer thread.
    std::atomic<ConnectionId>                         current_origin{0};

    // The listener that observes stage changes for this session and drives
    // the broadcast. It owns the pxr::TfNotice::Key and revokes the
    // registration on destruction.
    std::shared_ptr<StageNoticeListener>              listener;

    // Set by the reload command immediately before it calls
    // SdfLayer::Reload(force=true) on the root layer, and cleared once the
    // reload returns. While it is true the listener takes the "reload"
    // branch: it compares each affected prim's authored opinions on the
    // session layer against those on the root layer and drops the broadcast
    // for any attribute whose session-layer opinion still wins by
    // composition strength. Written by the consumer only; atomic for the
    // same reason as current_origin.
    std::atomic<bool>                                 reload_in_progress{false};

    // Per-session state version. Incremented once for every stage change
    // observed by the listener and stamped on outgoing broadcasts, acks and
    // snapshots as BaseMessage.server_seq. Atomic for the same reason as
    // current_origin.
    std::atomic<std::uint64_t>                        server_seq{0};

    // On-disk sidecar layer that backs the stage's session layer. Client
    // edits are authored into the session layer, which is this named file,
    // so persisting is a plain SdfLayer::Save(). Convention:
    // "<uploads_root>/sessions/<session-id>.usda". Opening a stage with this
    // file as the session layer on a subsequent run restores prior state.
    std::filesystem::path                             sidecar_path;

    // Set to true whenever a transform update is applied and reset to false
    // once the session layer has been saved to the sidecar. Atomic because
    // the SessionFlusher thread reads it to decide which sessions need a
    // flush command.
    std::atomic<bool>                                 dirty{false};

    // When true, the session's sidecar overrides are committed back into the
    // original USD file when the session is destroyed (by explicit DELETE or
    // the idle reaper). When false, overrides live only in the sidecar until
    // an explicit POST /commit; a teardown with uncommitted overrides logs a
    // warning so the potential data-loss event is visible in monitoring.
    bool                                              auto_commit = false;
};

} // namespace session
} // namespace idtx