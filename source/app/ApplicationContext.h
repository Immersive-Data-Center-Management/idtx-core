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
#include <memory>
#include <string>

#include "controller/AuthController.h"
#include "controller/HealthController.h"
#include "controller/FileServingController.h"
#include "controller/SessionController.h"
#include "controller/WebSocketController.h"
#include "session/SessionManager.h"
#include "thumbnails/PlaceholderThumbnailGenerator.h"
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
        ctx.usdFileLocator        = std::make_shared<idtx::utils::UsdFileLocator>();

        // Thumbnail generation is opt-out via IDTX_THUMBNAIL_ENABLED=false.
        // The placeholder generator is safe to run in the current container
        // (no imaging deps required); it can later be swapped for a Hydra-
        // based implementation without touching the controller.
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
            auto generator =
                std::make_shared<idtx::thumbnails::PlaceholderThumbnailGenerator>(size);
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
        // every live session bound to that file (see
        // docs/plans/reload-on-upload.md).
        ctx.sessionManager        = std::make_shared<idtx::session::SessionManager>(
                                        *ctx.usdFileLocator);

        ctx.fileServingController = std::make_shared<FileServingController>(
                                        ctx.usdFileLocator,
                                        ctx.thumbnailWorker,
                                        ctx.sessionManager);

        ctx.sessionController     = std::make_shared<SessionController>(ctx.sessionManager);
        ctx.webSocketController   = std::make_shared<WebSocketController>(ctx.sessionManager);

        return ctx;
    }
};