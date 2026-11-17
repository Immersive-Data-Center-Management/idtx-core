#include "AttributeDispatcher.h"

#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primDefinition.h>
#include <pxr/usd/usd/relationship.h>

#include "ProtoVtCodec.h"
#include "ReferencePathResolver.h"

namespace idtx
{
namespace session
{

using FR = AttributeDispatcher::FailureReason;

namespace
{

// --------------------------------------------------------------------------
// Asset-path anchoring (plan §2.5). Client sends paths relative to the shared
// scene root; the server authors a server-absolute path and broadcasts/reads
// back a portable root-relative path. URI / package / bare search-path forms
// are rejected in v1 (gated by plan §6.4).
// --------------------------------------------------------------------------

bool IsAssetType(const pxr::SdfValueTypeName& et)
{
    return et.GetScalarType().GetType() == pxr::TfType::Find<pxr::SdfAssetPath>();
}

bool RawAssetToAbsolute(const std::string& raw, const AttributeAssetContext& assets,
                        std::string& out, std::string& detail)
{
    if (raw.empty()) { out.clear(); return true; }
    if (raw.find("://") != std::string::npos)
    {
        detail = "URI assets are unsupported in v1";
        return false;
    }
    if (raw.find('[') != std::string::npos)
    {
        detail = "package-relative assets are unsupported in v1";
        return false;
    }
    std::filesystem::path p(raw);
    if (!p.is_absolute())
    {
        const bool anchored = raw.rfind("./", 0) == 0 || raw.rfind("../", 0) == 0
            || raw.rfind(".\\", 0) == 0 || raw.rfind("..\\", 0) == 0;
        if (!anchored)
        {
            detail = "relative asset must be anchored with ./ or ../ (search paths unsupported)";
            return false;
        }
        p = assets.root_layer_dir / p;
    }
    p = p.lexically_normal();
    if (!ReferencePathResolver::IsUnderRoot(p, assets.uploads_root))
    {
        detail = "asset escapes the uploads root";
        return false;
    }
    out = p.generic_string();
    return true;
}

bool AbsoluteAssetToBroadcast(const std::string& abs, const AttributeAssetContext& assets,
                              std::string& out, std::string& detail)
{
    if (abs.empty()) { out.clear(); return true; }
    const std::string rel =
        ReferencePathResolver::ToAnchoredRelative(std::filesystem::path(abs), assets.root_layer_dir);
    if (rel.empty())
    {
        detail = "cannot anchor asset relative to scene root";
        return false;
    }
    out = rel;
    return true;
}

// Apply @p fn to every SdfAssetPath element of @p in (scalar or array),
// producing @p out. @p fn maps one asset string to another or fails.
bool TransformAssets(const pxr::SdfValueTypeName& et, const pxr::VtValue& in,
                     const std::function<bool(const std::string&, std::string&, std::string&)>& fn,
                     pxr::VtValue& out, std::string& detail)
{
    if (!et.IsArray())
    {
        if (!in.IsHolding<pxr::SdfAssetPath>()) { detail = "internal: asset value mismatch"; return false; }
        std::string o;
        if (!fn(in.UncheckedGet<pxr::SdfAssetPath>().GetAssetPath(), o, detail)) return false;
        out = pxr::VtValue(pxr::SdfAssetPath(o));
        return true;
    }
    if (!in.IsHolding<pxr::VtArray<pxr::SdfAssetPath>>()) { detail = "internal: asset array mismatch"; return false; }
    const auto& arr = in.UncheckedGet<pxr::VtArray<pxr::SdfAssetPath>>();
    pxr::VtArray<pxr::SdfAssetPath> res(arr.size());
    for (size_t i = 0; i < arr.size(); ++i)
    {
        std::string o;
        if (!fn(arr[i].GetAssetPath(), o, detail)) return false;
        res[i] = pxr::SdfAssetPath(o);
    }
    out = pxr::VtValue(res);
    return true;
}

// --------------------------------------------------------------------------
// Validation
// --------------------------------------------------------------------------

// One validated attribute ready to author. Fully populated by ValidateAttribute
// before any mutation; the authoring transaction only reads it.
struct Plan
{
    std::size_t           index = 0;
    pxr::TfToken          name;
    pxr::SdfValueTypeName type;
    pxr::SdfVariability   var = pxr::SdfVariabilityVarying;
    bool                  custom = true;
    pxr::VtValue          value;         ///< Authored value (server-absolute assets).
    bool                  existed = false; ///< Attr spec existed on the edit layer before.
};

FR ValidatePrimLevel(const pxr::UsdStageRefPtr& stage, const std::string& path_str,
                     pxr::UsdPrim& out)
{
    if (!stage) return FR::PrimNotFound;
    if (path_str.empty()) return FR::PrimNotFound;
    if (!pxr::SdfPath::IsValidPathString(path_str)) return FR::PrimNotFound;

    const pxr::SdfPath path(path_str);
    if (!path.IsAbsolutePath() || !path.IsPrimPath() || path.ContainsPrimVariantSelection())
        return FR::PrimNotFound;

    const pxr::UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim || !prim.IsDefined()) return FR::PrimNotFound;
    if (!prim.IsActive()) return FR::PrimNotEditable;
    if (prim.IsInstanceProxy() || prim.IsInPrototype()) return FR::PrimNotEditable;

    out = prim;
    return FR::None;
}

// Reads the composed stage only; authors nothing. On FR::None, @p plan and
// @p out_wire are fully populated (incl. the pre-encoded broadcast value).
FR ValidateAttribute(const pxr::UsdPrim& prim, const idtxcore::PrimAttribute& a,
                     const int name_count, const AttributeAssetContext& assets,
                     Plan& plan, idtxcore::PrimAttribute& out_wire, std::string& detail)
{
    const std::string& name = a.name();

    // 1. Name: valid namespaced identifier AND unique in the request.
    if (!pxr::SdfPath::IsValidNamespacedIdentifier(name) || name_count != 1)
        return FR::InvalidName;

    // 2. Reserved: transforms flow through TransformUpdate.
    if (name == "xformOpOrder" || name.rfind("xformOp:", 0) == 0)
        return FR::ReservedAttribute;

    const pxr::TfToken name_tok(name);

    // 3. Value presence + effective type.
    if (!a.has_value()) return FR::InvalidValue;
    const pxr::SdfValueTypeName scalar =
        pxr::SdfSchema::GetInstance().FindType(pxr::TfToken(a.type_name()));
    if (!scalar) return FR::UnknownType;
    if (scalar.IsArray())
    {
        detail = "type_name must be a scalar token; set is_array instead of a '[]' spelling";
        return FR::InvalidValue;
    }
    const pxr::SdfValueTypeName effective =
        a.value().is_array() ? scalar.GetArrayType() : scalar;
    if (!effective) return FR::UnsupportedType; // scalar has no array form

    // 4. Property kind + existing type.
    if (prim.GetRelationship(name_tok)) return FR::PropertyKindMismatch;
    if (const pxr::UsdProperty prop = prim.GetProperty(name_tok))
    {
        if (prop.Is<pxr::UsdRelationship>()) return FR::PropertyKindMismatch;
    }
    const pxr::UsdAttribute existing = prim.GetAttribute(name_tok);
    if (existing && existing.GetTypeName() != effective) return FR::TypeMismatch;

    // 5. Decode + asset normalization + pre-encode canonical broadcast value.
    pxr::VtValue decoded;
    switch (ProtoVtCodec::DecodeValue(effective, a.value(), decoded))
    {
        case DecodeStatus::Ok: break;
        case DecodeStatus::UnsupportedType: return FR::UnsupportedType;
        case DecodeStatus::InvalidValue:    return FR::InvalidValue;
    }

    pxr::VtValue author_value = decoded;    // server-absolute assets
    pxr::VtValue broadcast_value = decoded; // root-relative assets
    if (IsAssetType(effective))
    {
        if (!TransformAssets(effective, decoded,
                [&](const std::string& raw, std::string& o, std::string& d)
                { return RawAssetToAbsolute(raw, assets, o, d); },
                author_value, detail))
            return FR::InvalidValue;
        if (!TransformAssets(effective, decoded,
                [&](const std::string& raw, std::string& o, std::string& d)
                {
                    std::string abs;
                    if (!RawAssetToAbsolute(raw, assets, abs, d)) return false;
                    return AbsoluteAssetToBroadcast(abs, assets, o, d);
                },
                broadcast_value, detail))
            return FR::InvalidValue;
    }

    out_wire.set_name(name);
    out_wire.set_type_name(effective.GetScalarType().GetAsToken().GetString());
    if (!ProtoVtCodec::EncodeValue(effective, broadcast_value, *out_wire.mutable_value()))
    {
        detail = "accepted decoder has no matching encoder";
        return FR::NotAuthorable;
    }

    // 6. Variability / custom.
    pxr::SdfVariability var = pxr::SdfVariabilityVarying;
    bool custom = true;
    if (existing)
    {
        var = existing.GetVariability();
        custom = existing.IsCustom();
    }
    else if (const pxr::UsdPrimDefinition::Attribute adef =
                 prim.GetPrimDefinition().GetAttributeDefinition(name_tok))
    {
        var = adef.GetVariability();
        custom = false;
    }

    plan.name = name_tok;
    plan.type = effective;
    plan.var = var;
    plan.custom = custom;
    plan.value = std::move(author_value);
    return FR::None;
}

// --------------------------------------------------------------------------
// Authoring transaction (plan §2.3). A change block is not a rollback; we keep
// an explicit journal and restore each failed entry, preserving committed
// siblings and removing newly created inert specs if nothing survives.
// --------------------------------------------------------------------------

struct Journal
{
    pxr::SdfLayerHandle    layer;
    pxr::SdfLayerRefPtr    backup;
    pxr::SdfPath           prim_spec_path;
    std::set<pxr::SdfPath> preexisting_prim_specs; // ancestor+self prim specs present before.
};

void RestoreEntry(const Plan& p, Journal& j)
{
    const pxr::SdfPath prop = j.prim_spec_path.AppendProperty(p.name);
    if (p.existed)
    {
        // Restore the exact prior spec (default/metadata/connections/samples).
        pxr::SdfCopySpec(j.backup, prop, j.layer, prop);
    }
    else if (const pxr::SdfPrimSpecHandle ps = j.layer->GetPrimAtPath(j.prim_spec_path))
    {
        if (const pxr::SdfAttributeSpecHandle attr = j.layer->GetAttributeAtPath(prop))
            ps->RemoveProperty(attr);
    }
}

// Author one plan entry, restoring it and returning false on any failure.
bool SetDefaultRestoringOnFailure(const pxr::SdfPrimSpecHandle& ps, const Plan& p, Journal& j)
{
    try
    {
        const pxr::SdfPath prop = ps->GetPath().AppendProperty(p.name);
        pxr::SdfAttributeSpecHandle attr = j.layer->GetAttributeAtPath(prop);
        if (!attr)
        {
            attr = pxr::SdfAttributeSpec::New(ps, p.name.GetString(), p.type, p.var);
            if (!attr) return false;
            attr->SetCustom(p.custom);
        }
        if (!attr->SetDefaultValue(p.value))
        {
            RestoreEntry(p, j);
            return false;
        }
        return true;
    }
    catch (...)
    {
        RestoreEntry(p, j);
        return false;
    }
}

// Remove the created prim spec and any newly created, now-empty inert ancestor
// overs when nothing was authored.
void RemoveNewInertParents(Journal& j)
{
    for (pxr::SdfPath path = j.prim_spec_path;
         !path.IsEmpty() && path != pxr::SdfPath::AbsoluteRootPath();
         path = path.GetParentPath())
    {
        if (j.preexisting_prim_specs.count(path)) break; // reached a spec we must keep
        const pxr::SdfPrimSpecHandle spec = j.layer->GetPrimAtPath(path);
        if (!spec) continue;
        // Only drop a spec we created that is now inert (no properties/children).
        if (!spec->GetProperties().empty() || !spec->GetNameChildren().empty()) break;
        const pxr::SdfPath parent_path = path.GetParentPath();
        const pxr::SdfPrimSpecHandle parent =
            parent_path == pxr::SdfPath::AbsoluteRootPath()
                ? j.layer->GetPseudoRoot()
                : j.layer->GetPrimAtPath(parent_path);
        if (!parent || !parent->RemoveNameChild(spec)) break;
    }
}

void FailAll(AttributeDispatcher::AuthoringResult& result, const FR reason)
{
    for (auto& r : result.attributes) { r.ok = false; r.reason = reason; }
    result.authored_any = false;
}

void FailPlanned(AttributeDispatcher::AuthoringResult& result,
                 const std::vector<Plan>& plans, const FR reason)
{
    for (const Plan& p : plans)
    {
        result.attributes[p.index].ok = false;
        result.attributes[p.index].reason = reason;
    }
}

} // namespace

const char* AttributeDispatcher::FailureReasonToString(const FailureReason r) noexcept
{
    switch (r)
    {
        case FailureReason::None:                 return "none";
        case FailureReason::PrimNotFound:         return "prim_not_found";
        case FailureReason::PrimNotEditable:      return "prim_not_editable";
        case FailureReason::InvalidName:          return "invalid_name";
        case FailureReason::ReservedAttribute:    return "reserved_attribute";
        case FailureReason::PropertyKindMismatch: return "property_kind_mismatch";
        case FailureReason::UnknownType:          return "unknown_type";
        case FailureReason::UnsupportedType:      return "unsupported_type";
        case FailureReason::TypeMismatch:         return "type_mismatch";
        case FailureReason::InvalidValue:         return "invalid_value";
        case FailureReason::NotAuthorable:        return "not_authorable";
    }
    return "unknown";
}

idtxcore::AttributeFailureReason AttributeDispatcher::ToProto(const FailureReason r) noexcept
{
    switch (r)
    {
        case FailureReason::None:                 return idtxcore::ATTRIBUTE_FAILURE_UNSPECIFIED;
        case FailureReason::PrimNotFound:         return idtxcore::ATTRIBUTE_FAILURE_PRIM_NOT_FOUND;
        case FailureReason::PrimNotEditable:      return idtxcore::ATTRIBUTE_FAILURE_PRIM_NOT_EDITABLE;
        case FailureReason::InvalidName:          return idtxcore::ATTRIBUTE_FAILURE_INVALID_NAME;
        case FailureReason::ReservedAttribute:    return idtxcore::ATTRIBUTE_FAILURE_RESERVED_ATTRIBUTE;
        case FailureReason::PropertyKindMismatch: return idtxcore::ATTRIBUTE_FAILURE_PROPERTY_KIND_MISMATCH;
        case FailureReason::UnknownType:          return idtxcore::ATTRIBUTE_FAILURE_UNKNOWN_TYPE;
        case FailureReason::UnsupportedType:      return idtxcore::ATTRIBUTE_FAILURE_UNSUPPORTED_TYPE;
        case FailureReason::TypeMismatch:         return idtxcore::ATTRIBUTE_FAILURE_TYPE_MISMATCH;
        case FailureReason::InvalidValue:         return idtxcore::ATTRIBUTE_FAILURE_INVALID_VALUE;
        case FailureReason::NotAuthorable:        return idtxcore::ATTRIBUTE_FAILURE_NOT_AUTHORABLE;
    }
    return idtxcore::ATTRIBUTE_FAILURE_UNSPECIFIED;
}

AttributeDispatcher::AuthoringResult AttributeDispatcher::Author(
    const pxr::UsdStageRefPtr& stage,
    const idtxcore::SetPrimAttributesRequest& req,
    const AttributeAssetContext& assets)
{
    AuthoringResult result;
    const int n = req.attributes_size();
    result.attributes.resize(n);
    for (int i = 0; i < n; ++i) result.attributes[i].name = req.attributes(i).name();

    // An empty request is an explicit no-op, decided before prim validation so
    // an invalid target cannot be implied as authored (plan §1.5).
    if (n == 0) return result;

    // Prim-level validation (once).
    pxr::UsdPrim prim;
    if (const FR prim_fail = ValidatePrimLevel(stage, req.prim_path(), prim); prim_fail != FR::None)
    {
        FailAll(result, prim_fail);
        return result;
    }

    // Count names up front so every occurrence of a duplicate fails.
    std::unordered_map<std::string, int> counts;
    for (int i = 0; i < n; ++i) ++counts[req.attributes(i).name()];

    // Per-attribute validation -> plans. Reads the composed stage only.
    std::vector<Plan> plans;
    plans.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        AttributeResult& r = result.attributes[i];
        const idtxcore::PrimAttribute& a = req.attributes(i);
        Plan p;
        p.index = static_cast<std::size_t>(i);
        std::string detail;
        const FR reason = ValidateAttribute(prim, a, counts[a.name()], assets, p, r.wire, detail);
        if (reason != FR::None)
        {
            r.ok = false;
            r.reason = reason;
            r.detail = std::move(detail);
            continue;
        }
        r.type = p.type;
        r.value = p.value;
        plans.push_back(std::move(p));
    }

    if (plans.empty()) return result; // all invalid: no prim/parent/spec creation.

    // --- authoring transaction ---
    const pxr::UsdEditTarget& tgt = stage->GetEditTarget();
    Journal journal;
    journal.layer = tgt.GetLayer();
    journal.prim_spec_path = tgt.MapToSpecPath(prim.GetPath());
    if (!journal.layer || journal.prim_spec_path.IsEmpty() || !journal.layer->PermissionToEdit())
    {
        FailPlanned(result, plans, FR::NotAuthorable);
        return result;
    }

    // Record pre-existing prim specs (so inert-parent cleanup keeps them) and
    // back up existing attribute specs we will overwrite.
    for (pxr::SdfPath path = journal.prim_spec_path;
         !path.IsEmpty() && path != pxr::SdfPath::AbsoluteRootPath();
         path = path.GetParentPath())
    {
        if (journal.layer->GetPrimAtPath(path)) journal.preexisting_prim_specs.insert(path);
    }
    journal.backup = pxr::SdfLayer::CreateAnonymous(".usda");
    bool any_backed_up = false;
    for (Plan& p : plans)
    {
        const pxr::SdfPath prop = journal.prim_spec_path.AppendProperty(p.name);
        if (journal.layer->GetAttributeAtPath(prop))
        {
            p.existed = true;
            if (!any_backed_up)
            {
                pxr::SdfCreatePrimInLayer(journal.backup, journal.prim_spec_path);
                any_backed_up = true;
            }
            pxr::SdfCopySpec(journal.layer, prop, journal.backup, prop);
        }
    }

    {
        pxr::SdfChangeBlock block;
        const pxr::SdfPrimSpecHandle ps =
            pxr::SdfCreatePrimInLayer(journal.layer, journal.prim_spec_path);
        if (!ps)
        {
            RemoveNewInertParents(journal);
            FailPlanned(result, plans, FR::NotAuthorable);
            return result;
        }
        for (const Plan& p : plans)
        {
            const bool ok = SetDefaultRestoringOnFailure(ps, p, journal);
            AttributeResult& r = result.attributes[p.index];
            r.ok = ok;
            r.reason = ok ? FR::None : FR::NotAuthorable;
            if (ok) result.authored_any = true;
        }
        if (!result.authored_any) RemoveNewInertParents(journal);
    }

    return result;
}

bool AttributeDispatcher::ReadAuthoredAttribute(const pxr::SdfAttributeSpecHandle& attr,
                                                const AttributeAssetContext& assets,
                                                idtxcore::PrimAttribute& out)
{
    if (!attr) return false;
    if (!attr->HasDefaultValue()) return false;

    const pxr::SdfValueTypeName type = attr->GetTypeName();
    if (!type) return false;
    pxr::VtValue value = attr->GetDefaultValue();
    if (value.IsEmpty()) return false;

    if (IsAssetType(type))
    {
        pxr::VtValue broadcast;
        std::string detail;
        if (!TransformAssets(type, value,
                [&](const std::string& abs, std::string& o, std::string& d)
                { return AbsoluteAssetToBroadcast(abs, assets, o, d); },
                broadcast, detail))
            return false;
        value = std::move(broadcast);
    }

    out.set_name(attr->GetName());
    out.set_type_name(type.GetScalarType().GetAsToken().GetString());
    return ProtoVtCodec::EncodeValue(type, value, *out.mutable_value());
}

} // namespace session
} // namespace idtx
