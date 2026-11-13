#include "ReferencePathResolver.h"

#include <system_error>

#include "utils/UsdFileLocator.h"

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
    const fs::path& root_layer_dir,
    const fs::path& sidecar_dir,
    const fs::path& root_layer_file,
    Resolved& out)
{
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
    fs::path target;
    if (asset_str.front() == '/')
    {
        // "Absolute" against the uploads root (the root download folder).
        target = uploads_root / asset_str.substr(1);
    }
    else if (asset_str.rfind("./", 0) == 0 || asset_str.rfind("../", 0) == 0)
    {
        // Relative to the open stage's root layer (the shared file).
        target = root_layer_dir / asset_str;
    }
    else
    {
        // Bare "folder/file.usd": USD would treat this as a search-path-anchored
        // asset path, which the server does not register. Reject.
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
    // climbs out of sessions/ with `..`).
    const fs::path rel_to_root = target_abs.lexically_relative(uploads_abs);
    if (rel_to_root.empty() || *rel_to_root.begin() == "..")
    {
        IDTX_LOG(IDTX_WARN, "reference_path '{}' escapes the uploads root; rejecting.", asset_str);
        return Status::OutsideRoot;
    }

    std::error_code ec;
    if (!fs::is_regular_file(target_abs, ec))
    {
        IDTX_LOG(IDTX_WARN, "reference_path '{}' does not resolve to an existing file ('{}').",
                 asset_str, target_abs.string());
        return Status::NotFound;
    }
    if (!utils::UsdFileLocator::IsUsdFilename(target_abs.filename().string()))
        return Status::NotAUsdFile;

    const fs::path root_file_abs = Normalise(root_layer_file);
    if (target_abs == root_file_abs)
    {
        IDTX_LOG(IDTX_WARN, "reference_path '{}' targets the session's own root layer; rejecting.",
                 asset_str);
        return Status::SelfReference;
    }

    // ---- Produce the authored (sidecar) and broadcast (root) forms ---------
    out.authored_asset = ToAnchoredRelative(target_abs, sidecar_dir);
    out.prim_path      = prim_path;
    out.broadcast_path = JoinAssetAndPrim(ToAnchoredRelative(target_abs, root_layer_dir), prim_path);
    return Status::Ok;
}

std::string ReferencePathResolver::AuthoredToBroadcast(const std::string& authored_asset_path,
                                                       const fs::path& sidecar_dir,
                                                       const fs::path& root_layer_dir)
{
    // The authored path is relative to the sidecar layer; resolve it to the
    // on-disk target, then re-express it relative to the root layer.
    const fs::path target_abs = Normalise(Normalise(sidecar_dir) / authored_asset_path);
    return ToAnchoredRelative(target_abs, root_layer_dir);
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
        case Status::NotFound:           return "NotFound";
        case Status::NotAUsdFile:        return "NotAUsdFile";
        case Status::SelfReference:      return "SelfReference";
    }
    return "Unknown";
}

} // namespace session
} // namespace idtx
