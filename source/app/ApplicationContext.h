/**
 * @file ApplicationContext.h
 * @brief Composition root for the IDTX-Core process.
 *
 * The context owns all shared, long-lived services (controllers, session
 * manager, thumbnail worker, file locator) and hands references or shared
 * pointers to whoever needs them. Building the context in one place keeps
 * the wiring explicit and testable: the @c create() factory reads any
 * configuration it needs from environment variables and instantiates the
 * dependency graph in a fixed, documented order.
 *
 * The struct is intentionally an aggregate of @c std::shared_ptr members:
 * it is a service locator, not a hierarchy, so callers pick out the exact
 * dependency they need instead of receiving an opaque handle.
 */
#pragma once

#include <cstdlib>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include "controller/AuthController.h"
#include "controller/HealthController.h"
#include "controller/FileServingController.h"
#include "controller/SessionController.h"
#include "controller/WebSocketController.h"
#include "session/SessionManager.h"
#include "session/SessionFlusher.h"
#include "thumbnails/PlaceholderThumbnailGenerator.h"
#if defined(IDTX_ENABLE_IMAGING)
#include "thumbnails/UsdImagingThumbnailGenerator.h"
#endif
#include "thumbnails/ThumbnailWorker.h"
#include "utils/Environment.h"
#include "utils/UsdFileLocator.h"

/**
 * @brief Bag of shared, long-lived services used across the application.
 *
 * Owned by @c idtx::core::Application. Every field is a @c std::shared_ptr
 * so that routes, controllers and background workers can share the same
 * underlying instance without complicated ownership rules. Members may be
 * @c nullptr when the corresponding feature is disabled (e.g. the
 * @c thumbnailWorker when @c IDTX_THUMBNAIL_ENABLED is @c false).
 */
struct ApplicationContext
{
    std::shared_ptr<idtx::utils::UsdFileLocator>       usdFileLocator;
    std::shared_ptr<idtx::thumbnails::ThumbnailWorker> thumbnailWorker;
    std::shared_ptr<HealthController>                  healthController;
    std::shared_ptr<AuthController>                    authController;
    std::shared_ptr<FileServingController>             fileServingController;
    std::shared_ptr<idtx::session::SessionManager>     sessionManager;
    std::shared_ptr<idtx::session::SessionFlusher>     sessionFlusher;
    std::shared_ptr<SessionController>                 sessionController;
    std::shared_ptr<WebSocketController>               webSocketController;

    /**
     * @brief Build a fully-initialised @c ApplicationContext.
     *
     * Instantiates the shared @c UsdFileLocator, optionally the
     * @c ThumbnailWorker (driven by the @c IDTX_THUMBNAIL_ENABLED and
     * @c IDTX_THUMBNAIL_SIZE environment variables), the OAuth2-backed
     * @c AuthController (configured from @c OAUTH_* environment variables),
     * the @c SessionManager and all HTTP/websocket controllers.
     *
     * Environment variables consulted:
     *   - @c IDTX_THUMBNAIL_ENABLED  (default: "true")
     *   - @c IDTX_THUMBNAIL_SIZE     (default: 256)
     *   - @c IDTX_SESSION_IDLE_TIMEOUT_SECONDS (default: 300; 0 disables the reaper)
     *   - @c IDTX_THUMBNAIL_RENDER   (default: "real" when built with imaging;
     *     set to "placeholder" to force the metadata-only generator. Ignored
     *     unless the binary was built with @c IDTX_ENABLE_IMAGING.)
     *   - @c OAUTH_TOKEN_URL, @c OAUTH_CLIENT_ID,
     *     @c OAUTH_CLIENT_SECRET, @c OAUTH_SCOPE
     *
     * @return A ready-to-use context. The caller is expected to keep it
     *         alive for the lifetime of the process.
     */
    static ApplicationContext create()
    {
        ApplicationContext ctx;

        // One UsdFileLocator instance shared by everything that needs to
        // map a request-supplied path to an on-disk USD file. Keeping a
        // single locator keeps the validation rules consistent across
        // file-serving and session creation.
        //
        // The uploads root is configurable via IDTX_UPLOADS_ROOT.
        const std::string uploads_root =
            EnvironmentUtils::get_env("IDTX_UPLOADS_ROOT").value_or("./uploads");
        // The session root folder is configurable via IDTX_SESSIONS_ROOT
        const std::string session_root =
            EnvironmentUtils::get_env("IDTX_SESSIONS_ROOT").value_or("./sessions");
        ctx.usdFileLocator        =
            std::make_shared<idtx::utils::UsdFileLocator>(uploads_root, session_root);

        // Fail fast: make sure the uploads root exists and is writable by this
        // process *now*, at startup, rather than surfacing an opaque
        // "Failed to prepare target directory" on the first upload.
        {
            std::error_code root_ec;
            const auto root_status = ctx.usdFileLocator->EnsureRootExists(root_ec);
            if (root_status != idtx::utils::UsdFileLocator::Status::Ok)
            {
                throw std::runtime_error(
                    "Uploads root '" + uploads_root + "' is not usable ("
                    + idtx::utils::UsdFileLocator::StatusToString(root_status)
                    + (root_ec ? ": " + root_ec.message() : std::string{})
                    + "). Set IDTX_UPLOADS_ROOT and ensure the directory is "
                      "writable by the server process (check volume ownership / "
                      "securityContext.fsGroup in Kubernetes).");
            }
        }

        // Thumbnail generation is opt-out via IDTX_THUMBNAIL_ENABLED=false.
        // When the binary is built with imaging (IDTX_ENABLE_IMAGING), the
        // Hydra-based UsdImagingThumbnailGenerator renders a real preview of
        // the stage (it internally falls back to the placeholder on any render
        // failure). Otherwise, or when IDTX_THUMBNAIL_RENDER=placeholder, the
        // metadata-only placeholder generator is used.
        const auto thumb_enabled =
            EnvironmentUtils::get_env("IDTX_THUMBNAIL_ENABLED").value_or("true");
        if (thumb_enabled != "false" && thumb_enabled != "0")
        {
            std::uint32_t size = 256;
            if (auto s = EnvironmentUtils::get_env("IDTX_THUMBNAIL_SIZE"))
            {
                try { size = static_cast<std::uint32_t>(std::stoul(*s)); }
                catch (...) { /* keep default */ }
            }

            std::shared_ptr<idtx::thumbnails::ThumbnailGenerator> generator;
#if defined(IDTX_ENABLE_IMAGING)
            const auto render_mode =
                EnvironmentUtils::get_env("IDTX_THUMBNAIL_RENDER").value_or("real");
            if (render_mode == "placeholder")
            {
                generator =
                    std::make_shared<idtx::thumbnails::PlaceholderThumbnailGenerator>(size);
            }
            else
            {
                generator =
                    std::make_shared<idtx::thumbnails::UsdImagingThumbnailGenerator>(size);
            }
#else
            generator =
                std::make_shared<idtx::thumbnails::PlaceholderThumbnailGenerator>(size);
#endif
            ctx.thumbnailWorker =
                std::make_shared<idtx::thumbnails::ThumbnailWorker>(generator);
        }

        ctx.healthController      = std::make_shared<HealthController>();

        // OAuth2 client configuration for the authentication endpoint. The
        // token URL and client id are required; client secret and scope are
        // optional and only sent to the IdP when configured.
        const auto tokenUrl     = EnvironmentUtils::get_env("OAUTH_TOKEN_URL").value_or("");
        const auto clientId     = EnvironmentUtils::get_env("OAUTH_CLIENT_ID").value_or("");
        const auto clientSecret = EnvironmentUtils::get_env("OAUTH_CLIENT_SECRET").value_or("");
        const auto scope        = EnvironmentUtils::get_env("OAUTH_SCOPE").value_or("");

        ctx.authController        = std::make_shared<AuthController>(
                                        tokenUrl, clientId, clientSecret, scope);

        // SessionManager takes the locator by value (each manager keeps its
        // own configured root); we pass a copy of the shared instance.
        // Constructed before the FileServingController so the latter can be
        // wired up with a link back to the manager: an upload that replaces
        // an existing USD file will then trigger a root-layer reload on
        // every live session bound to that file.
        //
        // A background reaper destroys any session that has had zero connected
        // clients for IDTX_SESSION_IDLE_TIMEOUT_SECONDS (default 300s). Set the
        // variable to 0 to disable the reaper and keep sessions until an
        // explicit DELETE.
        const auto idle_secs =
            EnvironmentUtils::get_env_u64("IDTX_SESSION_IDLE_TIMEOUT_SECONDS", 300);
        ctx.sessionManager        = std::make_shared<idtx::session::SessionManager>(
                                        *ctx.usdFileLocator,
                                        std::chrono::seconds{idle_secs});

        // Background worker that persists dirty session layers to their
        // sidecar files on a fixed interval (default 2 s). Configurable via
        // IDTX_SESSION_FLUSH_INTERVAL_MS. Declared after the manager so it is
        // torn down (and does its final flush) before the manager is destroyed.
        const auto flush_ms =
            EnvironmentUtils::get_env_u64("IDTX_SESSION_FLUSH_INTERVAL_MS", 2000);
        ctx.sessionFlusher        = std::make_shared<idtx::session::SessionFlusher>(
                                        ctx.sessionManager,
                                        std::chrono::milliseconds{flush_ms});

        ctx.fileServingController = std::make_shared<FileServingController>(
                                        ctx.usdFileLocator,
                                        ctx.thumbnailWorker,
                                        ctx.sessionManager);

        ctx.sessionController     = std::make_shared<SessionController>(ctx.sessionManager);
        ctx.webSocketController   = std::make_shared<WebSocketController>(ctx.sessionManager);

        return ctx;
    }
};