#include "ProtoVtCodec.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/assetPath.h>

namespace idtx
{
namespace session
{

namespace
{

// Which repeated lane of AttributeValue a USD type uses.
enum class Lane
{
    Doubles,
    Ints,
    Uints,
    Bools,
    Strings,
};

// True when every lane except @p lane is empty. The used lane itself may be
// empty (an empty array authors no components), so this is a necessary but not
// sufficient check; component-count validation follows.
bool OnlyLane(const idtxcore::AttributeValue& in, const Lane lane)
{
    const bool d = in.doubles_size() != 0;
    const bool i = in.ints_size() != 0;
    const bool u = in.uints_size() != 0;
    const bool b = in.bools_size() != 0;
    const bool s = in.strings_size() != 0;
    switch (lane)
    {
        case Lane::Doubles: return !i && !u && !b && !s;
        case Lane::Ints:    return !d && !u && !b && !s;
        case Lane::Uints:   return !d && !i && !b && !s;
        case Lane::Bools:   return !d && !i && !u && !s;
        case Lane::Strings: return !d && !i && !u && !b;
    }
    return false;
}

// ---- Narrowing of a double component to the target scalar ------------------
// A finite input outside the target range is rejected (nullopt); NaN/+-inf are
// representable and pass through. See the numeric policy in the header.

std::optional<double> NarrowComponent(const double v, double*)
{
    return v;
}

std::optional<float> NarrowComponent(const double v, float*)
{
    if (std::isfinite(v)
        && (v > static_cast<double>(std::numeric_limits<float>::max())
            || v < static_cast<double>(std::numeric_limits<float>::lowest())))
    {
        return std::nullopt;
    }
    return static_cast<float>(v);
}

std::optional<pxr::GfHalf> NarrowComponent(const double v, pxr::GfHalf*)
{
    // IEEE half has max magnitude 65504.
    constexpr double kHalfMax = 65504.0;
    if (std::isfinite(v) && (v > kHalfMax || v < -kHalfMax)) return std::nullopt;
    return pxr::GfHalf(static_cast<float>(v));
}

// ---- Integer/unsigned range checks -----------------------------------------

std::optional<int> ToInt32(const std::int64_t v)
{
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(v);
}

std::optional<std::uint8_t> ToUint8(const std::uint64_t v)
{
    if (v > std::numeric_limits<std::uint8_t>::max()) return std::nullopt;
    return static_cast<std::uint8_t>(v);
}

std::optional<unsigned int> ToUint32(const std::uint64_t v)
{
    if (v > std::numeric_limits<unsigned int>::max()) return std::nullopt;
    return static_cast<unsigned int>(v);
}

// ---- Generic lane decoders --------------------------------------------------
// Each reads WIDTH scalar components per logical element from one lane and
// builds a T via @p make (returning nullopt on a range/overflow failure). For a
// scalar exactly one element is required; for an array any whole number of
// elements (including zero) is accepted.

template <int Width, typename T, typename Make>
DecodeStatus DecodeDoubles(const idtxcore::AttributeValue& in, const bool is_array,
                           Make make, pxr::VtValue& out)
{
    if (!OnlyLane(in, Lane::Doubles)) return DecodeStatus::InvalidValue;
    const int size = in.doubles_size();
    if (size % Width != 0) return DecodeStatus::InvalidValue;
    const int count = size / Width;

    double comps[Width];
    if (!is_array)
    {
        if (count != 1) return DecodeStatus::InvalidValue;
        for (int k = 0; k < Width; ++k) comps[k] = in.doubles(k);
        const std::optional<T> v = make(comps);
        if (!v) return DecodeStatus::InvalidValue;
        out = pxr::VtValue(*v);
        return DecodeStatus::Ok;
    }

    pxr::VtArray<T> arr(count);
    for (int e = 0; e < count; ++e)
    {
        for (int k = 0; k < Width; ++k) comps[k] = in.doubles(e * Width + k);
        const std::optional<T> v = make(comps);
        if (!v) return DecodeStatus::InvalidValue;
        arr[e] = *v;
    }
    out = pxr::VtValue(arr);
    return DecodeStatus::Ok;
}

template <int Width, typename T, typename Make>
DecodeStatus DecodeInts(const idtxcore::AttributeValue& in, const bool is_array,
                        Make make, pxr::VtValue& out)
{
    if (!OnlyLane(in, Lane::Ints)) return DecodeStatus::InvalidValue;
    const int size = in.ints_size();
    if (size % Width != 0) return DecodeStatus::InvalidValue;
    const int count = size / Width;

    std::int64_t comps[Width];
    if (!is_array)
    {
        if (count != 1) return DecodeStatus::InvalidValue;
        for (int k = 0; k < Width; ++k) comps[k] = in.ints(k);
        const std::optional<T> v = make(comps);
        if (!v) return DecodeStatus::InvalidValue;
        out = pxr::VtValue(*v);
        return DecodeStatus::Ok;
    }

    pxr::VtArray<T> arr(count);
    for (int e = 0; e < count; ++e)
    {
        for (int k = 0; k < Width; ++k) comps[k] = in.ints(e * Width + k);
        const std::optional<T> v = make(comps);
        if (!v) return DecodeStatus::InvalidValue;
        arr[e] = *v;
    }
    out = pxr::VtValue(arr);
    return DecodeStatus::Ok;
}

template <int Width, typename T, typename Make>
DecodeStatus DecodeUints(const idtxcore::AttributeValue& in, const bool is_array,
                         Make make, pxr::VtValue& out)
{
    if (!OnlyLane(in, Lane::Uints)) return DecodeStatus::InvalidValue;
    const int size = in.uints_size();
    if (size % Width != 0) return DecodeStatus::InvalidValue;
    const int count = size / Width;

    std::uint64_t comps[Width];
    if (!is_array)
    {
        if (count != 1) return DecodeStatus::InvalidValue;
        for (int k = 0; k < Width; ++k) comps[k] = in.uints(k);
        const std::optional<T> v = make(comps);
        if (!v) return DecodeStatus::InvalidValue;
        out = pxr::VtValue(*v);
        return DecodeStatus::Ok;
    }

    pxr::VtArray<T> arr(count);
    for (int e = 0; e < count; ++e)
    {
        for (int k = 0; k < Width; ++k) comps[k] = in.uints(e * Width + k);
        const std::optional<T> v = make(comps);
        if (!v) return DecodeStatus::InvalidValue;
        arr[e] = *v;
    }
    out = pxr::VtValue(arr);
    return DecodeStatus::Ok;
}

DecodeStatus DecodeBools(const idtxcore::AttributeValue& in, const bool is_array,
                         pxr::VtValue& out)
{
    if (!OnlyLane(in, Lane::Bools)) return DecodeStatus::InvalidValue;
    const int count = in.bools_size();
    if (!is_array)
    {
        if (count != 1) return DecodeStatus::InvalidValue;
        out = pxr::VtValue(in.bools(0));
        return DecodeStatus::Ok;
    }
    pxr::VtArray<bool> arr(count);
    for (int e = 0; e < count; ++e) arr[e] = in.bools(e);
    out = pxr::VtValue(arr);
    return DecodeStatus::Ok;
}

template <typename T, typename Make>
DecodeStatus DecodeStrings(const idtxcore::AttributeValue& in, const bool is_array,
                           Make make, pxr::VtValue& out)
{
    if (!OnlyLane(in, Lane::Strings)) return DecodeStatus::InvalidValue;
    const int count = in.strings_size();
    if (!is_array)
    {
        if (count != 1) return DecodeStatus::InvalidValue;
        out = pxr::VtValue(make(in.strings(0)));
        return DecodeStatus::Ok;
    }
    pxr::VtArray<T> arr(count);
    for (int e = 0; e < count; ++e) arr[e] = make(in.strings(e));
    out = pxr::VtValue(arr);
    return DecodeStatus::Ok;
}

// ---- Generic lane encoders --------------------------------------------------

template <typename T, typename Put>
bool EncodeElems(const pxr::VtValue& in, const bool is_array, Put put,
                   idtxcore::AttributeValue& out)
{
    if (!is_array)
    {
        if (!in.IsHolding<T>()) return false;
        put(in.UncheckedGet<T>(), out);
        return true;
    }
    if (!in.IsHolding<pxr::VtArray<T>>()) return false;
    for (const T& e : in.UncheckedGet<pxr::VtArray<T>>()) put(e, out);
    return true;
}

} // namespace

DecodeStatus ProtoVtCodec::DecodeValue(const pxr::SdfValueTypeName& effective_type,
                                       const idtxcore::AttributeValue& in,
                                       pxr::VtValue& out)
{
    const pxr::SdfValueTypeName scalar = effective_type.GetScalarType();
    if (!scalar) return DecodeStatus::UnsupportedType;
    const bool is_array = effective_type.IsArray();

    // The caller derives the effective type's array-ness from in.is_array();
    // enforce that invariant defensively.
    if (is_array != in.is_array()) return DecodeStatus::InvalidValue;

    const pxr::TfType t = scalar.GetType();

    // ---- bool ----
    if (t == pxr::TfType::Find<bool>())
        return DecodeBools(in, is_array, out);

    // ---- unsigned (uints lane) ----
    if (t == pxr::TfType::Find<std::uint8_t>())
        return DecodeUints<1, std::uint8_t>(in, is_array,
            [](const std::uint64_t* c) { return ToUint8(c[0]); }, out);
    if (t == pxr::TfType::Find<unsigned int>())
        return DecodeUints<1, unsigned int>(in, is_array,
            [](const std::uint64_t* c) { return ToUint32(c[0]); }, out);
    if (t == pxr::TfType::Find<std::uint64_t>())
        return DecodeUints<1, std::uint64_t>(in, is_array,
            [](const std::uint64_t* c) -> std::optional<std::uint64_t> { return c[0]; }, out);

    // ---- signed integer (ints lane) ----
    if (t == pxr::TfType::Find<int>())
        return DecodeInts<1, int>(in, is_array,
            [](const std::int64_t* c) { return ToInt32(c[0]); }, out);
    if (t == pxr::TfType::Find<std::int64_t>())
        return DecodeInts<1, std::int64_t>(in, is_array,
            [](const std::int64_t* c) -> std::optional<std::int64_t> { return c[0]; }, out);
    if (t == pxr::TfType::Find<pxr::GfVec2i>())
        return DecodeInts<2, pxr::GfVec2i>(in, is_array, [](const std::int64_t* c) -> std::optional<pxr::GfVec2i> {
            const auto a = ToInt32(c[0]); const auto b = ToInt32(c[1]);
            if (!a || !b) return std::nullopt; return pxr::GfVec2i(*a, *b); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec3i>())
        return DecodeInts<3, pxr::GfVec3i>(in, is_array, [](const std::int64_t* c) -> std::optional<pxr::GfVec3i> {
            const auto a = ToInt32(c[0]); const auto b = ToInt32(c[1]); const auto d = ToInt32(c[2]);
            if (!a || !b || !d) return std::nullopt; return pxr::GfVec3i(*a, *b, *d); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec4i>())
        return DecodeInts<4, pxr::GfVec4i>(in, is_array, [](const std::int64_t* c) -> std::optional<pxr::GfVec4i> {
            const auto a = ToInt32(c[0]); const auto b = ToInt32(c[1]);
            const auto d = ToInt32(c[2]); const auto e = ToInt32(c[3]);
            if (!a || !b || !d || !e) return std::nullopt; return pxr::GfVec4i(*a, *b, *d, *e); }, out);

    // ---- floating scalars (doubles lane) ----
    if (t == pxr::TfType::Find<double>())
        return DecodeDoubles<1, double>(in, is_array,
            [](const double* c) { return NarrowComponent(c[0], static_cast<double*>(nullptr)); }, out);
    if (t == pxr::TfType::Find<float>())
        return DecodeDoubles<1, float>(in, is_array,
            [](const double* c) { return NarrowComponent(c[0], static_cast<float*>(nullptr)); }, out);
    if (t == pxr::TfType::Find<pxr::GfHalf>())
        return DecodeDoubles<1, pxr::GfHalf>(in, is_array,
            [](const double* c) { return NarrowComponent(c[0], static_cast<pxr::GfHalf*>(nullptr)); }, out);

    // ---- double vectors ----
    if (t == pxr::TfType::Find<pxr::GfVec2d>())
        return DecodeDoubles<2, pxr::GfVec2d>(in, is_array,
            [](const double* c) -> std::optional<pxr::GfVec2d> { return pxr::GfVec2d(c[0], c[1]); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec3d>())
        return DecodeDoubles<3, pxr::GfVec3d>(in, is_array,
            [](const double* c) -> std::optional<pxr::GfVec3d> { return pxr::GfVec3d(c[0], c[1], c[2]); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec4d>())
        return DecodeDoubles<4, pxr::GfVec4d>(in, is_array,
            [](const double* c) -> std::optional<pxr::GfVec4d> { return pxr::GfVec4d(c[0], c[1], c[2], c[3]); }, out);

    // ---- float vectors (narrowed) ----
    if (t == pxr::TfType::Find<pxr::GfVec2f>())
        return DecodeDoubles<2, pxr::GfVec2f>(in, is_array, [](const double* c) -> std::optional<pxr::GfVec2f> {
            const auto a = NarrowComponent(c[0], static_cast<float*>(nullptr));
            const auto b = NarrowComponent(c[1], static_cast<float*>(nullptr));
            if (!a || !b) return std::nullopt; return pxr::GfVec2f(*a, *b); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec3f>())
        return DecodeDoubles<3, pxr::GfVec3f>(in, is_array, [](const double* c) -> std::optional<pxr::GfVec3f> {
            const auto a = NarrowComponent(c[0], static_cast<float*>(nullptr));
            const auto b = NarrowComponent(c[1], static_cast<float*>(nullptr));
            const auto d = NarrowComponent(c[2], static_cast<float*>(nullptr));
            if (!a || !b || !d) return std::nullopt; return pxr::GfVec3f(*a, *b, *d); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec4f>())
        return DecodeDoubles<4, pxr::GfVec4f>(in, is_array, [](const double* c) -> std::optional<pxr::GfVec4f> {
            const auto a = NarrowComponent(c[0], static_cast<float*>(nullptr));
            const auto b = NarrowComponent(c[1], static_cast<float*>(nullptr));
            const auto d = NarrowComponent(c[2], static_cast<float*>(nullptr));
            const auto e = NarrowComponent(c[3], static_cast<float*>(nullptr));
            if (!a || !b || !d || !e) return std::nullopt; return pxr::GfVec4f(*a, *b, *d, *e); }, out);

    // ---- half vectors (narrowed) ----
    if (t == pxr::TfType::Find<pxr::GfVec2h>())
        return DecodeDoubles<2, pxr::GfVec2h>(in, is_array, [](const double* c) -> std::optional<pxr::GfVec2h> {
            const auto a = NarrowComponent(c[0], static_cast<pxr::GfHalf*>(nullptr));
            const auto b = NarrowComponent(c[1], static_cast<pxr::GfHalf*>(nullptr));
            if (!a || !b) return std::nullopt; return pxr::GfVec2h(*a, *b); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec3h>())
        return DecodeDoubles<3, pxr::GfVec3h>(in, is_array, [](const double* c) -> std::optional<pxr::GfVec3h> {
            const auto a = NarrowComponent(c[0], static_cast<pxr::GfHalf*>(nullptr));
            const auto b = NarrowComponent(c[1], static_cast<pxr::GfHalf*>(nullptr));
            const auto d = NarrowComponent(c[2], static_cast<pxr::GfHalf*>(nullptr));
            if (!a || !b || !d) return std::nullopt; return pxr::GfVec3h(*a, *b, *d); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec4h>())
        return DecodeDoubles<4, pxr::GfVec4h>(in, is_array, [](const double* c) -> std::optional<pxr::GfVec4h> {
            const auto a = NarrowComponent(c[0], static_cast<pxr::GfHalf*>(nullptr));
            const auto b = NarrowComponent(c[1], static_cast<pxr::GfHalf*>(nullptr));
            const auto d = NarrowComponent(c[2], static_cast<pxr::GfHalf*>(nullptr));
            const auto e = NarrowComponent(c[3], static_cast<pxr::GfHalf*>(nullptr));
            if (!a || !b || !d || !e) return std::nullopt; return pxr::GfVec4h(*a, *b, *d, *e); }, out);

    // ---- matrix4d (row-major, 16 doubles) ----
    if (t == pxr::TfType::Find<pxr::GfMatrix4d>())
        return DecodeDoubles<16, pxr::GfMatrix4d>(in, is_array, [](const double* c) -> std::optional<pxr::GfMatrix4d> {
            return pxr::GfMatrix4d(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
                                   c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]); }, out);

    // ---- quaternions (real, i, j, k) ----
    if (t == pxr::TfType::Find<pxr::GfQuatd>())
        return DecodeDoubles<4, pxr::GfQuatd>(in, is_array, [](const double* c) -> std::optional<pxr::GfQuatd> {
            return pxr::GfQuatd(c[0], c[1], c[2], c[3]); }, out);
    if (t == pxr::TfType::Find<pxr::GfQuatf>())
        return DecodeDoubles<4, pxr::GfQuatf>(in, is_array, [](const double* c) -> std::optional<pxr::GfQuatf> {
            const auto r = NarrowComponent(c[0], static_cast<float*>(nullptr));
            const auto i = NarrowComponent(c[1], static_cast<float*>(nullptr));
            const auto j = NarrowComponent(c[2], static_cast<float*>(nullptr));
            const auto k = NarrowComponent(c[3], static_cast<float*>(nullptr));
            if (!r || !i || !j || !k) return std::nullopt; return pxr::GfQuatf(*r, *i, *j, *k); }, out);

    // ---- strings lane: string / token / asset ----
    if (t == pxr::TfType::Find<std::string>())
        return DecodeStrings<std::string>(in, is_array,
            [](const std::string& s) { return s; }, out);
    if (t == pxr::TfType::Find<pxr::TfToken>())
        return DecodeStrings<pxr::TfToken>(in, is_array,
            [](const std::string& s) { return pxr::TfToken(s); }, out);
    if (t == pxr::TfType::Find<pxr::SdfAssetPath>())
        return DecodeStrings<pxr::SdfAssetPath>(in, is_array,
            [](const std::string& s) { return pxr::SdfAssetPath(s); }, out);

    return DecodeStatus::UnsupportedType;
}

bool ProtoVtCodec::EncodeValue(const pxr::SdfValueTypeName& effective_type,
                               const pxr::VtValue& in,
                               idtxcore::AttributeValue& out)
{
    out.Clear();
    const pxr::SdfValueTypeName scalar = effective_type.GetScalarType();
    if (!scalar) return false;
    const bool is_array = effective_type.IsArray();
    out.set_is_array(is_array);

    const pxr::TfType t = scalar.GetType();

    // ---- bool ----
    if (t == pxr::TfType::Find<bool>())
    {
        if (!is_array)
        {
            if (!in.IsHolding<bool>()) return false;
            out.add_bools(in.UncheckedGet<bool>());
            return true;
        }
        if (!in.IsHolding<pxr::VtArray<bool>>()) return false;
        for (const bool e : in.UncheckedGet<pxr::VtArray<bool>>()) out.add_bools(e);
        return true;
    }

    // ---- unsigned ----
    if (t == pxr::TfType::Find<std::uint8_t>())
        return EncodeElems<std::uint8_t>(in, is_array,
            [](const std::uint8_t& v, idtxcore::AttributeValue& o) { o.add_uints(v); }, out);
    if (t == pxr::TfType::Find<unsigned int>())
        return EncodeElems<unsigned int>(in, is_array,
            [](const unsigned int& v, idtxcore::AttributeValue& o) { o.add_uints(v); }, out);
    if (t == pxr::TfType::Find<std::uint64_t>())
        return EncodeElems<std::uint64_t>(in, is_array,
            [](const std::uint64_t& v, idtxcore::AttributeValue& o) { o.add_uints(v); }, out);

    // ---- signed integer ----
    if (t == pxr::TfType::Find<int>())
        return EncodeElems<int>(in, is_array,
            [](const int& v, idtxcore::AttributeValue& o) { o.add_ints(v); }, out);
    if (t == pxr::TfType::Find<std::int64_t>())
        return EncodeElems<std::int64_t>(in, is_array,
            [](const std::int64_t& v, idtxcore::AttributeValue& o) { o.add_ints(v); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec2i>())
        return EncodeElems<pxr::GfVec2i>(in, is_array,
            [](const pxr::GfVec2i& v, idtxcore::AttributeValue& o) { o.add_ints(v[0]); o.add_ints(v[1]); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec3i>())
        return EncodeElems<pxr::GfVec3i>(in, is_array,
            [](const pxr::GfVec3i& v, idtxcore::AttributeValue& o) { o.add_ints(v[0]); o.add_ints(v[1]); o.add_ints(v[2]); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec4i>())
        return EncodeElems<pxr::GfVec4i>(in, is_array,
            [](const pxr::GfVec4i& v, idtxcore::AttributeValue& o) { o.add_ints(v[0]); o.add_ints(v[1]); o.add_ints(v[2]); o.add_ints(v[3]); }, out);

    // ---- floating scalars ----
    if (t == pxr::TfType::Find<double>())
        return EncodeElems<double>(in, is_array,
            [](const double& v, idtxcore::AttributeValue& o) { o.add_doubles(v); }, out);
    if (t == pxr::TfType::Find<float>())
        return EncodeElems<float>(in, is_array,
            [](const float& v, idtxcore::AttributeValue& o) { o.add_doubles(static_cast<double>(v)); }, out);
    if (t == pxr::TfType::Find<pxr::GfHalf>())
        return EncodeElems<pxr::GfHalf>(in, is_array,
            [](const pxr::GfHalf& v, idtxcore::AttributeValue& o) { o.add_doubles(static_cast<double>(static_cast<float>(v))); }, out);

    // ---- double vectors ----
    if (t == pxr::TfType::Find<pxr::GfVec2d>())
        return EncodeElems<pxr::GfVec2d>(in, is_array,
            [](const pxr::GfVec2d& v, idtxcore::AttributeValue& o) { o.add_doubles(v[0]); o.add_doubles(v[1]); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec3d>())
        return EncodeElems<pxr::GfVec3d>(in, is_array,
            [](const pxr::GfVec3d& v, idtxcore::AttributeValue& o) { o.add_doubles(v[0]); o.add_doubles(v[1]); o.add_doubles(v[2]); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec4d>())
        return EncodeElems<pxr::GfVec4d>(in, is_array,
            [](const pxr::GfVec4d& v, idtxcore::AttributeValue& o) { o.add_doubles(v[0]); o.add_doubles(v[1]); o.add_doubles(v[2]); o.add_doubles(v[3]); }, out);

    // ---- float vectors ----
    if (t == pxr::TfType::Find<pxr::GfVec2f>())
        return EncodeElems<pxr::GfVec2f>(in, is_array,
            [](const pxr::GfVec2f& v, idtxcore::AttributeValue& o) { o.add_doubles(v[0]); o.add_doubles(v[1]); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec3f>())
        return EncodeElems<pxr::GfVec3f>(in, is_array,
            [](const pxr::GfVec3f& v, idtxcore::AttributeValue& o) { o.add_doubles(v[0]); o.add_doubles(v[1]); o.add_doubles(v[2]); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec4f>())
        return EncodeElems<pxr::GfVec4f>(in, is_array,
            [](const pxr::GfVec4f& v, idtxcore::AttributeValue& o) { o.add_doubles(v[0]); o.add_doubles(v[1]); o.add_doubles(v[2]); o.add_doubles(v[3]); }, out);

    // ---- half vectors ----
    if (t == pxr::TfType::Find<pxr::GfVec2h>())
        return EncodeElems<pxr::GfVec2h>(in, is_array,
            [](const pxr::GfVec2h& v, idtxcore::AttributeValue& o) { o.add_doubles(static_cast<float>(v[0])); o.add_doubles(static_cast<float>(v[1])); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec3h>())
        return EncodeElems<pxr::GfVec3h>(in, is_array,
            [](const pxr::GfVec3h& v, idtxcore::AttributeValue& o) { o.add_doubles(static_cast<float>(v[0])); o.add_doubles(static_cast<float>(v[1])); o.add_doubles(static_cast<float>(v[2])); }, out);
    if (t == pxr::TfType::Find<pxr::GfVec4h>())
        return EncodeElems<pxr::GfVec4h>(in, is_array,
            [](const pxr::GfVec4h& v, idtxcore::AttributeValue& o) { o.add_doubles(static_cast<float>(v[0])); o.add_doubles(static_cast<float>(v[1])); o.add_doubles(static_cast<float>(v[2])); o.add_doubles(static_cast<float>(v[3])); }, out);

    // ---- matrix4d (row-major) ----
    if (t == pxr::TfType::Find<pxr::GfMatrix4d>())
        return EncodeElems<pxr::GfMatrix4d>(in, is_array,
            [](const pxr::GfMatrix4d& m, idtxcore::AttributeValue& o) {
                for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) o.add_doubles(m[i][j]); }, out);

    // ---- quaternions (real, i, j, k) ----
    if (t == pxr::TfType::Find<pxr::GfQuatd>())
        return EncodeElems<pxr::GfQuatd>(in, is_array,
            [](const pxr::GfQuatd& q, idtxcore::AttributeValue& o) {
                o.add_doubles(q.GetReal());
                const pxr::GfVec3d im = q.GetImaginary();
                o.add_doubles(im[0]); o.add_doubles(im[1]); o.add_doubles(im[2]); }, out);
    if (t == pxr::TfType::Find<pxr::GfQuatf>())
        return EncodeElems<pxr::GfQuatf>(in, is_array,
            [](const pxr::GfQuatf& q, idtxcore::AttributeValue& o) {
                o.add_doubles(static_cast<double>(q.GetReal()));
                const pxr::GfVec3f im = q.GetImaginary();
                o.add_doubles(im[0]); o.add_doubles(im[1]); o.add_doubles(im[2]); }, out);

    // ---- strings lane ----
    if (t == pxr::TfType::Find<std::string>())
        return EncodeElems<std::string>(in, is_array,
            [](const std::string& s, idtxcore::AttributeValue& o) { o.add_strings(s); }, out);
    if (t == pxr::TfType::Find<pxr::TfToken>())
        return EncodeElems<pxr::TfToken>(in, is_array,
            [](const pxr::TfToken& s, idtxcore::AttributeValue& o) { o.add_strings(s.GetString()); }, out);
    if (t == pxr::TfType::Find<pxr::SdfAssetPath>())
        return EncodeElems<pxr::SdfAssetPath>(in, is_array,
            [](const pxr::SdfAssetPath& a, idtxcore::AttributeValue& o) { o.add_strings(a.GetAssetPath()); }, out);

    return false;
}

} // namespace session
} // namespace idtx
