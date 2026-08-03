// tests/support/TestServer.h — in-process test server harness.
//
// A TestServer spins the full CrowApp (routes + middlewares) on a background
// thread and cleans up after itself. It also isolates the file system: every
// instance runs from its own temp directory containing a fresh uploads/
// with the canonical cube.usda fixture, and picks a free ephemeral port so
// tests never collide.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

// Forward declarations of the internal server types to keep this header light.
struct ApplicationContext;
namespace idtx::core { class Server; }

namespace idtx::tests
{

class TestServer
{
public:
    struct Options
    {
        bool thumbnails_enabled = true;      // IDTX_THUMBNAIL_ENABLED
        std::uint32_t thumbnail_size = 64;   // IDTX_THUMBNAIL_SIZE (small = fast in tests)
        bool write_cube_usda      = true;    // create uploads/cube.usda automatically
    };

    static Options WithThumbnails();
    static Options WithoutThumbnails();

    explicit TestServer(Options opts = WithThumbnails());
    ~TestServer();

    TestServer(const TestServer&)            = delete;
    TestServer& operator=(const TestServer&) = delete;

    /// Block until GET /api/v1/health returns 200 (or throw on timeout).
    void WaitReady(std::chrono::milliseconds timeout = std::chrono::milliseconds{10000});

    const std::string& host() const noexcept { return m_host_; }
    std::uint16_t      port() const noexcept { return m_port_; }
    std::string        base_http_url() const;
    std::string        base_ws_url()   const;
    std::filesystem::path uploads_root() const noexcept { return m_work_dir_ / "uploads"; }
    const std::filesystem::path& work_dir() const noexcept { return m_work_dir_; }

private:
    /// Pick a free TCP port by binding to 0.
    static std::uint16_t PickFreePort();
    /// Restore the process' original CWD and best-effort remove the temp dir.
    void Teardown() noexcept;

    Options                                m_opts_{};
    std::filesystem::path                  m_prev_cwd_;
    std::filesystem::path                  m_work_dir_;
    std::string                            m_host_{"127.0.0.1"};
    std::uint16_t                          m_port_{0};

    std::unique_ptr<ApplicationContext>    m_ctx_;
    std::unique_ptr<idtx::core::Server>    m_server_;
    std::thread                            m_thread_;
    std::atomic<bool>                      m_stop_flag_{false};
};

} // namespace idtx::tests