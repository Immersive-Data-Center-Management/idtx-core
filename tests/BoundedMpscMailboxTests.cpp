// tests/BoundedMpscMailboxTests.cpp
//

#include "thirdparty/doctest/doctest.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "session/BoundedMpscMailbox.h"

using idtx::session::BoundedMpscMailbox;

namespace
{

// Counts live instances so tests can verify that the mailbox destroys every
// value it holds exactly once.
struct Tracked
{
    static inline std::atomic<int> live{0};

    int value = 0;

    explicit Tracked(int v) noexcept : value(v) { ++live; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++live; }
    Tracked& operator=(Tracked&&) noexcept = default;
    Tracked(const Tracked&)            = delete;
    Tracked& operator=(const Tracked&) = delete;
    ~Tracked() { --live; }
};

} // namespace

TEST_CASE("mailbox: empty pop yields nothing")
{
    BoundedMpscMailbox<int, 4> mailbox;
    CHECK(mailbox.EmptyApproximate());
    CHECK(mailbox.ApproximateSize() == 0);
    CHECK_FALSE(mailbox.TryPop().has_value());
    CHECK(BoundedMpscMailbox<int, 4>::MaxSize() == 4);
}

TEST_CASE("mailbox: values come out in FIFO order")
{
    BoundedMpscMailbox<int, 8> mailbox;
    for (int i = 0; i < 5; ++i) REQUIRE(mailbox.TryPush(i));
    CHECK(mailbox.ApproximateSize() == 5);

    for (int i = 0; i < 5; ++i)
    {
        auto value = mailbox.TryPop();
        REQUIRE(value.has_value());
        CHECK(*value == i);
    }
    CHECK_FALSE(mailbox.TryPop().has_value());
    CHECK(mailbox.EmptyApproximate());
}

TEST_CASE("mailbox: rejects pushes at capacity and recovers after a pop")
{
    BoundedMpscMailbox<int, 4> mailbox;
    for (int i = 0; i < 4; ++i) REQUIRE(mailbox.TryPush(i));

    CHECK_FALSE(mailbox.TryPush(99));
    CHECK(mailbox.ApproximateSize() == 4);

    auto first = mailbox.TryPop();
    REQUIRE(first.has_value());
    CHECK(*first == 0);

    CHECK(mailbox.TryPush(4));
    CHECK_FALSE(mailbox.TryPush(5));

    for (int expected = 1; expected <= 4; ++expected)
    {
        auto value = mailbox.TryPop();
        REQUIRE(value.has_value());
        CHECK(*value == expected);
    }
}

TEST_CASE("mailbox: keeps FIFO order across many ring wrap-arounds")
{
    BoundedMpscMailbox<std::uint64_t, 4> mailbox;
    std::uint64_t next_push = 0;
    std::uint64_t next_pop  = 0;

    // Alternate between partially filling and partially draining so the
    // positions wrap the ring many times at different offsets.
    for (int round = 0; round < 1000; ++round)
    {
        const int pushes = 1 + round % 4;
        for (int i = 0; i < pushes; ++i)
        {
            if (!mailbox.TryPush(next_push)) break;
            ++next_push;
        }
        const int pops = 1 + (round * 7) % 4;
        for (int i = 0; i < pops; ++i)
        {
            auto value = mailbox.TryPop();
            if (!value) break;
            REQUIRE(*value == next_pop);
            ++next_pop;
        }
    }
    while (auto value = mailbox.TryPop())
    {
        REQUIRE(*value == next_pop);
        ++next_pop;
    }
    CHECK(next_pop == next_push);
    CHECK(next_push > 1000);
}

TEST_CASE("mailbox: stores move-only values")
{
    BoundedMpscMailbox<std::unique_ptr<int>, 2> mailbox;
    REQUIRE(mailbox.TryPush(std::make_unique<int>(7)));
    REQUIRE(mailbox.TryEmplace(new int(8)));

    auto a = mailbox.TryPop();
    auto b = mailbox.TryPop();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(**a == 7);
    CHECK(**b == 8);
}

TEST_CASE("mailbox: destroys every value exactly once")
{
    Tracked::live = 0;
    {
        BoundedMpscMailbox<Tracked, 8> mailbox;
        for (int i = 0; i < 6; ++i) REQUIRE(mailbox.TryEmplace(i));
        CHECK(Tracked::live == 6);

        {
            auto popped = mailbox.TryPop();
            REQUIRE(popped.has_value());
            CHECK(popped->value == 0);
            CHECK(Tracked::live == 6); // five queued plus the popped one
        }
        CHECK(Tracked::live == 5);
    }
    // The destructor released the values that were never popped.
    CHECK(Tracked::live == 0);
}

TEST_CASE("mailbox: concurrent producers lose nothing and keep per-producer order")
{
    constexpr int           kProducers         = 4;
    constexpr std::uint32_t kItemsPerProducer  = 50000;

    BoundedMpscMailbox<std::uint64_t, 1024> mailbox;
    std::atomic<bool> start{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&, p] {
            while (!start.load()) std::this_thread::yield();
            for (std::uint32_t seq = 0; seq < kItemsPerProducer; ++seq)
            {
                const std::uint64_t item =
                    (static_cast<std::uint64_t>(p) << 32) | seq;
                while (!mailbox.TryPush(item)) std::this_thread::yield();
            }
        });
    }

    std::vector<std::uint32_t> next_seq(kProducers, 0);
    std::uint64_t received    = 0;
    bool          order_ok    = true;
    const std::uint64_t total = std::uint64_t{kProducers} * kItemsPerProducer;

    start.store(true);
    while (received < total)
    {
        auto item = mailbox.TryPop();
        if (!item)
        {
            std::this_thread::yield();
            continue;
        }
        const auto producer = static_cast<std::size_t>(*item >> 32);
        const auto seq      = static_cast<std::uint32_t>(*item & 0xFFFFFFFFu);
        if (producer >= next_seq.size() || seq != next_seq[producer])
        {
            order_ok = false;
        }
        else
        {
            ++next_seq[producer];
        }
        ++received;
    }

    for (auto& t : producers) t.join();

    CHECK(order_ok);
    CHECK_FALSE(mailbox.TryPop().has_value());
    for (int p = 0; p < kProducers; ++p)
    {
        CHECK(next_seq[p] == kItemsPerProducer);
    }
}
