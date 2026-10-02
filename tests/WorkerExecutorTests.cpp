// tests/WorkerExecutorTests.cpp
//

#include "thirdparty/doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include "concurrency/WorkerExecutor.h"

using idtx::concurrency::WorkerExecutor;

TEST_CASE("executor: rejects a zero worker count")
{
    CHECK_THROWS_AS(WorkerExecutor{0}, std::invalid_argument);
}

TEST_CASE("executor: runs posted tasks on its worker threads")
{
    WorkerExecutor executor(2);
    CHECK(executor.WorkerCount() == 2);

    std::promise<std::thread::id> ran_on;
    auto future = ran_on.get_future();
    REQUIRE(executor.Post([&] { ran_on.set_value(std::this_thread::get_id()); }));

    REQUIRE(future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    CHECK(future.get() != std::this_thread::get_id());
}

TEST_CASE("executor: rejects empty tasks")
{
    WorkerExecutor executor(1);
    CHECK_FALSE(executor.Post(WorkerExecutor::Task{}));
}

TEST_CASE("executor: Stop finishes every accepted task, then rejects new ones")
{
    WorkerExecutor executor(2);
    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i)
    {
        REQUIRE(executor.Post([&] {
            std::this_thread::sleep_for(std::chrono::microseconds{200});
            ++done;
        }));
    }

    executor.Stop();
    CHECK(done == 100);
    CHECK_FALSE(executor.Post([] {}));

    // A second Stop is a no-op.
    executor.Stop();
}

TEST_CASE("executor: a throwing task does not take down its worker")
{
    WorkerExecutor executor(1);
    REQUIRE(executor.Post([] { throw std::runtime_error("boom"); }));

    std::promise<void> after;
    auto future = after.get_future();
    REQUIRE(executor.Post([&] { after.set_value(); }));
    CHECK(future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
}
