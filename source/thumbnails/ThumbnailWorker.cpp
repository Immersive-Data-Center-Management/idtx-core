#include "thumbnails/ThumbnailWorker.h"

#include <utility>

namespace idtx
{
namespace thumbnails
{

ThumbnailWorker::ThumbnailWorker(std::shared_ptr<ThumbnailGenerator> generator)
    : m_generator_(std::move(generator))
{
    m_thread_ = std::thread(&ThumbnailWorker::Run, this);
}

ThumbnailWorker::~ThumbnailWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        m_stop_ = true;
    }
    m_cv_.notify_all();
    if (m_thread_.joinable()) m_thread_.join();
}

void ThumbnailWorker::Submit(std::filesystem::path usd_file,
                             std::filesystem::path out_path)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_stop_) return;
        m_queue_.push_back(Job{ std::move(usd_file), std::move(out_path) });
    }
    m_cv_.notify_one();
}

std::size_t ThumbnailWorker::PendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex_);
    return m_queue_.size();
}

void ThumbnailWorker::Run()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex_);
            m_cv_.wait(lock, [this] { return m_stop_ || !m_queue_.empty(); });
            if (m_stop_ && m_queue_.empty()) return;
            job = std::move(m_queue_.front());
            m_queue_.pop_front();
        }

        std::string err;
        try
        {
            if (!m_generator_->Generate(job.usd_file, job.out_path, err))
            {
                IDTX_LOG(IDTX_ERROR,
                         "Thumbnail generation failed for '{}': {}",
                         job.usd_file.string(), err);
            }
            else
            {
                IDTX_LOG(IDTX_INFO,
                         "Thumbnail generated: {}",
                         job.out_path.string());
            }
        }
        catch (const std::exception& e)
        {
            IDTX_LOG(IDTX_ERROR,
                     "Thumbnail generator threw for '{}': {}",
                     job.usd_file.string(), e.what());
        }
        catch (...)
        {
            IDTX_LOG(IDTX_ERROR,
                     "Thumbnail generator threw unknown exception for '{}'",
                     job.usd_file.string());
        }
    }
}

} // namespace thumbnails
} // namespace idtx