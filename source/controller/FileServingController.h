/**
 * @file FileServingController.h
 * @brief HTTP REST adapter for USD file browsing, download and upload.
 *
 * The controller is intentionally stateless with respect to per-request data:
 * it parses the request, delegates path validation to a shared
 * @c UsdFileLocator, optionally schedules asynchronous work on the injected
 * @c ThumbnailWorker, and returns a @c crow::response. All filesystem access
 * is confined to the configured uploads root.
 */
#pragma once

#include <memory>

#include <crow/http_request.h>
#include <crow/http_response.h>

#include <idtx/utils/Logger.h>
#include "utils/UsdFileLocator.h"
#include "thumbnails/ThumbnailWorker.h"

namespace idtx { namespace session { class SessionManager; } }

/**
 * @brief HTTP controller for listing, downloading, uploading USD files and
 *        serving their thumbnails.
 *
 * The controller shares a @c UsdFileLocator with other components (see
 * @c ApplicationContext::create) so that path-traversal defence and
 * extension checks stay consistent across the process. The
 * @c ThumbnailWorker is optional; when absent the thumbnail endpoints
 * return a "disabled" status rather than a hard failure.
 */
class FileServingController
{
    IDTX_LOG_CATEGORY("FileServingController")

public:
    /**
     * @brief Default-construct a controller with a lazily-initialised locator.
     *
     * The locator is created on first use with default settings. Intended
     * primarily for tests; production code should pass an explicitly
     * configured locator via the parameterised constructor.
     */
    FileServingController() = default;

    /**
     * @brief Construct a controller with shared collaborators.
     *
     * @param locator         Path resolver used for every incoming request;
     *                        must not be null.
     * @param thumbnails      Optional background worker for asynchronous
     *                        thumbnail generation. When null, thumbnail-
     *                        related endpoints degrade gracefully.
     * @param session_manager Optional session manager. When provided, an
     *                        upload that replaces an existing file on disk
     *                        will trigger a root-layer reload on every live
     *                        session that holds the same USD file
     *                        (see @c UploadFile and
     *                        @c idtx::session::SessionManager::ReloadSessionsForFile).
     *                        Held as a @c weak_ptr because the controller
     *                        must not extend the manager's lifetime.
     */
    explicit FileServingController(std::shared_ptr<idtx::utils::UsdFileLocator> locator,
                                   std::shared_ptr<idtx::thumbnails::ThumbnailWorker> thumbnails = {},
                                   std::shared_ptr<idtx::session::SessionManager> session_manager = {})
        : m_locator_(std::move(locator))
        , m_thumbnails_(std::move(thumbnails))
        , m_session_manager_(session_manager)
    {}
    ~FileServingController() = default;

    /**
     * @brief Enumerate USD files under the configured uploads root.
     *
     * Walks the uploads directory (respecting the locator's allow-list of
     * extensions) and returns a JSON array describing each file (relative
     * path, size, modification time, thumbnail availability). Directory
     * traversal, unreadable entries and files outside the root are silently
     * skipped so the response never leaks server-side layout details.
     *
     * @param req The incoming HTTP request. Query parameters are accepted
     *            for pagination/filtering (see implementation).
     * @return A JSON response with the list of files, or an error response
     *         if the uploads root is not configured or unreadable.
     */
    crow::response GetFileList(const crow::request& req);

    /**
     * @brief HEAD probe for the existence of a specific USD file.
     *
     * Runs the same path validation as @c ServeFile but does not write a
     * body, mirroring the semantics of an HTTP HEAD request.
     *
     * @param filepath  URL-decoded path relative to the uploads root.
     * @return 200 if the file exists and is a valid USD file; a 4xx error
     *         response describing the reason otherwise.
     */
    crow::response FileExists(const std::string& filepath);

    /**
     * @brief Stream a single USD file back to the client.
     *
     * The path is validated with the shared @c UsdFileLocator to reject
     * directory-traversal attempts and non-USD extensions. On success the
     * response uses @c application/octet-stream and includes a
     * @c Content-Disposition header with the original file name.
     *
     * @param filepath  URL-decoded path relative to the uploads root.
     * @return 200 with the file body on success; 4xx on validation failure
     *         or missing file.
     */
    crow::response ServeFile(const std::string& filepath);

    /**
     * @brief Multipart upload endpoint.
     *
     * Accepts a `multipart/form-data` request with the following parts:
     *   - `file`      : binary payload of the USD file (required).
     *   - `path`      : target directory relative to the uploads root (optional; empty == root).
     *   - `filename`  : explicit filename overriding the `file` part's Content-Disposition (optional).
     *   - `overwrite` : `"true"` to allow overwriting an existing file (optional; default false).
     *
     * On success the response body is a JSON @c UploadResponse describing the
     * stored file and the thumbnail's asynchronous status.
     */
    crow::response UploadFile(const crow::request& req);

    /**
     * @brief Serve the thumbnail image belonging to a given USD file.
     * @param usd_filepath  Path (relative to uploads root) of the USD file the
     *                      thumbnail was generated for. The controller derives
     *                      the actual image path as `<dir>/thumbs/<stem>.png`.
     */
    crow::response ServeThumbnail(const std::string& usd_filepath);

    /**
     * @brief HEAD probe for the availability of a thumbnail image.
     *
     * Returns 200 when a thumbnail exists on disk for @p usd_filepath and a
     * 404 otherwise. The USD file itself is not required to exist for this
     * check; only the derived thumbnail path is inspected.
     *
     * @param usd_filepath  Path (relative to uploads root) of the USD file
     *                      whose thumbnail is being queried.
     */
    crow::response ThumbnailExists(const std::string& usd_filepath);

    /**
     * @brief Direct access to the shared file locator.
     *
     * Provided so other components (e.g. @c SessionManager during
     * construction) can reuse the exact same validation rules without
     * reaching into file-serving logic.
     *
     * @return The shared @c UsdFileLocator; may be null when the controller
     *         was default-constructed and the locator has not yet been
     *         lazily initialised.
     */
    const std::shared_ptr<idtx::utils::UsdFileLocator>& GetLocator() const noexcept { return m_locator_; }

private:
    /// Lazily initialise the locator if the controller was default-constructed.
    idtx::utils::UsdFileLocator& Locator();

    /// Resolve `<usd_path>` -> `<dir>/thumbs/<stem>.<ext>` under the uploads
    /// root, applying the same path-traversal defence as @c Locator().Resolve.
    /// Returns Ok even if the thumbnail file does not (yet) exist; the caller
    /// is expected to check separately.
    idtx::utils::UsdFileLocator::Status ResolveThumbnailPath(
        const std::string& usd_filepath,
        std::filesystem::path& out_full,
        std::filesystem::path& out_relative) const;

    std::shared_ptr<idtx::utils::UsdFileLocator>       m_locator_;
    std::shared_ptr<idtx::thumbnails::ThumbnailWorker> m_thumbnails_;
    /// Non-owning link to the session manager. Used only by @c UploadFile
    /// to trigger stage reloads on file replacement. Never dereferenced
    /// without first acquiring the @c weak_ptr into a @c shared_ptr.
    std::weak_ptr<idtx::session::SessionManager>       m_session_manager_;
};
