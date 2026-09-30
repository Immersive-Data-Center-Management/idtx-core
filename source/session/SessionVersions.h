/**
 * @file SessionVersions.h
 * @brief Per-session version bookkeeping that decides whether a client's
 *        update was made on the current state.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "SessionCommand.h"

namespace idtx::session
{

/** @brief Result of checking the base of a client's update. */
enum class UpdateBaseStatus : std::uint8_t
{
    Current,
    // The prim was changed by someone else, or a correction for it was sent
    // to the client, after the base.
    Stale,
    // The base is higher than any version sent to the client.
    InvalidBase,
};

/**
 * @brief Records which versions (server_seq values) changed which prim and
 *        which versions were sent to which connection.
 *
 * A client's TransformUpdate carries a base: the highest server_seq the client
 * had received and applied when it created the update. CheckUpdate() rejects
 * the update if the base is higher than anything sent to the client, or if
 * the prim changed after the base by anyone other than the client itself, or
 * if a correction for the prim was sent to the client after the base. The
 * client's own earlier changes never make its update stale, so a continuous
 * drag is not rejected by itself.
 *
 * Entries are never removed; they are small and freed with the session.
 *
 * Used by the session's consumer. The methods take an internal mutex because
 * a change notice from a root layer shared with another session can be
 * delivered on that session's consumer thread; otherwise it is uncontended.
 * Submitting commands never touches this class.
 */
class SessionVersions
{
public:
    [[nodiscard]]
    UpdateBaseStatus CheckUpdate(ConnectionId       sender,
                                 const std::string& prim_path,
                                 std::uint64_t      base) const;

    /**
     * @brief Record that @p prim_path changed with version @p seq.
     *
     * @param writer Connection whose update caused the change, 0 for changes
     *               made by the server (reloads).
     */
    void RecordChange(const std::string& prim_path, std::uint64_t seq, ConnectionId writer);

    /** @brief Record that a message carrying @p seq was sent to @p connection. */
    void RecordSent(ConnectionId connection, std::uint64_t seq);

    /**
     * @brief Record that a correction for @p prim_path with version @p seq was
     *        sent to @p connection only. Implies RecordSent().
     */
    void RecordCorrection(ConnectionId connection, const std::string& prim_path, std::uint64_t seq);

private:
    struct PrimVersion
    {
        std::uint64_t last_change_seq = 0;
        ConnectionId  last_writer     = 0;
        // last_change_seq before last_writer's current run of consecutive
        // changes, i.e. the latest change last_writer did not make.
        std::uint64_t foreign_before_run = 0;
    };

    struct ConnectionVersions
    {
        std::uint64_t                                  max_sent_seq = 0;
        std::unordered_map<std::string, std::uint64_t> correction_seq;
    };

    mutable std::mutex                                 mutex_;
    std::unordered_map<std::string, PrimVersion>       prims_;
    std::unordered_map<ConnectionId, ConnectionVersions> connections_;
};

} // namespace idtx::session
