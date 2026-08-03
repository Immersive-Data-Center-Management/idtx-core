// tests/support/ProtoHelpers.h — small helpers to build and inspect
// idtxcore::BaseMessage payloads in tests.

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include <idtx/proto/base.pb.h>
#include <idtx/proto/transform.pb.h>

namespace idtx::tests
{

class WsTestClient;

/// Build a serialized BaseMessage containing a TransformUpdate with a
/// separate-translation only (rotation & scale identity).
std::string BuildTransformUpdate(const std::string& session_id,
                                 const std::string& usd_file,
                                 const std::string& prim_path,
                                 double tx, double ty, double tz);

/// Parse a wire-format BaseMessage. Returns true on success.
bool ParseBaseMessage(const std::string& bytes, idtxcore::BaseMessage& out);

/// Read binary frames from @p ws until @p pred returns true for the decoded
/// message or the total elapsed time exceeds @p timeout.
std::optional<idtxcore::BaseMessage> WaitForMessage(
    WsTestClient& ws,
    std::chrono::milliseconds timeout,
    const std::function<bool(const idtxcore::BaseMessage&)>& pred);

} // namespace idtx::tests