/**
 * @file AuthoringContext.h
 * @brief What the current thread is doing to USD, for StageNoticeListener.
 */
#pragma once

#include <algorithm>
#include <vector>

#include "SessionCommand.h"

namespace idtx::session
{

struct Session;

/**
 * @brief Describes the operation running on this thread.
 *
 * UsdNotice::ObjectsChanged is delivered synchronously on the thread that
 * changed a layer, which is not necessarily the consumer of the session
 * whose stage sends it: a reload of a shared layer recomposes every stage
 * that uses it on the reloading thread. Whether a notice is a reload, which
 * client caused it and whether it is a prim creation or an active-state
 * change are therefore properties of the sending thread and are read from
 * here rather than from the receiving session.
 */
struct AuthoringContext
{
    // Session whose command runs on this thread; null outside a command.
    const Session* session = nullptr;

    // Connection whose change is being applied; 0 otherwise.
    ConnectionId origin = 0;

    // A prim creation is being authored. It is broadcast explicitly, so the
    // listener ignores the resync it fires.
    bool creating = false;

    // A prim's active state (soft delete or its undo) is being authored. It
    // is broadcast explicitly, and the listener must ignore the resync: an
    // inactive prim is still readable via GetPrimAtPath, so the transform
    // path would broadcast a misleading frame in place of the delete.
    bool setting_active = false;

    // A shared layer is being reloaded; every stage using it is notified.
    bool reload = false;

    // During a reload: the sessions paused for it. A notice for any other
    // session means its cached layer list was out of date.
    const std::vector<const Session*>* paused = nullptr;

    [[nodiscard]]
    bool IsPaused(const Session* candidate) const noexcept
    {
        return paused
            && std::find(paused->begin(), paused->end(), candidate) != paused->end();
    }
};

namespace detail
{
inline thread_local AuthoringContext t_authoring_context;
}

[[nodiscard]]
inline const AuthoringContext& CurrentAuthoringContext() noexcept
{
    return detail::t_authoring_context;
}

/// Sets the context for its lifetime and restores the previous one.
class AuthoringScope final
{
public:
    explicit AuthoringScope(const AuthoringContext& context) noexcept
        : m_previous_(detail::t_authoring_context)
    {
        detail::t_authoring_context = context;
    }

    ~AuthoringScope()
    {
        detail::t_authoring_context = m_previous_;
    }

    AuthoringScope(const AuthoringScope&)            = delete;
    AuthoringScope& operator=(const AuthoringScope&) = delete;

private:
    AuthoringContext m_previous_;
};

} // namespace idtx::session
