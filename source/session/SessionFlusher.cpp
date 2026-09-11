/**
 * @file SessionFlusher.cpp
 * @brief Implementation of the periodic session-layer flush worker.
 */
#include "SessionFlusher.h"

#include <utility>

#include "SessionManager.h"

namespace idtx
{
namespace session
{

SessionFlusher::SessionFlusher(std::shared_ptr<SessionManager> manager,
                               std::chrono::milliseconds interval)
    : m_manager_(std::move(manager))
    , m_interval_(interval)
{
    if (m_manager_)
    {
        m_thread_ = std::thread(&SessionFlusher::Run, this);
        IDTX_LOG(IDTX_INFO, "Session flusher started (interval {}ms).",
                 static_cast<long long>(m_interval_.count()));
    }
}

SessionFlusher::~SessionFlusher()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex_);
        m_stop_ = true;
    }
    m_cv_.notify_all();
    if (m_thread_.joinable()) m_thread_.join();

    // Final synchronous flush so edits made since the last tick are not lost on
    // a clean shutdown.
    if (m_manager_)
    {
        try { m_manager_->FlushDirtySessions(); }
        catch (...) { /* best-effort on shutdown */ }
    }
}

void SessionFlusher::Run()
{
    std::unique_lock<std::mutex> lk(m_mutex_);
    while (!m_stop_)
    {
        m_cv_.wait_for(lk, m_interval_, [this] { return m_stop_.load(); });
        if (m_stop_) break;

        lk.unlock();
        try
        {
            m_manager_->FlushDirtySessions();
        }
        catch (const std::exception& e)
        {
            IDTX_LOG(IDTX_ERROR, "Session flush sweep threw: {}", e.what());
        }
        catch (...)
        {
            IDTX_LOG(IDTX_ERROR, "Session flush sweep threw unknown exception.");
        }
        lk.lock();
    }
}

} // namespace session
} // namespace idtx
