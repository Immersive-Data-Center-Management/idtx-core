#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace idtx::session
{

/**
 * @brief Requirements for values stored in a BoundedMpscMailbox.
 *
 * Values must be movable without throwing because the consumer moves values
 * out of mailbox storage. Values must also be destructible without throwing.
 */
template<typename T>
concept MailboxValue =
    std::move_constructible<T> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_destructible_v<T>;

/**
 * @brief Requirements for the fixed mailbox capacity.
 *
 * A power-of-two capacity allows slot selection through a bit mask instead
 * of a modulo operation.
 */
template<std::size_t Capacity>
concept ValidMailboxCapacity =
    Capacity >= 2 &&
    std::has_single_bit(Capacity);

/**
 * @brief Fixed-capacity, lock-free, multiple-producer single-consumer mailbox.
 *
 * Multiple producer threads may concurrently call TryPush() or TryEmplace().
 * Exactly one consumer thread may call TryPop().
 *
 * Properties:
 *
 *  - fixed capacity with exactly Capacity usable slots
 *  - no allocation performed by the mailbox after construction
 *  - non-blocking enqueue and dequeue operations
 *  - FIFO according to successful producer position reservation
 *  - ordering from each individual producer is preserved
 *  - values are constructed directly within preallocated slot storage
 *
 * Capacity must be a power of two and at least two.
 *
 * The mailbox must not be destroyed while any producer or consumer is still
 * accessing it. Its owner is responsible for stopping producers and the
 * consumer before destruction.
 *
 * @tparam T        Type stored in the mailbox.
 * @tparam Capacity Maximum number of values stored simultaneously.
 */
template<MailboxValue T, std::size_t Capacity>
requires ValidMailboxCapacity<Capacity>
class BoundedMpscMailbox final
{
private:
    static constexpr std::size_t kIndexMask = Capacity - 1;

    /*
     * Conservative cache-line size for mainstream desktop and server
     * platforms. This separates producer and consumer positions to reduce
     * false sharing.
     */
    static constexpr std::size_t kCacheLineSize = 64;

    /**
     * @brief One ring-buffer slot.
     *
     * The sequence value represents both the slot state and the ring
     * generation for which that state is valid.
     */
    struct Slot
    {
        std::atomic<std::size_t> sequence{0};

        alignas(T) std::byte storage[sizeof(T)]{};

        [[nodiscard]]
        T* Value() noexcept
        {
            return std::launder(
                reinterpret_cast<T*>(storage));
        }

        [[nodiscard]]
        const T* Value() const noexcept
        {
            return std::launder(
                reinterpret_cast<const T*>(storage));
        }
    };

public:
    /**
     * @brief Constructs an empty mailbox.
     *
     * Each slot initially receives the sequence value corresponding to its
     * first enqueue position.
     */
    BoundedMpscMailbox() noexcept
    {
        for (std::size_t index = 0; index < Capacity; ++index)
        {
            slots_[index].sequence.store(
                index,
                std::memory_order_relaxed);
        }
    }

    /**
     * @brief Destroys all remaining published values.
     *
     * Destruction requires external synchronization. No producer may have a
     * reserved but unpublished position, and no thread may still be accessing
     * the mailbox.
     */
    ~BoundedMpscMailbox() noexcept
    {
        DestroyRemaining();
    }

    BoundedMpscMailbox(const BoundedMpscMailbox&) = delete;
    BoundedMpscMailbox& operator=(const BoundedMpscMailbox&) = delete;

    BoundedMpscMailbox(BoundedMpscMailbox&&) = delete;
    BoundedMpscMailbox& operator=(BoundedMpscMailbox&&) = delete;

    /**
     * @brief Attempts to copy a value into the mailbox.
     *
     * This overload only participates in overload resolution when T can be
     * copied without throwing.
     *
     * @param value Value to copy into the mailbox.
     *
     * @return true if the value was published, or false if the mailbox was
     *         full when the enqueue operation was attempted.
     */
    [[nodiscard]]
    bool TryPush(const T& value) noexcept
        requires std::is_nothrow_copy_constructible_v<T>
    {
        return TryEmplace(value);
    }

    /**
     * @brief Attempts to move a value into the mailbox.
     *
     * @param value Value to move into the mailbox.
     *
     * @return true if the value was published, or false if the mailbox was
     *         full when the enqueue operation was attempted.
     */
    [[nodiscard]]
    bool TryPush(T&& value) noexcept
    {
        return TryEmplace(std::move(value));
    }

    /**
     * @brief Attempts to construct and publish a value in the mailbox.
     *
     * Multiple producer threads may call this function concurrently.
     *
     * Construction must not throw. Once a producer reserves a queue position,
     * an exception during construction would leave an unpublished hole at that
     * position and prevent the consumer from progressing.
     *
     * The function does not wait for the consumer to create capacity. It may
     * retry an atomic compare-exchange while racing another producer, but it
     * returns false if the target slot has not yet been released by the
     * consumer.
     *
     * @param args Constructor arguments forwarded to T.
     *
     * @return true if the value was constructed and published, or false if the
     *         mailbox was full when the enqueue operation was attempted.
     */
    template<typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
    [[nodiscard]]
    bool TryEmplace(Args&&... args) noexcept
    {
        std::size_t position =
            enqueue_position_.load(std::memory_order_relaxed);

        Slot* slot = nullptr;

        for (;;)
        {
            slot = &slots_[position & kIndexMask];

            const std::size_t sequence =
                slot->sequence.load(std::memory_order_acquire);

            const std::intptr_t difference =
                static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position);

            if (difference == 0)
            {
                /*
                 * This slot is available for the observed queue position.
                 *
                 * A successful compare-exchange reserves the position for
                 * this producer. No other producer can construct a value at
                 * this position for the current ring generation.
                 */
                if (enqueue_position_.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    break;
                }

                /*
                 * compare_exchange_weak updates position with the currently
                 * observed enqueue position on failure.
                 */
                continue;
            }

            if (difference < 0)
            {
                /*
                 * The slot still contains an unconsumed value from the
                 * current ring generation. The mailbox is full from this
                 * producer's perspective.
                 */
                return false;
            }

            /*
             * Another producer advanced the enqueue position before this
             * producer inspected the corresponding slot. Refresh the
             * position and retry.
             */
            position =
                enqueue_position_.load(std::memory_order_relaxed);
        }

        /*
         * Construct T directly within the preallocated slot storage.
         *
         * The operation is guaranteed not to throw by the TryEmplace
         * constraint.
         */
        ::new (static_cast<void*>(slot->storage))
            T(std::forward<Args>(args)...);

        /*
         * Publish the fully constructed value.
         *
         * The consumer performs an acquire-load of this sequence value. That
         * acquire-load synchronizes with this release-store and makes the
         * constructed value visible to the consumer.
         */
        slot->sequence.store(
            position + 1,
            std::memory_order_release);

        return true;
    }

    /**
     * @brief Attempts to remove the next published value.
     *
     * Exactly one thread may call this function. Concurrent consumers are not
     * supported.
     *
     * A null result means the head position is not currently published. This
     * can mean either:
     *
     *  - the mailbox contains no reserved positions, or
     *  - a producer reserved the head position but has not published it yet
     *
     * The consumer does not skip a reserved but unpublished head position,
     * because doing so would violate FIFO ordering.
     *
     * @return The next value, or std::nullopt when the head position is not
     *         currently ready.
     */
    [[nodiscard]]
    std::optional<T> TryPop() noexcept
    {
        const std::size_t position =
            dequeue_position_.load(std::memory_order_relaxed);

        Slot& slot = slots_[position & kIndexMask];

        const std::size_t sequence =
            slot.sequence.load(std::memory_order_acquire);

        const std::intptr_t difference =
            static_cast<std::intptr_t>(sequence) -
            static_cast<std::intptr_t>(position + 1);

        if (difference != 0)
        {
            return std::nullopt;
        }

        T* value = slot.Value();

        std::optional<T> result{
            std::in_place,
            std::move(*value)
        };

        value->~T();

        /*
         * Release the slot for its next ring generation.
         *
         * A producer performs an acquire-load of the sequence before
         * constructing another value in this storage.
         */
        slot.sequence.store(
            position + Capacity,
            std::memory_order_release);

        dequeue_position_.store(
            position + 1,
            std::memory_order_relaxed);

        return result;
    }

    /**
     * @brief Returns an approximate number of reserved positions.
     *
     * The result includes positions that producers have reserved but have not
     * yet published.
     *
     * Under concurrent activity, this value is suitable only for diagnostics,
     * monitoring, and telemetry. It must not be used to decide whether a
     * subsequent TryPush() or TryPop() operation will succeed.
     */
    [[nodiscard]]
    std::size_t ApproximateSize() const noexcept
    {
        const std::size_t enqueue =
            enqueue_position_.load(std::memory_order_acquire);

        const std::size_t dequeue =
            dequeue_position_.load(std::memory_order_acquire);

        const std::size_t difference = enqueue - dequeue;

        return difference < Capacity
            ? difference
            : Capacity;
    }

    /**
     * @brief Returns an approximate observation of whether the mailbox is empty.
     *
     * The result can immediately become outdated because producers may enqueue
     * concurrently.
     *
     * This function must not be used as part of a consumer wake-up protocol.
     */
    [[nodiscard]]
    bool EmptyApproximate() const noexcept
    {
        return enqueue_position_.load(std::memory_order_acquire) ==
               dequeue_position_.load(std::memory_order_acquire);
    }

    /**
     * @brief Returns the mailbox's fixed capacity.
     */
    [[nodiscard]]
    static consteval std::size_t MaxSize() noexcept
    {
        return Capacity;
    }

private:
    /**
     * @brief Destroys all consecutively published values remaining at shutdown.
     *
     * The mailbox owner must stop all producers before calling the destructor.
     * In particular, no producer may have reserved a position without
     * publishing it.
     */
    void DestroyRemaining() noexcept
    {
        for (;;)
        {
            const std::size_t position =
                dequeue_position_.load(std::memory_order_relaxed);

            Slot& slot = slots_[position & kIndexMask];

            const std::size_t sequence =
                slot.sequence.load(std::memory_order_relaxed);

            const std::intptr_t difference =
                static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position + 1);

            if (difference != 0)
            {
                return;
            }

            slot.Value()->~T();

            slot.sequence.store(
                position + Capacity,
                std::memory_order_relaxed);

            dequeue_position_.store(
                position + 1,
                std::memory_order_relaxed);
        }
    }

    std::array<Slot, Capacity> slots_{};

    /*
     * Shared between all producers.
     *
     * Cache-line alignment reduces false sharing with the consumer position.
     */
    alignas(kCacheLineSize)
    std::atomic<std::size_t> enqueue_position_{0};

    /*
     * Written only by the single consumer.
     *
     * It remains atomic so ApproximateSize() and EmptyApproximate() may read
     * it from other threads without introducing a data race.
     */
    alignas(kCacheLineSize)
    std::atomic<std::size_t> dequeue_position_{0};
};

} // namespace idtx::session