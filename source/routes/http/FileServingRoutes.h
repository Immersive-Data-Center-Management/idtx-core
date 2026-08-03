/**
 * @file FileServingRoutes.h
 * @brief HTTP route registration for the file-serving endpoints.
 *
 * Endpoints registered:
 *   - GET  /api/v1/files                        — list uploaded USD files.
 *   - HEAD /api/v1/download/<path>              — check whether a USD file exists.
 *   - GET  /api/v1/download/<path>              — download a USD file.
 *   - POST /api/v1/upload                       — upload a USD file (multipart).
 *   - HEAD /api/v1/thumbnail/<path>             — check whether a thumbnail is available.
 *   - GET  /api/v1/thumbnail/<path>             — download the thumbnail image
 *                                                 for the given USD file path.
 *
 * The thumbnail routes take the *USD* file path as their argument, not the
 * PNG path — the controller maps `<dir>/<file>.usd(a|c|z)` to
 * `<dir>/thumbs/<file>.<image_ext>` internally. Clients therefore do not need
 * to know how thumbnails are laid out on disk.
 */
#pragma once

#include <crow/common.h>

#include "controller/FileServingController.h"

template<typename CrowApp>
class FileServingRoutes
{
public:
    FileServingRoutes(CrowApp& app, std::shared_ptr<FileServingController> fileController)
        : m_app_(app)
        , m_fileController_(std::move(fileController))
    {}

    ~FileServingRoutes() = default;

    void RegisterRoutes()
    {
        // Route to list available USD files.
        CROW_ROUTE(m_app_, "/api/v1/files")
            .methods(crow::HTTPMethod::Get)
            ([this](const crow::request& req) {
                return m_fileController_->GetFileList(req);
            });

        // Route to test for file existence using a HEAD request.
        CROW_ROUTE(m_app_, "/api/v1/download/<path>")
            .methods(crow::HTTPMethod::Head)
            ([this](const std::string& filepath) {
                return m_fileController_->FileExists(filepath);
            });

        // Route to serve a single file to the client.
        CROW_ROUTE(m_app_, "/api/v1/download/<path>")
            .methods(crow::HTTPMethod::Get)
            ([this](const std::string& filepath) {
                return m_fileController_->ServeFile(filepath);
            });

        // Route to upload a USD file (multipart body).
        CROW_ROUTE(m_app_, "/api/v1/upload")
            .methods(crow::HTTPMethod::Post)
            ([this](const crow::request& req) {
                return m_fileController_->UploadFile(req);
            });

        // Route to test whether a thumbnail exists for a USD file.
        CROW_ROUTE(m_app_, "/api/v1/thumbnail/<path>")
            .methods(crow::HTTPMethod::Head)
            ([this](const std::string& usd_filepath) {
                return m_fileController_->ThumbnailExists(usd_filepath);
            });

        // Route to serve the thumbnail image for a USD file.
        CROW_ROUTE(m_app_, "/api/v1/thumbnail/<path>")
            .methods(crow::HTTPMethod::Get)
            ([this](const std::string& usd_filepath) {
                return m_fileController_->ServeThumbnail(usd_filepath);
            });
    }

private:
    CrowApp&                                 m_app_;
    std::shared_ptr<FileServingController>   m_fileController_;
};