/**
 * @file ProtoVtCodec.h
 * @brief Convert between the wire AttributeValue message and pxr::VtValue.
 *
 * This is the single place that maps a generic, lane-based AttributeValue (the
 * protobuf representation used by SetPrimAttributes* messages) onto a concrete
 * USD VtValue of a given SdfValueTypeName, and back. It is shared by:
 *   - AttributeDispatcher (authoring: decode client lanes -> VtValue; encode the
 *     canonical echo value back to lanes), and
 *   - the join-snapshot reader (encode an authored default straight off the
 *     session layer).
 * Keeping decode and encode in one translation unit lets a single round-trip
 * test pin them together, exactly as TransformDispatcher's authored-transform
 * reader is pinned to its author path.
 *
 * Dispatch is on the EFFECTIVE type's scalar C++ representation plus its
 * array-ness (SdfValueTypeName::GetScalarType()/IsArray()). Role types share a
 * handler (color3f/point3f/normal3f/float3 are all GfVec3f; frame4d/matrix4d
 * are GfMatrix4d), so the OWNING SdfValueTypeName — not the VtValue's held C++
 * type — must drive encoding; the caller supplies it and preserves the role.
 *
 * Numeric policy (v1, documented per plan §6.4):
 *   - double components are narrowed to the target scalar (double/float/half).
 *   - A FINITE input that overflows the target float/half range is rejected
 *     (InvalidValue); it never silently becomes +/-inf.
 *   - NaN and +/-inf are representable and pass through unchanged. Signed zero
 *     is preserved. Precision loss on narrowing is accepted silently.
 *   - Integer and unsigned components are range-checked before narrowing; the
 *     signed/unsigned 64-bit boundaries round-trip (separate ints/uints lanes).
 */
#pragma once

#include <pxr/pxr.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/valueTypeName.h>

#include <idtx/utils/Logger.h>

#include <idtx/proto/attribute.pb.h>

namespace idtx
{
namespace session
{

/** @brief Outcome of decoding an AttributeValue into a VtValue. */
enum class DecodeStatus
{
    Ok,
    /// The effective type resolves to a USD type the codec has no handler for.
    UnsupportedType,
    /// The lanes do not decode to the effective type: wrong/populated lane,
    /// wrong component count, a value out of the target range, or the scalar/
    /// array flag disagreeing with the effective type.
    InvalidValue,
};

/**
 * @brief Stateless codec between idtxcore::AttributeValue and pxr::VtValue.
 *
 * Multi-component order is USD memory order: matrices are row-major (16
 * doubles, as Matrix4dTransform), quaternions are real then i, j, k. Scalars
 * consume exactly their component width from the matching lane; arrays consume
 * a whole number of elements (empty and length-one arrays are valid arrays).
 * All lanes other than the one the type uses must be empty.
 */
class ProtoVtCodec
{
    IDTX_LOG_CATEGORY("ProtoVtCodec")

public:
    /**
     * @brief Decode @p in into @p out as a value of @p effective_type.
     *
     * @param effective_type The already-resolved USD type, including role and
     *        array-ness (the caller derived array-ness from @p in.is_array()).
     * @param in  The wire lanes.
     * @param out Receives the decoded VtValue (scalar T or VtArray<T>) on Ok.
     * @return Ok, UnsupportedType (no handler for this type), or InvalidValue.
     */
    static DecodeStatus DecodeValue(const pxr::SdfValueTypeName& effective_type,
                                    const idtxcore::AttributeValue& in,
                                    pxr::VtValue& out);

    /**
     * @brief Encode @p in (a VtValue of @p effective_type) into @p out.
     *
     * @p out is cleared first, so a reused message cannot retain another lane.
     * Sets @p out.is_array from the effective type. The caller is responsible
     * for setting PrimAttribute.type_name from
     * @c effective_type.GetScalarType().GetAsToken().
     *
     * @return false when @p in does not hold the C++ type the effective type
     *         requires, or the type is unsupported. A handler accepted on
     *         decode always has a matching encoder.
     */
    static bool EncodeValue(const pxr::SdfValueTypeName& effective_type,
                            const pxr::VtValue& in,
                            idtxcore::AttributeValue& out);
};

} // namespace session
} // namespace idtx
