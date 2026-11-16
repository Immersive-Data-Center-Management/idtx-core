#include "ReferencePathResolver.h"

#include <system_error>

#include <pxr/usd/sdf/primSpec.h>

namespace idtx
{
namespace session
{

namespace fs = std::filesystem;

namespace
{

std::string Trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

fs::path Normalise(const fs::path& p)
{
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

} // namespace

std::string ReferencePathResolver::ToAnchoredRelative(const fs::path& target, const fs::path& base)
{
    const fs::path rel = Normalise(target).lexically_relative(Normalise(base));
    if (rel.empty()) return {};
    std::string s = rel.generic_string();
    // A relative path that does not climb out (`..`) must be anchored with `./`
    // so USD does not read it as a search-path-anchored asset path.
    if (s.rfind("..", 0) != 0) s = "./" + s;
    return s;
}

std::string ReferencePathResolver::JoinAssetAndPrim(const std::string& asset_path,
                                                    const pxr::SdfPath& prim_path)
{
    if (prim_path.IsEmpty()) return asset_path;
    return asset_path + "<" + prim_path.GetString() + ">";
}

ReferencePathResolver::Status ReferencePathResolver::Resolve(
    const std::string& reference_path,
    const fs::path& uploads_root,
    const pxr::SdfLayerHandle& root_layer,
    Resolved& out)
{
    if (!root_layer)
    {
        IDTX_LOG(IDTX_ERROR, "ReferencePathResolver::Resolve called with a null root layer.");
        return Status::LayerUnreadable;
    }

    const std::string trimmed = Trim(reference_path);

    // ---- Split off an optional <prim> suffix -------------------------------
    std::string asset_str = trimmed;
    pxr::SdfPath prim_path;
    if (!trimmed.empty() && trimmed.back() == '>')
    {
        const auto lt = trimmed.rfind('<');
        if (lt == std::string::npos)
        {
            IDTX_LOG(IDTX_WARN, "reference_path '{}' has a trailing '>' without '<'.", reference_path);
            return Status::InvalidPrimPath;
        }
        const std::string prim_str = Trim(trimmed.substr(lt + 1, trimmed.size() - lt - 2));
        asset_str = Trim(trimmed.substr(0, lt));
        if (!pxr::SdfPath::IsValidPathString(prim_str))
        {
            IDTX_LOG(IDTX_WARN, "reference_path prim target '{}' is not a valid path.", prim_str);
            return Status::InvalidPrimPath;
        }
        prim_path = pxr::SdfPath(prim_str);
        if (!prim_path.IsAbsolutePath() || !prim_path.IsPrimPath())
        {
            IDTX_LOG(IDTX_WARN, "reference_path prim target '{}' is not an absolute prim path.", prim_str);
            return Status::InvalidPrimPath;
        }
    }

    if (asset_str.empty()) return Status::EmptyPath;

    // ---- Classify the asset path and locate the on-disk target -------------
    // Server path policy (deliberately NOT USD semantics): a leading '/' is
    // anchored at the uploads root, './' and '../' at the open stage's root
    // layer, and a bare "folder/file.usd" (a USD search-path) is rejected. This
    // mapping, together with the escape guard below, is security-relevant and is
    // kept out of USD, which would treat '/abs' as a real filesystem path and
    // happily resolve a path outside the uploads tree.
    const fs::path root_dir = fs::path(root_layer->GetRealPath()).parent_path();

    fs::path target;
    if (asset_str.front() == '/')
    {
        target = uploads_root / asset_str.substr(1);
    }
    else if (asset_str.rfind("./", 0) == 0 || asset_str.rfind("../", 0) == 0)
    {
        target = root_dir / asset_str;
    }
    else
    {
        IDTX_LOG(IDTX_WARN,
                 "reference_path '{}' is a search-path-anchored asset path; rejecting "
                 "(use '/abs', './rel' or '../rel').",
                 asset_str);
        return Status::SearchPathRejected;
    }

    const fs::path target_abs  = Normalise(target);
    const fs::path uploads_abs = Normalise(uploads_root);

    // ---- Escape guard: the resolved target must stay under the uploads root.
    // Checked on the resolved target (not the authored string, which legitimately
    // climbs out of sessions/ with `..`). Runs BEFORE the target is opened, so
    // a path outside the uploads tree is never touched.
    const fs::path rel_to_root = target_abs.lexically_relative(uploads_abs);
    if (rel_to_root.empty() || *rel_to_root.begin() == "..")
    {
        IDTX_LOG(IDTX_WARN, "reference_path '{}' escapes the uploads root; rejecting.", asset_str);
        return Status::OutsideRoot;
    }

    // ---- Open the target as a USD layer ------------------------------------
    // FindOrOpen both proves the target is an accessible, parseable USD layer
    // (replacing the old existence + filename-extension checks) and hands us the
    // layer needed to validate the reference target. The path is already
    // resolved and escape-guarded, so opening it is safe.
    const pxr::SdfLayerRefPtr layer = pxr::SdfLayer::FindOrOpen(target_abs.generic_string());
    if (!layer)
    {
        IDTX_LOG(IDTX_WARN, "reference_path '{}' is not an accessible USD layer ('{}').",
                 asset_str, target_abs.string());
        return Status::LayerUnreadable;
    }

    // Self-reference guard via the layer's canonical identifier: FindOrOpen
    // returns the already-open root layer when the target resolves to it, so an
    // identifier match is an exact self-reference (no path-string normalisation).
    if (layer->GetIdentifier() == root_layer->GetIdentifier())
    {
        IDTX_LOG(IDTX_WARN, "reference_path '{}' targets the session's own root layer; rejecting.",
                 asset_str);
        return Status::SelfReference;
    }

    // ---- Verify the effective target prim exists ---------------------------
    // An empty reference prim path means "use the layer's defaultPrim", so when
    // no <prim> suffix was given we resolve and require that defaultPrim.
    pxr::SdfPath target_prim = prim_path;
    if (target_prim.IsEmpty())
    {
        target_prim = layer->GetDefaultPrimAsPath();
        if (target_prim.IsEmpty())
        {
            IDTX_LOG(IDTX_WARN,
                     "reference_path '{}' omits a <prim> and layer '{}' has no usable defaultPrim.",
                     asset_str, target_abs.string());
            return Status::NoDefaultPrim;
        }
    }

    // NOTE: GetPrimAtPath is an Sdf-level (single-layer) lookup — it sees prim
    // specs authored directly in this layer, not prims introduced by the
    // layer's own sublayers or composition arcs. This is the intended
    // lightweight check; a false negative is possible only for a target whose
    // prim exists solely through the referenced layer's own composition.
    if (!layer->GetPrimAtPath(target_prim))
    {
        IDTX_LOG(IDTX_WARN, "reference_path '{}' target prim '{}' does not exist in layer '{}'.",
                 asset_str, target_prim.GetString(), target_abs.string());
        return Status::PrimNotFound;
    }

    // ---- Produce the authored (sidecar) and broadcast (root) forms ---------
    // out.prim_path keeps the AUTHORED prim path (empty == defaultPrim); it must
    // not be overwritten with the resolved defaultPrim, so the authored
    // SdfReference/SdfPayload keeps meaning "use the layer's defaultPrim".
    //
    // The authored asset is the absolute on-disk target. The sidecar is
    // server-only and ephemeral, so an absolute path never leaves the server:
    // USD composes it directly during the live session, the commit re-anchors it
    // relative to the written file (StageCommitter), and the broadcast form below
    // is already root-relative. Absolute also sidesteps the cross-drive case
    // (Windows uploads and sessions on different drives), where a sidecar-relative
    // path does not exist and would otherwise silently become an empty asset path
    // (a USD internal reference) instead of a valid external arc.
    out.authored_asset = target_abs.generic_string();
    out.prim_path      = prim_path;
    out.broadcast_path = JoinAssetAndPrim(ToAnchoredRelative(target_abs, root_dir), prim_path);
    return Status::Ok;
}

std::string ReferencePathResolver::AuthoredToBroadcast(const std::string& authored_asset_path,
                                                       const fs::path& root_layer_dir)
{
    // The authored path is the absolute on-disk target (see Resolve); re-express
    // it relative to the root layer for broadcast. ToAnchoredRelative normalises
    // the target itself, so the absolute path is handed straight through.
    return ToAnchoredRelative(authored_asset_path, root_layer_dir);
}

const char* ReferencePathResolver::StatusToString(Status s) noexcept
{
    switch (s)
    {
        case Status::Ok:                 return "Ok";
        case Status::EmptyPath:          return "EmptyPath";
        case Status::SearchPathRejected: return "SearchPathRejected";
        case Status::InvalidPrimPath:    return "InvalidPrimPath";
        case Status::OutsideRoot:        return "OutsideRoot";
        case Status::LayerUnreadable:    return "LayerUnreadable";
        case Status::SelfReference:      return "SelfReference";
        case Status::NoDefaultPrim:      return "NoDefaultPrim";
        case Status::PrimNotFound:       return "PrimNotFound";
    }
    return "Unknown";
}

} // namespace session
} // namespace idtx
