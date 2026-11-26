/**
 * @file AttributeDispatcher.h
 * @brief Validate and author a batch of arbitrary typed attribute default
 *        values on one prim, with per-attribute (partial) success.
 *
 * This is the single place that turns a SetPrimAttributesRequest into USD
 * authoring on the session's stage, isolating USD-schema/Sdf concerns from
 * SessionManager exactly as TransformDispatcher / PrimDispatcher do. Value
 * (de)serialisation is delegated to ProtoVtCodec so decode/encode stay
 * symmetric and pinned by one round-trip test.
 *
 * Scope (plan §0): default-value authoring on active, defined, composed prims.
 * Relationships, connections, metadata, time samples, clears/blocks, schema
 * application and transforms (xformOp:* / xformOpOrder -> TransformUpdate) are
 * out of scope and rejected.
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <pxr/pxr.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/usd/stage.h>

#include <idtx/utils/Logger.h>

#include <idtx/proto/attribute.pb.h>

namespace idtx
{
namespace session
{

/**
 * @brief Scene-root asset context for anchoring asset-valued attributes.
 *
 * A client sends asset paths relative to the shared scene root, not the
 * server-only sidecar. The dispatcher authors a server-absolute path (for
 * files hosted under the uploads root) and broadcasts/reads back a portable
 * root-relative path. See plan §2.5.
 */
struct AttributeAssetContext
{
    std::filesystem::path root_layer_dir;  ///< Directory of the stage's root layer.
    std::filesystem::path uploads_root;    ///< Server uploads root ("root download folder").
};

class AttributeDispatcher
{
    IDTX_LOG_CATEGORY("AttributeDispatcher")

public:
    /// 1:1 with proto AttributeFailureReason (authoring subset). Transport
    /// reasons (QueueFull, SessionClosing, RequestTooLarge) are produced by the
    /// routing/admission layer, not here.
    enum class FailureReason
    {
        None,
        PrimNotFound,
        PrimNotEditable,
        InvalidName,
        ReservedAttribute,
        PropertyKindMismatch,
        UnknownType,
        UnsupportedType,
        TypeMismatch,
        InvalidValue,
        NotAuthorable,
    };

    static const char*                      FailureReasonToString(FailureReason) noexcept;
    static idtxcore::AttributeFailureReason ToProto(FailureReason) noexcept;

    struct AttributeResult
    {
        std::string             name;
        bool                    ok = false;
        FailureReason           reason = FailureReason::None;
        std::string             detail; ///< Optional human-readable failure context.
        pxr::SdfValueTypeName   type;   ///< Effective USD type (role + array-ness).
        pxr::VtValue            value;  ///< Decoded, normalized value to author.
        idtxcore::PrimAttribute wire;   ///< Role-preserving broadcast value, pre-encoded.
    };

    struct AuthoringResult
    {
        bool                         authored_any = false; ///< The session layer was mutated.
        std::vector<AttributeResult> attributes;           ///< One per request entry, in order.
    };

    /**
     * @brief Validate and author @p req's attributes onto its prim.
     *
     * Call from the session consumer, with the stage's edit target set to the
     * session layer. All composed-stage reads (validation) happen first; only
     * Sdf authoring runs inside the single change block. Allocates result
     * metadata, canonical wire values and the undo journal before the first
     * mutation, so a post-write allocation cannot turn an applied entry into a
     * failure. Never throws.
     *
     * @return One result per requested attribute (request order); authored_any
     *         tells the caller whether to bump server_seq / broadcast at all.
     */
    static AuthoringResult Author(const pxr::UsdStageRefPtr& stage,
                                  const idtxcore::SetPrimAttributesRequest& req,
                                  const AttributeAssetContext& assets);

    /**
     * @brief Encode ONE authored attribute default spec for the join snapshot,
     *        as a pure Sdf read (no stage composition).
     *
     * @param attr   An authored attribute spec on the session layer.
     * @param assets Asset context for server-absolute -> root-relative rewrite.
     * @param out    Filled with the role-preserving wire value on success.
     * @return false when the spec has no default, or encoding/asset-context
     *         conversion fails. The caller (snapshot) pre-checks HasDefaultValue
     *         and treats a false here (present default) as a hard failure.
     */
    static bool ReadAuthoredAttribute(const pxr::SdfAttributeSpecHandle& attr,
                                      const AttributeAssetContext& assets,
                                      idtxcore::PrimAttribute& out);
};

} // namespace session
} // namespace idtx
