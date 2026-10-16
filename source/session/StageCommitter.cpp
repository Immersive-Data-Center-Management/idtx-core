/**
 * @file StageCommitter.cpp
 * @brief Implementation of the session-layer -> original-file commit.
 */
#include "StageCommitter.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/zipFile.h>
#include <pxr/usd/usdUtils/flattenLayerStack.h>

namespace fs = std::filesystem;

namespace idtx
{
namespace session
{

namespace
{

/// RAII guard that recursively removes a directory on destruction.
struct TempDirGuard
{
    fs::path path;
    explicit TempDirGuard(fs::path p) : path(std::move(p)) {}
    ~TempDirGuard()
    {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool HasExtension(const std::string& path, const char* ext)
{
    std::string lower;
    lower.reserve(path.size());
    for (char c : path) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower.ends_with(ext);
}

// Fold the session's soft deletes into @p merged: a prim the session marked
// `active=false` is a delete request, so the prim (and its subtree) is removed
// from the flattened layer before it is written back to the original file.
//
// The delete set is derived from the session layer itself (every prim spec with
// an authored active=false), not from in-memory state, so it is correct after a
// server restart that resumed the sidecar. We deliberately key off the session
// layer rather than "every inactive prim in @p merged": a prim the *source*
// file legitimately shipped inactive must survive the commit untouched.
//
// Covers both a root-authored prim (its session over carries active=false) and
// a prim created-then-deleted this session (its session def carries
// active=false); in both cases the flattened result holds the prim and we drop
// it here. SetActive() already rejected prims defined across references/payloads
// or in sublayers, so a path reaching here is always local to the flattened
// root and safe to remove.
void RemoveSoftDeletedPrims(const pxr::UsdStageRefPtr& stage,
                            const pxr::SdfLayerRefPtr& merged)
{
    if (!stage || !merged) return;
    const pxr::SdfLayerHandle session_layer = stage->GetSessionLayer();
    if (!session_layer) return;

    std::vector<pxr::SdfPath> deleted;
    session_layer->Traverse(
        pxr::SdfPath::AbsoluteRootPath(),
        [&](const pxr::SdfPath& p)
        {
            if (!p.IsPrimPath()) return;
            const pxr::SdfPrimSpecHandle spec = session_layer->GetPrimAtPath(p);
            if (spec && spec->HasActive() && !spec->GetActive()) deleted.push_back(p);
        });

    for (const pxr::SdfPath& path : deleted)
    {
        const pxr::SdfPrimSpecHandle spec = merged->GetPrimAtPath(path);
        if (!spec) continue; // already removed as part of a deleted ancestor
        const pxr::SdfPath parent_path = path.GetParentPath();
        const pxr::SdfPrimSpecHandle parent =
            parent_path == pxr::SdfPath::AbsoluteRootPath()
                ? merged->GetPseudoRoot()
                : merged->GetPrimAtPath(parent_path);
        if (parent) parent->RemoveNameChild(spec);
    }
}

} // namespace

bool StageCommitter::Commit(const pxr::UsdStageRefPtr& stage,
                            const std::string& original_path,
                            std::string& out_error)
{
    if (!stage)
    {
        out_error = "null stage";
        return false;
    }
    if (HasExtension(original_path, ".usdz"))
        return CommitUsdz(stage, original_path, out_error);
    return CommitFlat(stage, original_path, out_error);
}

bool StageCommitter::CommitFlat(const pxr::UsdStageRefPtr& stage,
                                const std::string& original_path,
                                std::string& out_error)
{
    // Merge the composed layer stack (sidecar session layer over the root
    // layer stack) into a single layer, preserving all composition arcs.
    pxr::SdfLayerRefPtr merged = pxr::UsdUtilsFlattenLayerStack(stage);
    if (!merged)
    {
        out_error = "UsdUtilsFlattenLayerStack returned null";
        return false;
    }

    // Fold the session's soft deletes in: a prim marked active=false is removed
    // from the flattened layer so it does not survive into the committed file.
    RemoveSoftDeletedPrims(stage, merged);

    // Write to a temp sibling first, then atomically rename over the original
    // so a failed/partial export can never truncate the source file. Preserve
    // the original extension so the on-disk encoding (.usda vs .usdc) is kept.
    const fs::path dst(original_path);
    const fs::path tmp = dst.parent_path() /
        ("." + dst.stem().string() + "_idtx_commit" + dst.extension().string());

    std::error_code ec;
    fs::remove(tmp, ec); // best-effort pre-clean

    if (!merged->Export(tmp.string()))
    {
        out_error = "SdfLayer::Export failed for '" + tmp.string() + "'";
        std::error_code rmec;
        fs::remove(tmp, rmec);
        return false;
    }

    fs::rename(tmp, dst, ec);
    if (ec)
    {
        out_error = "atomic rename '" + tmp.string() + "' -> '" + dst.string()
                  + "' failed: " + ec.message();
        std::error_code rmec;
        fs::remove(tmp, rmec);
        return false;
    }

    IDTX_LOG(IDTX_INFO, "Committed session overrides into '{}'.", original_path);
    return true;
}

bool StageCommitter::CommitUsdz(const pxr::UsdStageRefPtr& stage,
                                const std::string& original_path,
                                std::string& out_error)
{
    pxr::SdfLayerRefPtr merged = pxr::UsdUtilsFlattenLayerStack(stage);
    if (!merged)
    {
        out_error = "UsdUtilsFlattenLayerStack returned null";
        return false;
    }

    RemoveSoftDeletedPrims(stage, merged);

    const fs::path dst(original_path);
    const fs::path tmpDir = dst.parent_path() /
        ("." + dst.stem().string() + "_idtx_commit_tmp");

    std::error_code ec;
    fs::remove_all(tmpDir, ec); // best-effort
    fs::create_directories(tmpDir, ec);
    if (ec && !fs::exists(tmpDir))
    {
        out_error = "could not create temp dir '" + tmpDir.string() + "': " + ec.message();
        return false;
    }
    TempDirGuard guard(tmpDir);

    // 1) Extract the source package, preserving entry order. Per the usdz
    //    spec the first entry is the default root layer.
    std::vector<std::string> fileOrder;
    {
        pxr::SdfZipFile zip = pxr::SdfZipFile::Open(original_path);
        if (!zip)
        {
            out_error = "cannot open usdz package '" + original_path + "'";
            return false;
        }
        for (auto it = zip.begin(); it != zip.end(); ++it)
        {
            const std::string name = *it;
            const pxr::SdfZipFile::FileInfo info = it.GetFileInfo();
            if (info.compressionMethod != 0)
            {
                out_error = "compressed entry '" + name + "' in usdz; cannot re-package";
                return false;
            }
            const char* dataPtr = it.GetFile();
            const std::size_t dataSize = info.size;
            if (!dataPtr && dataSize > 0)
            {
                out_error = "could not read entry '" + name + "' from usdz";
                return false;
            }
            const fs::path outFile = tmpDir / name;
            fs::create_directories(outFile.parent_path(), ec);
            std::ofstream ofs(outFile, std::ios::binary | std::ios::trunc);
            if (!ofs)
            {
                out_error = "could not open '" + outFile.string() + "' for writing";
                return false;
            }
            if (dataSize > 0)
                ofs.write(dataPtr, static_cast<std::streamsize>(dataSize));
            fileOrder.push_back(name);
        }
    }
    if (fileOrder.empty())
    {
        out_error = "usdz package '" + original_path + "' is empty";
        return false;
    }

    // 2) Replace the root layer entry (first) with the merged layer. Export via
    //    SdfLayer::Export (NOT UsdStage::Export) so package-internal relative
    //    asset paths (e.g. "0/texture.png") are preserved verbatim; keep the
    //    original root-entry filename so its encoding (.usda/.usdc) is kept.
    const std::string& rootName = fileOrder.front();
    const fs::path rootPath = tmpDir / rootName;
    fs::remove(rootPath, ec); // ensure writable
    if (!merged->Export(rootPath.string()))
    {
        out_error = "failed to export merged root layer to '" + rootPath.string() + "'";
        return false;
    }

    // 3) Re-zip every entry (root layer first) into a temp package, then
    //    atomically rename over the original.
    const fs::path tmpPkg = dst.parent_path() /
        ("." + dst.stem().string() + "_idtx_commit.usdz");
    fs::remove(tmpPkg, ec);
    {
        pxr::SdfZipFileWriter writer = pxr::SdfZipFileWriter::CreateNew(tmpPkg.string());
        if (!writer)
        {
            out_error = "could not create usdz '" + tmpPkg.string() + "'";
            return false;
        }
        for (const std::string& name : fileOrder)
        {
            const fs::path absPath = tmpDir / name;
            if (writer.AddFile(absPath.string(), name).empty())
            {
                out_error = "failed to add '" + name + "' to usdz";
                writer.Discard();
                std::error_code rmec;
                fs::remove(tmpPkg, rmec);
                return false;
            }
        }
        if (!writer.Save())
        {
            out_error = "failed to save usdz '" + tmpPkg.string() + "'";
            std::error_code rmec;
            fs::remove(tmpPkg, rmec);
            return false;
        }
    }

    fs::rename(tmpPkg, dst, ec);
    if (ec)
    {
        out_error = "atomic rename '" + tmpPkg.string() + "' -> '" + dst.string()
                  + "' failed: " + ec.message();
        std::error_code rmec;
        fs::remove(tmpPkg, rmec);
        return false;
    }

    IDTX_LOG(IDTX_INFO, "Committed session overrides into usdz '{}'.", original_path);
    return true;
}

} // namespace session
} // namespace idtx
