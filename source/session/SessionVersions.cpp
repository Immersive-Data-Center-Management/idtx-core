#include "SessionVersions.h"

#include <algorithm>

namespace idtx::session
{

UpdateBaseStatus SessionVersions::CheckUpdate(const ConnectionId  sender,
                                              const std::string&  prim_path,
                                              const std::uint64_t base) const
{
    std::lock_guard lock(mutex_);

    std::uint64_t max_sent   = 0;
    std::uint64_t correction = 0;
    if (const auto conn = connections_.find(sender); conn != connections_.end())
    {
        max_sent = conn->second.max_sent_seq;
        if (const auto corr = conn->second.correction_seq.find(prim_path);
            corr != conn->second.correction_seq.end())
        {
            correction = corr->second;
        }
    }

    // Only the server advances the version and the base is the version of a
    // state the client received, so an honest client can never exceed it.
    if (base > max_sent) return UpdateBaseStatus::InvalidBase;

    std::uint64_t latest_foreign = 0;
    if (const auto prim = prims_.find(prim_path); prim != prims_.end())
    {
        latest_foreign = prim->second.last_writer == sender
            ? prim->second.foreign_before_run
            : prim->second.last_change_seq;
    }

    return base < std::max(latest_foreign, correction)
        ? UpdateBaseStatus::Stale
        : UpdateBaseStatus::Current;
}

void SessionVersions::RecordChange(const std::string& prim_path,
                                   const std::uint64_t seq,
                                   const ConnectionId  writer)
{
    std::lock_guard lock(mutex_);

    PrimVersion& prim = prims_[prim_path];
    // A server-side change (writer 0) always starts a new run, so it is
    // foreign to every connection.
    if (writer == 0 || prim.last_writer != writer)
    {
        prim.foreign_before_run = prim.last_change_seq;
        prim.last_writer        = writer;
    }
    prim.last_change_seq = seq;
}

void SessionVersions::RecordSent(const ConnectionId connection, const std::uint64_t seq)
{
    std::lock_guard lock(mutex_);

    std::uint64_t& max_sent = connections_[connection].max_sent_seq;
    max_sent = std::max(max_sent, seq);
}

void SessionVersions::RecordCorrection(const ConnectionId  connection,
                                       const std::string&  prim_path,
                                       const std::uint64_t seq)
{
    std::lock_guard lock(mutex_);

    ConnectionVersions& versions = connections_[connection];
    versions.max_sent_seq = std::max(versions.max_sent_seq, seq);
    versions.correction_seq[prim_path] = seq;
}

} // namespace idtx::session
