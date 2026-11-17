#include "ProtoHelpers.h"

#include <chrono>

#include "WsTestClient.h"

namespace idtx::tests
{

std::string BuildTransformUpdate(const std::string& session_id,
                                 const std::string& usd_file,
                                 const std::string& prim_path,
                                 double tx, double ty, double tz,
                                 const std::uint64_t request_id,
                                 const std::uint64_t base)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session_id);
    msg.set_request_id(request_id);
    msg.set_server_seq(base);

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

std::string BuildCreatePrimRequest(const std::string& session_id,
                                   const std::string& parent_path,
                                   idtxcore::PrimType type,
                                   const std::uint64_t request_id)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session_id);
    msg.set_request_id(request_id);

    auto* req = msg.mutable_create_prim();
    req->set_session_id(session_id);
    req->set_parent_path(parent_path);
    req->set_type(type);

    std::string out;
    msg.SerializeToString(&out);
    return out;
}

std::string BuildCreatePrimRequestWithTranslate(const std::string& session_id,
                                                const std::string& parent_path,
                                                idtxcore::PrimType type,
                                                double tx, double ty, double tz,
                                                const std::uint64_t request_id)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session_id);
    msg.set_request_id(request_id);

    auto* req = msg.mutable_create_prim();
    req->set_session_id(session_id);
    req->set_parent_path(parent_path);
    req->set_type(type);

    auto* sep = req->mutable_seperate();
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

std::string BuildCreatePrimReferenceRequest(const std::string& session_id,
                                            const std::string& parent_path,
                                            idtxcore::PrimRefType ref_type,
                                            const std::string& reference_path,
                                            const std::uint64_t request_id)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session_id);
    msg.set_request_id(request_id);

    auto* req = msg.mutable_create_prim();
    req->set_session_id(session_id);
    req->set_parent_path(parent_path);
    req->set_type(idtxcore::PRIM_TYPE_REF);
    req->set_ref_type(ref_type);
    req->set_reference_path(reference_path);

    std::string out;
    msg.SerializeToString(&out);
    return out;
}

std::string BuildSetPrimActiveRequest(const std::string& session_id,
                                      const std::string& prim_path,
                                      bool active,
                                      const std::uint64_t request_id)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session_id);
    msg.set_request_id(request_id);

    auto* req = msg.mutable_set_prim_active();
    req->set_session_id(session_id);
    req->set_prim_path(prim_path);
    req->set_active(active);

    std::string out;
    msg.SerializeToString(&out);
    return out;
}

bool ParseBaseMessage(const std::string& bytes, idtxcore::BaseMessage& out)
{
    return out.ParseFromString(bytes);
}

idtxcore::PrimAttribute MakeDoubleAttr(const std::string& name, const std::string& type_name,
                                       const std::vector<double>& comps, bool is_array)
{
    idtxcore::PrimAttribute a;
    a.set_name(name);
    a.set_type_name(type_name);
    auto* v = a.mutable_value();
    v->set_is_array(is_array);
    for (double d : comps) v->add_doubles(d);
    return a;
}

idtxcore::PrimAttribute MakeIntAttr(const std::string& name, const std::string& type_name,
                                    const std::vector<std::int64_t>& comps, bool is_array)
{
    idtxcore::PrimAttribute a;
    a.set_name(name);
    a.set_type_name(type_name);
    auto* v = a.mutable_value();
    v->set_is_array(is_array);
    for (std::int64_t i : comps) v->add_ints(i);
    return a;
}

idtxcore::PrimAttribute MakeStringAttr(const std::string& name, const std::string& type_name,
                                       const std::vector<std::string>& vals, bool is_array)
{
    idtxcore::PrimAttribute a;
    a.set_name(name);
    a.set_type_name(type_name);
    auto* v = a.mutable_value();
    v->set_is_array(is_array);
    for (const auto& s : vals) v->add_strings(s);
    return a;
}

idtxcore::PrimAttribute MakeBoolAttr(const std::string& name, const std::string& type_name,
                                     const std::vector<bool>& vals, bool is_array)
{
    idtxcore::PrimAttribute a;
    a.set_name(name);
    a.set_type_name(type_name);
    auto* v = a.mutable_value();
    v->set_is_array(is_array);
    for (bool b : vals) v->add_bools(b);
    return a;
}

std::string BuildSetPrimAttributesRequest(const std::string& session_id,
                                          const std::string& prim_path,
                                          const std::vector<idtxcore::PrimAttribute>& attrs,
                                          const std::uint64_t request_id)
{
    idtxcore::BaseMessage msg;
    msg.set_session_id(session_id);
    msg.set_request_id(request_id);
    auto* req = msg.mutable_set_prim_attributes();
    req->set_session_id(session_id);
    req->set_prim_path(prim_path);
    for (const auto& a : attrs) *req->add_attributes() = a;
    std::string out;
    msg.SerializeToString(&out);
    return out;
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