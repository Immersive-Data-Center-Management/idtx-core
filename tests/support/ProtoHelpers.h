// tests/support/ProtoHelpers.h - small helpers to build and inspect
// idtxcore::BaseMessage payloads in tests.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <idtx/proto/base.pb.h>
#include <idtx/proto/transform.pb.h>
#include <idtx/proto/prim.pb.h>

namespace idtx::tests
{

class WsTestClient;

/// Build a serialized BaseMessage containing a TransformUpdate with a
/// separate-translation only (rotation & scale identity). @p request_id is
/// set on the enclosing BaseMessage and echoed back in the Ack. @p base is
/// sent as BaseMessage.server_seq: the highest server_seq the client has
/// received.
std::string BuildTransformUpdate(const std::string& session_id,
                                 const std::string& usd_file,
                                 const std::string& prim_path,
                                 double tx, double ty, double tz,
                                 std::uint64_t request_id = 0,
                                 std::uint64_t base = 0);

/// Build a serialized BaseMessage containing a CreatePrimRequest for @p type
/// under @p parent_path. @p request_id is set on the enclosing BaseMessage and
/// echoed back in the Ack.
std::string BuildCreatePrimRequest(const std::string& session_id,
                                   const std::string& parent_path,
                                   idtxcore::PrimType type,
                                   std::uint64_t request_id = 0);

/// Build a serialized BaseMessage containing a SetPrimActiveRequest for
/// @p prim_path. @p active == false is a soft delete, true is its undo.
/// @p request_id is set on the enclosing BaseMessage and echoed back in the Ack.
std::string BuildSetPrimActiveRequest(const std::string& session_id,
                                      const std::string& prim_path,
                                      bool active,
                                      std::uint64_t request_id = 0);

/// Parse a wire-format BaseMessage. Returns true on success.
bool ParseBaseMessage(const std::string& bytes, idtxcore::BaseMessage& out);

/// Read binary frames from @p ws until @p pred returns true for the decoded
/// message or the total elapsed time exceeds @p timeout.
std::optional<idtxcore::BaseMessage> WaitForMessage(
    WsTestClient& ws,
    std::chrono::milliseconds timeout,
    const std::function<bool(const idtxcore::BaseMessage&)>& pred);

} // namespace idtx::tests