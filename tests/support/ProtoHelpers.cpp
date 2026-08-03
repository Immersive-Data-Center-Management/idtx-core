#include "ProtoHelpers.h"

#include <chrono>

#include "WsTestClient.h"

namespace idtx::tests
{

std::string BuildTransformUpdate(const std::string& session_id,
                                 const std::string& usd_file,
                                 const std::string& prim_path,
                                 double tx, double ty, double tz)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session_id);

    auto* upd = msg.mutable_xform_update();
    upd->set_session_id(session_id);
    upd->set_usd_file(usd_file);
    upd->set_prim_path(prim_path);
    upd->set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto* sep = upd->mutable_seperate();
    sep->mutable_translation()->set_x(tx);
    sep->mutable_translation()->set_y(ty);
    sep->mutable_translation()->set_z(tz);
    sep->mutable_rotation()->set_x(0.0);
    sep->mutable_rotation()->set_y(0.0);
    sep->mutable_rotation()->set_z(0.0);
    sep->mutable_scale()->set_x(1.0);
    sep->mutable_scale()->set_y(1.0);
    sep->mutable_scale()->set_z(1.0);

    std::string out;
    msg.SerializeToString(&out);
    return out;
}

bool ParseBaseMessage(const std::string& bytes, idtxcore::BaseMessage& out)
{
    return out.ParseFromString(bytes);
}

std::optional<idtxcore::BaseMessage> WaitForMessage(
    WsTestClient& ws,
    std::chrono::milliseconds timeout,
    const std::function<bool(const idtxcore::BaseMessage&)>& pred)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;

        auto frame = ws.ReadBinary(remaining);
        if (!frame) return std::nullopt; // timed out

        idtxcore::BaseMessage msg;
        if (!ParseBaseMessage(*frame, msg)) continue;
        if (pred(msg)) return msg;
        // Otherwise loop and try to read the next frame.
    }
    return std::nullopt;
}

} // namespace idtx::tests