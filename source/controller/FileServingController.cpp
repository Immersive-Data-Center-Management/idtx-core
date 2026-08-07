#include "FileServingController.h"

#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <crow/multipart.h>
#include <nlohmann/json.hpp>

#include <pxr/pxr.h>
#include <pxr/base/vt/value.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/layer.h>

#include "dto/ErrorResponse.h"
#include "dto/UploadDto.h"
#include "session/SessionManager.h"

namespace fs   = std::filesystem;
using json     = nlohmann::json;

namespace {

/// Cap on the accepted request body. Requests larger than this are rejected
/// with 413. Overridable via IDTX_UPLOAD_MAX_BYTES.
std::uint64_t UploadMaxBytes()
{
    if (const char* v = std::getenv("IDTX_UPLOAD_MAX_BYTES"))
    {
        try { return std::stoull(v); } catch (...) { /* fall through */ }
    }
    return 500ull * 1024ull * 1024ull; // 500 MiB
}

bool ParseBool(const std::string& s, bool fallback = false) noexcept
{
    if (s.empty()) return fallback;
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

/// Extract a form field's body. Returns empty string when the part is absent
/// (Crow returns a default-constructed part in that case, whose body is empty).
///
/// The reference is intentionally non-const because Crow's
/// @c get_part_by_name is a non-const member on the multipart message.
std::string FieldBody(crow::multipart::message& msg, const std::string& name)
{
    const auto& part = msg.get_part_by_name(name);
    return part.body;
}

/// Pull the `filename="..."` value out of a multipart part's Content-Disposition
/// header. Returns empty string when absent.
std::string ExtractPartFilename(const crow::multipart::part& part)
{
    // Crow keeps parsed header params in part.headers["Content-Disposition"].params.
    auto it = part.headers.find("Content-Disposition");
    if (it == part.headers.end()) return {};
    auto p = it->second.params.find("filename");
    if (p == it->second.params.end()) return {};
    return p->second;
}

} // namespace

idtx::utils::UsdFileLocator& FileServingController::Locator()
{
    if (!m_locator_) m_locator_ = std::make_shared<idtx::utils::UsdFileLocator>();
    return *m_locator_;
}

// ---------------------------------------------------------------------------
// File Serving
// ---------------------------------------------------------------------------


crow::response FileServingController::GetFileList(const crow::request& req)
{
    try
    {
        json files = json::array();
        std::string name_filter = req.url_params.get("name_contains") ? req.url_params.get("name_contains") : "";
        std::string ext_filter  = req.url_params.get("extension")     ? req.url_params.get("extension")     : "";

        const std::string root = Locator().GetRoot();

        for (const auto& entry : fs::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied))
        {
            if (!entry.is_regular_file()) continue;

            auto path = entry.path();
            if (!idtx::utils::UsdFileLocator::IsUsdFilename(path.filename().string())) continue;

            auto relative_path = fs::relative(path, root);
            bool match = true;

            if (!name_filter.empty() && path.filename().string().find(name_filter) == std::string::npos)
                match = false;
            if (!ext_filter.empty() && !path.filename().string().ends_with("." + ext_filter))
                match = false;

            if (match)
            {
                files.push_back({
                    {"filepath",  relative_path.string()},
                    {"filename",  path.filename().string()},
                    {"directory", relative_path.parent_path().string()},
                    {"size",      fs::file_size(path)},
                    {"modified",  fs::last_write_time(path).time_since_epoch().count()}
                });
            }
        }

        return crow::response(200, json{{"files", files}}.dump());
    }
    catch (const std::exception& e)
    {
        return crow::response(500, json{{"error", e.what()}}.dump());
    }
}

crow::response FileServingController::FileExists(const std::string& filepath)
{
    try
    {
        fs::path resolved;
        auto status = Locator().Resolve(filepath, resolved);
        if (status != idtx::utils::UsdFileLocator::Status::Ok)
        {
            CROW_LOG_ERROR << "invalid file " << filepath
                           << " ("
                           << idtx::utils::UsdFileLocator::StatusToString(status)
                           << ")";
            crow::response res(404);
            res.body.clear();
            res.set_header("USD-ERROR", "File not found");
            return res;
        }

        pxr::SdfLayerRefPtr layer = pxr::SdfLayer::FindOrOpen(resolved.string());
        if (!layer)
        {
            crow::response res(405);
            res.set_header("USD-ERROR", "Unable to access usd file");
            res.body.clear();
            return res;
        }

        pxr::VtDictionary layer_data = layer->GetCustomLayerData();
        crow::response res(200);
        res.body.clear();

        if (pxr::VtDictionaryIsHolding<std::string>(layer_data, "Timestamp"))
            res.set_header("USD-Timestamp", layer_data["Timestamp"].Get<std::string>());
        if (pxr::VtDictionaryIsHolding<std::string>(layer_data, "Version"))
            res.set_header("USD-Version", layer_data["Version"].Get<std::string>());

        return res;
    }
    catch (const std::exception& e)
    {
        crow::response res(500);
        res.set_header("USD-ERROR", e.what());
        res.body.clear();
        return res;
    }
}

crow::response FileServingController::ServeFile(const std::string& filepath)
{
    try
    {
        fs::path resolved;
        auto status = Locator().Resolve(filepath, resolved);
        if (status != idtx::utils::UsdFileLocator::Status::Ok)
            return crow::response(404, "File not found");

        std::ifstream file(resolved, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

        crow::response res(200, content);
        res.set_header("Content-Type", "application/octet-stream");
        res.set_header("Content-Disposition",
                       "attachment; filename=" + resolved.filename().string());
        return res;
    }
    catch (const std::exception& e)
    {
        return crow::response(500, json{{"error", e.what()}}.dump());
    }
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

crow::response FileServingController::UploadFile(const crow::request& req)
{
    try
    {
        // Size guard first — before allocating multipart parsing state.
        const std::uint64_t max_bytes = UploadMaxBytes();
        if (req.body.size() > max_bytes)
        {
            return idtx::dto::make_error(413, "payload_too_large",
                                         "Upload exceeds the configured size limit.");
        }

        // Crow throws on malformed multipart bodies; convert to a 400.
        crow::multipart::message msg(req);

        const auto& file_part = msg.get_part_by_name("file");
        if (file_part.body.empty())
        {
            return idtx::dto::make_error(400, "invalid_request",
                                         "Missing or empty 'file' part.");
        }

        // Filename: explicit "filename" field wins, otherwise use the file
        // part's Content-Disposition. Both go through the same validator so
        // there is no way to sneak in "../" or absolute paths.
        std::string filename = FieldBody(msg, "filename");
        if (filename.empty()) filename = ExtractPartFilename(file_part);
        if (filename.empty())
        {
            return idtx::dto::make_error(400, "invalid_request",
                                         "No filename provided.");
        }

        const std::string target_dir = FieldBody(msg, "path");
        const bool        overwrite  = ParseBool(FieldBody(msg, "overwrite"), false);

        fs::path full_path, relative_path;
        auto vstatus = Locator().ValidateUploadTarget(target_dir, filename,
                                                     full_path, relative_path);
        switch (vstatus)
        {
            case idtx::utils::UsdFileLocator::Status::Ok:
                break;
            case idtx::utils::UsdFileLocator::Status::NotAUsdFile:
                return idtx::dto::make_error(400, "invalid_request",
                    "Only .usd, .usda, .usdc and .usdz files may be uploaded.");
            case idtx::utils::UsdFileLocator::Status::InvalidEncoding:
                return idtx::dto::make_error(400, "invalid_request",
                    "Path contains invalid percent-encoding.");
            case idtx::utils::UsdFileLocator::Status::OutsideRoot:
                return idtx::dto::make_error(403, "forbidden_path",
                    "Resolved path is outside the uploads root.");
            case idtx::utils::UsdFileLocator::Status::InvalidPath:
            case idtx::utils::UsdFileLocator::Status::NotFound:
            default:
                return idtx::dto::make_error(400, "invalid_request",
                    std::string("Invalid path: ") +
                    idtx::utils::UsdFileLocator::StatusToString(vstatus));
        }

        // Overwrite check (before mkdir so we don't churn the filesystem for
        // rejected requests). We also remember whether the target existed
        // before the rename so we know, after the write succeeds, whether
        // this upload actually replaced an existing file — that's the only
        // case in which live sessions need to be told to reload.
        std::error_code ec;
        const bool target_existed_before = fs::exists(full_path, ec);
        if (target_existed_before && !overwrite)
        {
            return idtx::dto::make_error(409, "conflict",
                "A file with the same name already exists. Set 'overwrite=true' to replace it.");
        }

        // Ensure the target directory exists and *is* still inside the root
        // after symlink resolution.
        const fs::path parent_dir = full_path.parent_path();
        auto dir_status = Locator().EnsureDirectoryInsideRoot(parent_dir, ec);
        if (dir_status != idtx::utils::UsdFileLocator::Status::Ok)
        {
            IDTX_LOG(IDTX_ERROR, "Cannot create upload directory '{}': {} ({})",
                     parent_dir.string(),
                     idtx::utils::UsdFileLocator::StatusToString(dir_status),
                     ec ? ec.message() : "no errno");

            // A permission problem is an operator/deployment misconfiguration
            // (e.g. the uploads volume is not writable by the server's user),
            // not a client error — surface it distinctly so it is not confused
            // with a bad request and is easy to spot in monitoring.
            if (dir_status == idtx::utils::UsdFileLocator::Status::PermissionDenied)
            {
                return idtx::dto::make_error(500, "internal_error",
                    "Upload target directory is not writable by the server. "
                    "This is a server configuration issue.");
            }
            return idtx::dto::make_error(500, "internal_error",
                                         "Failed to prepare target directory.");
        }

        // Write atomically: dump to a sibling temp file, fsync-lite, then rename.
        const fs::path tmp_path = full_path.string() + ".part";
        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                return idtx::dto::make_error(500, "internal_error",
                                             "Failed to open target file for writing.");
            }
            out.write(file_part.body.data(),
                      static_cast<std::streamsize>(file_part.body.size()));
            if (!out)
            {
                std::error_code rmec;
                fs::remove(tmp_path, rmec);
                return idtx::dto::make_error(500, "internal_error",
                                             "Failed to write file bytes.");
            }
        }
        fs::rename(tmp_path, full_path, ec);
        if (ec)
        {
            std::error_code rmec;
            fs::remove(tmp_path, rmec);
            return idtx::dto::make_error(500, "internal_error",
                                         "Failed to finalise uploaded file: " + ec.message());
        }

        // ---- Reload live sessions when this upload replaced an existing
        // file. We only do this for real replacements (not brand-new files)
        // because live sessions can only ever be bound to files that already
        // existed at session-creation time. Errors here are logged but must
        // not fail the HTTP response — the upload itself succeeded.
        if (target_existed_before && overwrite)
        {
            if (auto sm = m_session_manager_.lock())
            {
                try
                {
                    const std::size_t n = sm->ReloadSessionsForFile(
                        relative_path.generic_string());
                    if (n > 0)
                    {
                        IDTX_LOG(IDTX_INFO,
                                 "Upload replaced '{}'; triggered reload on {} live session(s).",
                                 relative_path.generic_string(), n);
                    }
                }
                catch (const std::exception& e)
                {
                    IDTX_LOG(IDTX_ERROR,
                             "ReloadSessionsForFile failed for '{}': {}",
                             relative_path.generic_string(), e.what());
                }
            }
        }

        // ---- Thumbnail: submit asynchronously ------------------------------
        idtx::dto::ThumbnailInfo thumb_info;
        if (m_thumbnails_)
        {
            const std::string ext = m_thumbnails_->Extension();
            const fs::path thumbs_dir  = parent_dir / "thumbs";
            const fs::path thumb_full  = thumbs_dir / (relative_path.stem().string() + ext);
            const fs::path thumb_rel   = (relative_path.parent_path() / "thumbs" /
                                          (relative_path.stem().string() + ext));

            auto thumbs_status = Locator().EnsureDirectoryInsideRoot(thumbs_dir, ec);
            if (thumbs_status != idtx::utils::UsdFileLocator::Status::Ok)
            {
                IDTX_LOG(IDTX_ERROR, "Cannot create thumbs directory '{}': {}",
                         thumbs_dir.string(),
                         idtx::utils::UsdFileLocator::StatusToString(thumbs_status));
                thumb_info.status = idtx::dto::ThumbnailStatus::Failed;
                thumb_info.error  = "Failed to prepare thumbs directory.";
            }
            else
            {
                m_thumbnails_->Submit(full_path, thumb_full);
                thumb_info.status = idtx::dto::ThumbnailStatus::Queued;
                thumb_info.path   = thumb_rel.generic_string();
            }
        }
        else
        {
            thumb_info.status = idtx::dto::ThumbnailStatus::Skipped;
        }

        idtx::dto::UploadResponse resp;
        resp.filepath  = relative_path.generic_string();
        resp.filename  = relative_path.filename().string();
        resp.directory = relative_path.parent_path().generic_string();
        resp.size      = fs::file_size(full_path, ec);
        if (ec) resp.size = file_part.body.size();
        resp.thumbnail = thumb_info;

        crow::response res(201, json(resp).dump());
        res.set_header("Content-Type", "application/json");
        res.set_header("Location", "/api/v1/download/" + resp.filepath);
        return res;
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "UploadFile failed: {}", e.what());
        return idtx::dto::make_error(400, "invalid_request",
                                     std::string("Malformed upload: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// Thumbnail retrieval
// ---------------------------------------------------------------------------

idtx::utils::UsdFileLocator::Status
FileServingController::ResolveThumbnailPath(const std::string& usd_filepath,
                                            fs::path& out_full,
                                            fs::path& out_relative) const
{
    // Reuse the same syntactic validation the download endpoint uses so the
    // client-supplied USD path can't escape the uploads root even when the
    // thumbnail file itself is what we're looking up.
    std::string decoded;
    auto s = m_locator_->ValidatePath(usd_filepath, decoded);
    if (s != idtx::utils::UsdFileLocator::Status::Ok) return s;

    if (!idtx::utils::UsdFileLocator::IsUsdFilename(fs::path(decoded).filename().string()))
        return idtx::utils::UsdFileLocator::Status::NotAUsdFile;

    // Determine the thumbnail extension: prefer the configured worker's
    // extension, but fall back to ".png" when the controller was constructed
    // without a worker (e.g. in tests).
    const std::string ext = m_thumbnails_
        ? std::string(m_thumbnails_->Extension())
        : std::string(".png");

    fs::path decoded_path(decoded);
    fs::path relative = decoded_path.parent_path() / "thumbs" /
                        (decoded_path.stem().string() + ext);

    fs::path candidate = (fs::path(m_locator_->GetRoot()) / relative).lexically_normal();

    // Verify containment under the root.
    fs::path normalised_root = fs::path(m_locator_->GetRoot()).lexically_normal();
    auto mismatch_it = std::mismatch(normalised_root.begin(), normalised_root.end(),
                                     candidate.begin(), candidate.end());
    if (mismatch_it.first != normalised_root.end())
        return idtx::utils::UsdFileLocator::Status::OutsideRoot;
    for (const auto& part : relative)
        if (part == "..") return idtx::utils::UsdFileLocator::Status::OutsideRoot;

    out_full     = candidate;
    out_relative = relative;
    return idtx::utils::UsdFileLocator::Status::Ok;
}

crow::response FileServingController::ThumbnailExists(const std::string& usd_filepath)
{
    try
    {
        fs::path full, rel;
        auto s = ResolveThumbnailPath(usd_filepath, full, rel);
        if (s != idtx::utils::UsdFileLocator::Status::Ok)
            return crow::response(404);

        std::error_code ec;
        if (!fs::exists(full, ec) || !fs::is_regular_file(full, ec))
            return crow::response(404);
        return crow::response(200);
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "ThumbnailExists failed: {}", e.what());
        return crow::response(500);
    }
}

crow::response FileServingController::ServeThumbnail(const std::string& usd_filepath)
{
    try
    {
        fs::path full, rel;
        auto s = ResolveThumbnailPath(usd_filepath, full, rel);
        if (s != idtx::utils::UsdFileLocator::Status::Ok)
            return idtx::dto::make_error(404, "not_found",
                                         "Thumbnail path resolution failed.");

        std::error_code ec;
        if (!fs::exists(full, ec) || !fs::is_regular_file(full, ec))
        {
            return idtx::dto::make_error(404, "not_found",
                                         "Thumbnail has not been generated yet.");
        }

        std::ifstream f(full, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        crow::response res(200, content);
        // Content-Type is inferred from the configured generator's extension;
        // both ".png" and ".jpg"/".jpeg" are commonly supported by browsers.
        const std::string ext = full.extension().string();
        if      (ext == ".png")                res.set_header("Content-Type", "image/png");
        else if (ext == ".jpg" || ext == ".jpeg") res.set_header("Content-Type", "image/jpeg");
        else                                   res.set_header("Content-Type", "application/octet-stream");
        res.set_header("Content-Disposition",
                       "inline; filename=" + full.filename().string());
        return res;
    }
    catch (const std::exception& e)
    {
        IDTX_LOG(IDTX_ERROR, "ServeThumbnail failed: {}", e.what());
        return idtx::dto::make_error(500, "internal_error", e.what());
    }
}
