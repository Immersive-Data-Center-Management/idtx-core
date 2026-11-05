// tests/SharedLayerLockTests.cpp
//
// Exercises idtx::concurrency::SharedLayerLock on its own: reentrancy, the
// refused upgrade, mutual exclusion under contention, writer progress
// against overlapping readers, and the non-blocking path executor workers
// use (try_lock_shared_or_notify). No USD is involved.

#include "thirdparty/doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "concurrency/SharedLayerLock.h"

using idtx::concurrency::SharedLayerLock;

TEST_CASE("shared layer lock: shared is reentrant and an upgrade is refused")
{
    SharedLayerLock lock;
    {
        std::shared_lock outer(lock);
        std::shared_lock inner(lock);
        CHECK_THROWS_AS(lock.lock(), std::logic_error);
        CHECK_FALSE(SharedLayerLock::HeldExclusivelyByThisThread());
    }
    {
        std::unique_lock exclusive(lock);
        CHECK(SharedLayerLock::HeldExclusivelyByThisThread());
        {
            std::shared_lock nested(lock);
        }
        CHECK(SharedLayerLock::HeldExclusivelyByThisThread());
        CHECK_THROWS_AS(lock.lock(), std::logic_error);
    }
    CHECK_FALSE(SharedLayerLock::HeldExclusivelyByThisThread());
}

TEST_CASE("shared layer lock: readers overlap, a writer excludes them")
{
    SharedLayerLock lock;
    long long first = 0, second = 0;
    std::atomic<bool> stop{false};
    std::atomic<int>  torn{0}, inside{0}, max_inside{0}, writer_saw_reader{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i)
    {
        readers.emplace_back([&] {
            while (!stop.load())
            {
                std::shared_lock reading(lock);
                const int now = ++inside;
                int seen = max_inside.load();
                while (now > seen && !max_inside.compare_exchange_weak(seen, now)) {}
                if (first != second) ++torn;
                std::this_thread::sleep_for(std::chrono::microseconds{50});
                --inside;
            }
        });
    }

    for (int i = 0; i < 1000; ++i)
    {
        std::unique_lock writing(lock);
        if (inside.load() != 0) ++writer_saw_reader;
        ++first;
        std::this_thread::yield();
        ++second;
    }
    stop = true;
    for (auto& t : readers) t.join();

    CHECK(torn == 0);
    CHECK(writer_saw_reader == 0);
    CHECK(max_inside > 1);
}

TEST_CASE("shared layer lock: a writer gets in against continuously overlapping readers")
{
    SharedLayerLock lock;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i)
    {
        readers.emplace_back([&, i] {
            std::this_thread::sleep_for(std::chrono::microseconds(i * 250));
            while (!stop.load())
            {
                std::shared_lock reading(lock);
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    double worst_ms = 0;
    for (int i = 0; i < 20; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        {
            std::unique_lock writing(lock);
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (ms > worst_ms) worst_ms = ms;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    stop = true;
    for (auto& t : readers) t.join();

    CHECK(worst_ms < 200.0);
}

TEST_CASE("shared layer lock: try_lock_shared_or_notify succeeds while no writer is around")
{
    SharedLayerLock lock;
    bool called = false;
    REQUIRE(lock.try_lock_shared_or_notify([&] { called = true; }));
    lock.unlock_shared();

    std::unique_lock writing(lock);
    writing.unlock();
    CHECK_FALSE(called);
}

TEST_CASE("shared layer lock: try_lock_shared_or_notify defers while a writer holds the lock")
{
    SharedLayerLock lock;
    std::promise<void> held;
    std::promise<void> release;
    auto release_future = release.get_future();

    std::thread writer([&] {
        std::unique_lock writing(lock);
        held.set_value();
        release_future.wait();
    });
    held.get_future().wait();

    std::atomic<int> called{0};
    CHECK_FALSE(lock.try_lock_shared_or_notify([&] { ++called; }));
    CHECK(called == 0);

    release.set_value();
    writer.join();
    CHECK(called == 1);

    REQUIRE(lock.try_lock_shared_or_notify([&] { ++called; }));
    lock.unlock_shared();
    CHECK(called == 1);
}

TEST_CASE("shared layer lock: try_lock_shared_or_notify defers while a writer only waits")
{
    // Writer preference: a writer waiting for a blocking reader already turns
    // non-blocking readers away, and they are told once it is done.
    SharedLayerLock lock;
    std::promise<void> reader_in;
    std::promise<void> reader_out;
    auto reader_out_future = reader_out.get_future();

    std::thread reader([&] {
        std::shared_lock reading(lock);
        reader_in.set_value();
        reader_out_future.wait();
    });
    reader_in.get_future().wait();

    std::atomic<bool> writer_done{false};
    std::thread writer([&] {
        std::unique_lock writing(lock);
        writer_done = true;
    });

    // Wait until the writer is counted: then the non-blocking attempt fails.
    std::atomic<int> called{0};
    bool deferred = false;
    for (int i = 0; i < 1000 && !deferred; ++i)
    {
        if (lock.try_lock_shared_or_notify([&] { ++called; }))
        {
            lock.unlock_shared();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        else
        {
            deferred = true;
        }
    }
    REQUIRE(deferred);
    CHECK_FALSE(writer_done);

    reader_out.set_value();
    reader.join();
    writer.join();
    CHECK(writer_done);
    CHECK(called == 1);
}
