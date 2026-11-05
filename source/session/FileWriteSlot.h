/**
 * @file FileWriteSlot.h
 * @brief Held share of the session manager's single file-write slot.
 */
#pragma once

#include <memory>
#include <semaphore>

namespace idtx::session
{

class SessionManager;

/**
 * @brief Keeps the session manager's file-write slot taken while any copy of
 *        it is alive.
 *
 * Commits and replacing uploads hold the slot from writing a file until its
 * layer is reloaded, which may happen on another thread, so the slot is
 * copyable and released when the last copy is destroyed or reset. A
 * default-constructed slot holds nothing and tests false. Only the
 * SessionManager can create a held one.
 */
class FileWriteSlot
{
public:
    FileWriteSlot() = default;

    /// True while this copy holds the slot.
    explicit operator bool() const noexcept { return static_cast<bool>(m_release_); }

    /// Drop this copy's share; the slot is released once no copy holds it.
    void reset() noexcept { m_release_.reset(); }

private:
    friend class SessionManager;

    // Releases the already acquired semaphore when destroyed. Built in place
    // and never copied, so it releases exactly once.
    struct Release
    {
        explicit Release(std::binary_semaphore& slot) noexcept : slot(slot) {}
        Release(const Release&) = delete;
        Release& operator=(const Release&) = delete;
        ~Release() { slot.release(); }

        std::binary_semaphore& slot;
    };

    /// Take ownership of @p slot, which the caller has already acquired.
    explicit FileWriteSlot(std::binary_semaphore& slot)
        : m_release_(std::make_shared<Release>(slot)) {}

    std::shared_ptr<Release> m_release_;
};

} // namespace idtx::session
