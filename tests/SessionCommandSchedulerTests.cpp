// tests/SessionCommandSchedulerTests.cpp
//
// Exercises SessionCommandScheduler with SnapshotCommand values as tagged
// payloads: the connection field carries a test-chosen number, so the handler
// can record which command ran and in what order. No server is involved.

#include "thirdparty/doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "concurrency/WorkerExecutor.h"
#include "session/SessionCommand.h"
#include "session/SessionCommandScheduler.h"

using idtx::concurrency::WorkerExecutor;
using idtx::session::CommandAdmissionStatus;
using idtx::session::SnapshotCommand;
using idtx::session::SessionCommand;
using idtx::session::SessionCommandScheduler;

namespace
{

SessionCommand Tagged(std::uint64_t tag)
{
    return SnapshotCommand{tag};
}

std::uint64_t TagOf(const SessionCommand& command)
{
    return std::get<SnapshotCommand>(command).connection;
}

// Records the tags a scheduler's handler saw, in execution order, and flags
// any overlap between two handler invocations.
struct Recorder
{
    std::mutex                 mutex;
    std::vector<std::uint64_t> tags;
    std::atomic<int>           in_handler{0};
    std::atomic<bool>          overlapped{false};

    SessionCommandScheduler::CommandHandler Handler()
    {
        return [this](SessionCommand&& command) {
            if (in_handler.fetch_add(1) != 0) overlapped = true;
            {
                std::lock_guard lock(mutex);
                tags.push_back(TagOf(command));
            }
            in_handler.fetch_sub(1);
        };
    }

    std::vector<std::uint64_t> Snapshot()
    {
        std::lock_guard lock(mutex);
        return tags;
    }
};

// Occupies one executor worker until Release() is called, so tests can
// build up queued work deterministically.
class WorkerBlocker
{
public:
    explicit WorkerBlocker(WorkerExecutor& executor)
    {
        auto gate = m_gate_.get_future().share();
        REQUIRE(executor.Post([gate] { gate.wait(); }));
    }
    ~WorkerBlocker() { Release(); }

    void Release()
    {
        if (!m_released_)
        {
            m_released_ = true;
            m_gate_.set_value();
        }
    }

private:
    std::promise<void> m_gate_;
    bool               m_released_ = false;
};

} // namespace

TEST_CASE("scheduler: processes commands in submission order")
{
    auto executor = std::make_shared<WorkerExecutor>(4);
    Recorder recorder;
    auto scheduler = SessionCommandScheduler::Create(executor, recorder.Handler());

    constexpr std::uint64_t kCount = 500;
    for (std::uint64_t i = 0; i < kCount; ++i)
    {
        REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    scheduler->WaitIdle();

    const auto tags = recorder.Snapshot();
    REQUIRE(tags.size() == kCount);
    for (std::uint64_t i = 0; i < kCount; ++i) CHECK(tags[i] == i);
    CHECK_FALSE(recorder.overlapped);
    CHECK(scheduler->ApproximatePendingCount() == 0);
}

TEST_CASE("scheduler: concurrent producers get a single consumer and keep their order")
{
    constexpr int           kProducers = 4;
    constexpr std::uint32_t kPerProducer = 5000;

    auto executor = std::make_shared<WorkerExecutor>(4);
    Recorder recorder;
    auto scheduler = SessionCommandScheduler::Create(executor, recorder.Handler());

    std::atomic<bool>        start{false};
    std::atomic<std::size_t> queue_full{0};
    std::atomic<bool>        unexpected_status{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&, p] {
            while (!start.load()) std::this_thread::yield();
            for (std::uint32_t seq = 0; seq < kPerProducer; ++seq)
            {
                const std::uint64_t tag = (static_cast<std::uint64_t>(p) << 32) | seq;
                for (;;)
                {
                    const auto status = scheduler->TrySubmit(Tagged(tag));
                    if (status == CommandAdmissionStatus::Accepted) break;
                    if (status != CommandAdmissionStatus::QueueFull)
                    {
                        unexpected_status = true;
                        return;
                    }
                    ++queue_full;
                    std::this_thread::yield();
                }
            }
        });
    }
    start.store(true);
    for (auto& t : producers) t.join();
    scheduler->WaitIdle();
    REQUIRE_FALSE(unexpected_status);

    const auto tags = recorder.Snapshot();
    REQUIRE(tags.size() == std::size_t{kProducers} * kPerProducer);
    CHECK_FALSE(recorder.overlapped);

    std::vector<std::uint32_t> next(kProducers, 0);
    bool order_ok = true;
    for (const auto tag : tags)
    {
        const auto p   = static_cast<std::size_t>(tag >> 32);
        const auto seq = static_cast<std::uint32_t>(tag & 0xFFFFFFFFu);
        if (p >= next.size() || seq != next[p]) { order_ok = false; break; }
        ++next[p];
    }
    CHECK(order_ok);
    MESSAGE("queue_full retries: " << queue_full.load());
}

TEST_CASE("scheduler: reports QueueFull once the mailbox is full")
{
    auto executor = std::make_shared<WorkerExecutor>(1);
    Recorder recorder;
    auto scheduler = SessionCommandScheduler::Create(executor, recorder.Handler());

    WorkerBlocker blocker(*executor);
    for (std::uint64_t i = 0; i < SessionCommandScheduler::kMailboxCapacity; ++i)
    {
        REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    CHECK(scheduler->TrySubmit(Tagged(9999)) == CommandAdmissionStatus::QueueFull);

    blocker.Release();
    scheduler->WaitIdle();

    const auto tags = recorder.Snapshot();
    REQUIRE(tags.size() == SessionCommandScheduler::kMailboxCapacity);
    CHECK(tags.back() == SessionCommandScheduler::kMailboxCapacity - 1);

    // Capacity is available again after the drain.
    CHECK(scheduler->TrySubmit(Tagged(1)) == CommandAdmissionStatus::Accepted);
    scheduler->WaitIdle();
}

TEST_CASE("scheduler: priority commands overtake queued normal commands")
{
    auto executor = std::make_shared<WorkerExecutor>(1);
    Recorder recorder;
    auto scheduler = SessionCommandScheduler::Create(executor, recorder.Handler());

    WorkerBlocker blocker(*executor);
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    REQUIRE(scheduler->TrySubmitPriority(Tagged(100)) == CommandAdmissionStatus::Accepted);
    REQUIRE(scheduler->TrySubmitPriority(Tagged(101)) == CommandAdmissionStatus::Accepted);
    REQUIRE(scheduler->TrySubmit(Tagged(5)) == CommandAdmissionStatus::Accepted);

    blocker.Release();
    scheduler->WaitIdle();

    CHECK(recorder.Snapshot() == std::vector<std::uint64_t>{100, 101, 0, 1, 2, 3, 4, 5});
    CHECK_FALSE(recorder.overlapped);
}

TEST_CASE("scheduler: a priority command submitted during a drain runs before the next normal one")
{
    auto executor = std::make_shared<WorkerExecutor>(1);

    // Only touched by the consumer; read after WaitIdle().
    std::vector<std::uint64_t>               order;
    bool                                     priority_accepted = false;
    std::shared_ptr<SessionCommandScheduler> scheduler;
    scheduler = SessionCommandScheduler::Create(
        executor,
        [&](SessionCommand&& command) {
            const std::uint64_t tag = TagOf(command);
            order.push_back(tag);
            if (tag == 1)
            {
                priority_accepted = scheduler->TrySubmitPriority(Tagged(200))
                                    == CommandAdmissionStatus::Accepted;
            }
        });

    WorkerBlocker blocker(*executor);
    for (std::uint64_t i = 0; i < 4; ++i)
    {
        REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    blocker.Release();
    scheduler->WaitIdle();

    CHECK(priority_accepted);
    CHECK(order == std::vector<std::uint64_t>{0, 1, 200, 2, 3});
}

TEST_CASE("scheduler: the priority mailbox has its own capacity and admission")
{
    auto executor = std::make_shared<WorkerExecutor>(1);
    Recorder recorder;
    auto scheduler = SessionCommandScheduler::Create(executor, recorder.Handler());

    WorkerBlocker blocker(*executor);
    for (std::uint64_t i = 0; i < SessionCommandScheduler::kPriorityMailboxCapacity; ++i)
    {
        REQUIRE(scheduler->TrySubmitPriority(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    CHECK(scheduler->TrySubmitPriority(Tagged(9999)) == CommandAdmissionStatus::QueueFull);
    // The normal mailbox is still available.
    CHECK(scheduler->TrySubmit(Tagged(5000)) == CommandAdmissionStatus::Accepted);

    scheduler->CloseAdmission();
    CHECK(scheduler->TrySubmitPriority(Tagged(9998)) == CommandAdmissionStatus::Stopping);

    blocker.Release();
    scheduler->WaitIdle();

    const auto tags = recorder.Snapshot();
    REQUIRE(tags.size() == SessionCommandScheduler::kPriorityMailboxCapacity + 1);
    CHECK(tags.back() == 5000);
}

TEST_CASE("scheduler: a busy session yields the worker after one batch")
{
    auto executor = std::make_shared<WorkerExecutor>(1);

    std::mutex                 order_mutex;
    std::vector<std::uint64_t> order; // 0 = busy session, 1 = other session
    auto handler_for = [&](std::uint64_t session) {
        return [&, session](SessionCommand&&) {
            std::lock_guard lock(order_mutex);
            order.push_back(session);
        };
    };
    auto busy  = SessionCommandScheduler::Create(executor, handler_for(0));
    auto other = SessionCommandScheduler::Create(executor, handler_for(1));

    WorkerBlocker blocker(*executor);
    for (std::uint64_t i = 0; i < 512; ++i)
    {
        REQUIRE(busy->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    REQUIRE(other->TrySubmit(Tagged(0)) == CommandAdmissionStatus::Accepted);
    blocker.Release();

    busy->WaitIdle();
    other->WaitIdle();

    std::lock_guard lock(order_mutex);
    REQUIRE(order.size() == 513);
    // The single worker runs the busy session's first batch, then the other
    // session's drain that was queued behind it.
    std::size_t other_index = 0;
    while (other_index < order.size() && order[other_index] != 1) ++other_index;
    CHECK(other_index == SessionCommandScheduler::kMaxCommandsPerDrain);
}

TEST_CASE("scheduler: closing admission rejects new commands but finishes accepted ones")
{
    auto executor = std::make_shared<WorkerExecutor>(1);
    Recorder recorder;
    auto scheduler = SessionCommandScheduler::Create(executor, recorder.Handler());

    WorkerBlocker blocker(*executor);
    for (std::uint64_t i = 0; i < 10; ++i)
    {
        REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    CHECK(scheduler->IsAccepting());
    scheduler->CloseAdmission();
    CHECK_FALSE(scheduler->IsAccepting());
    CHECK(scheduler->TrySubmit(Tagged(100)) == CommandAdmissionStatus::Stopping);

    blocker.Release();
    scheduler->WaitIdle();
    CHECK(recorder.Snapshot().size() == 10);
}

TEST_CASE("scheduler: WaitIdle returns only after the last command finished")
{
    auto executor = std::make_shared<WorkerExecutor>(2);
    std::atomic<int> done{0};
    auto scheduler = SessionCommandScheduler::Create(executor, [&](SessionCommand&&) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        ++done;
    });

    for (std::uint64_t i = 0; i < 50; ++i)
    {
        REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    scheduler->WaitIdle();
    CHECK(done == 50);
}

TEST_CASE("scheduler: a throwing handler does not stop later commands")
{
    auto executor = std::make_shared<WorkerExecutor>(1);
    std::atomic<int> handled{0};
    auto scheduler = SessionCommandScheduler::Create(executor, [&](SessionCommand&& command) {
        ++handled;
        if (TagOf(command) % 2 == 0) throw std::runtime_error("boom");
    });

    for (std::uint64_t i = 0; i < 20; ++i)
    {
        REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }
    scheduler->WaitIdle();
    CHECK(handled == 20);
}

TEST_CASE("scheduler: drains inline when the executor is gone or stopped")
{
    const auto caller = std::this_thread::get_id();

    SUBCASE("executor destroyed")
    {
        std::weak_ptr<WorkerExecutor> gone;
        {
            auto executor = std::make_shared<WorkerExecutor>(1);
            gone = executor;
        }
        REQUIRE(gone.expired());

        std::vector<std::thread::id> threads;
        auto scheduler = SessionCommandScheduler::Create(gone, [&](SessionCommand&&) {
            threads.push_back(std::this_thread::get_id());
        });
        for (std::uint64_t i = 0; i < 3; ++i)
        {
            REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
        }
        // Processed synchronously by TrySubmit itself.
        REQUIRE(threads.size() == 3);
        for (const auto& id : threads) CHECK(id == caller);
        scheduler->WaitIdle();
    }

    SUBCASE("executor stopped")
    {
        auto executor = std::make_shared<WorkerExecutor>(1);
        executor->Stop();

        std::vector<std::thread::id> threads;
        auto scheduler = SessionCommandScheduler::Create(executor, [&](SessionCommand&&) {
            threads.push_back(std::this_thread::get_id());
        });
        REQUIRE(scheduler->TrySubmit(Tagged(0)) == CommandAdmissionStatus::Accepted);
        REQUIRE(threads.size() == 1);
        CHECK(threads.front() == caller);
        scheduler->WaitIdle();
    }
}

namespace
{

// Polls @p done until it holds or @p timeout elapses.
template<typename Predicate>
bool WaitUntil(Predicate done, std::chrono::milliseconds timeout = std::chrono::seconds{5})
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

} // namespace

TEST_CASE("scheduler: a paused session holds no worker and other sessions keep running")
{
    // One worker: if the paused session parked it, the other could not run.
    auto executor = std::make_shared<WorkerExecutor>(1);
    Recorder paused_recorder;
    Recorder other_recorder;
    auto paused = SessionCommandScheduler::Create(executor, paused_recorder.Handler());
    auto other  = SessionCommandScheduler::Create(executor, other_recorder.Handler());

    paused->Pause();
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        REQUIRE(paused->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
        REQUIRE(other->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
    }

    other->WaitIdle();
    CHECK(other_recorder.Snapshot().size() == 5);
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(paused_recorder.Snapshot().empty());
    CHECK(paused->ApproximatePendingCount() == 5);

    paused->Resume();
    paused->WaitIdle();
    CHECK(paused_recorder.Snapshot() == std::vector<std::uint64_t>{0, 1, 2, 3, 4});
}

TEST_CASE("scheduler: Pause returns only after the running command finished")
{
    auto executor = std::make_shared<WorkerExecutor>(2);
    std::promise<void> entered;
    std::promise<void> gate;
    auto gate_future = gate.get_future().share();
    std::atomic<bool> first_done{false};
    Recorder recorder;
    auto record = recorder.Handler();

    auto scheduler = SessionCommandScheduler::Create(executor, [&](SessionCommand&& command) {
        if (TagOf(command) == 0)
        {
            entered.set_value();
            gate_future.wait();
            first_done = true;
        }
        record(std::move(command));
    });

    REQUIRE(scheduler->TrySubmit(Tagged(0)) == CommandAdmissionStatus::Accepted);
    entered.get_future().wait();

    auto pausing = std::async(std::launch::async, [&] { scheduler->Pause(); });
    CHECK(pausing.wait_for(std::chrono::milliseconds{50}) == std::future_status::timeout);

    gate.set_value();
    pausing.wait();
    CHECK(first_done);

    REQUIRE(scheduler->TrySubmit(Tagged(1)) == CommandAdmissionStatus::Accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(recorder.Snapshot() == std::vector<std::uint64_t>{0});

    scheduler->Resume();
    scheduler->WaitIdle();
    CHECK(recorder.Snapshot() == std::vector<std::uint64_t>{0, 1});
}

TEST_CASE("scheduler: pauses nest")
{
    auto executor = std::make_shared<WorkerExecutor>(1);
    Recorder recorder;
    auto scheduler = SessionCommandScheduler::Create(executor, recorder.Handler());

    scheduler->Pause();
    scheduler->Pause();
    REQUIRE(scheduler->TrySubmit(Tagged(0)) == CommandAdmissionStatus::Accepted);

    scheduler->Resume();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(recorder.Snapshot().empty());

    scheduler->Resume();
    scheduler->WaitIdle();
    CHECK(recorder.Snapshot() == std::vector<std::uint64_t>{0});
}

TEST_CASE("scheduler: a deferred command stays pending and runs first after Resume")
{
    auto executor = std::make_shared<WorkerExecutor>(1);
    Recorder recorder;
    auto record = recorder.Handler();
    std::atomic<int> attempts_of_first{0};
    std::shared_ptr<SessionCommandScheduler> scheduler;

    scheduler = SessionCommandScheduler::Create(executor, [&](SessionCommand&& command) {
        if (TagOf(command) == 1 && attempts_of_first.fetch_add(1) == 0)
        {
            scheduler->DeferCurrent(std::move(command));
            return;
        }
        record(std::move(command));
    });

    {
        WorkerBlocker blocker(*executor);
        for (std::uint64_t i = 1; i <= 3; ++i)
        {
            REQUIRE(scheduler->TrySubmit(Tagged(i)) == CommandAdmissionStatus::Accepted);
        }
    }

    REQUIRE(WaitUntil([&] { return attempts_of_first.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(recorder.Snapshot().empty());
    CHECK(scheduler->ApproximatePendingCount() == 3);

    // A priority command submitted meanwhile does not overtake the deferred one.
    REQUIRE(scheduler->TrySubmitPriority(Tagged(9)) == CommandAdmissionStatus::Accepted);

    scheduler->Resume();
    scheduler->WaitIdle();
    CHECK(attempts_of_first == 2);
    CHECK(recorder.Snapshot() == std::vector<std::uint64_t>{1, 9, 2, 3});
}

TEST_CASE("scheduler: a Resume that arrives before its deferral still resumes")
{
    // Models the lock's release callback firing on another thread between the
    // failed attempt and DeferCurrent().
    auto executor = std::make_shared<WorkerExecutor>(1);
    Recorder recorder;
    auto record = recorder.Handler();
    std::atomic<int> attempts{0};
    std::shared_ptr<SessionCommandScheduler> scheduler;

    scheduler = SessionCommandScheduler::Create(executor, [&](SessionCommand&& command) {
        if (attempts.fetch_add(1) == 0)
        {
            scheduler->Resume();
            scheduler->DeferCurrent(std::move(command));
            return;
        }
        record(std::move(command));
    });

    REQUIRE(scheduler->TrySubmit(Tagged(7)) == CommandAdmissionStatus::Accepted);
    REQUIRE(WaitUntil([&] { return recorder.Snapshot().size() == 1; }));
    scheduler->WaitIdle();
    CHECK(attempts == 2);
    CHECK(recorder.Snapshot() == std::vector<std::uint64_t>{7});
}

TEST_CASE("scheduler: pausing while producers submit never runs a command while paused")
{
    auto executor = std::make_shared<WorkerExecutor>(4);
    std::atomic<bool> paused_flag{false};
    std::atomic<int>  ran_while_paused{0};
    std::atomic<int>  handled{0};

    auto scheduler = SessionCommandScheduler::Create(executor, [&](SessionCommand&&) {
        if (paused_flag.load()) ++ran_while_paused;
        ++handled;
    });

    std::atomic<bool> stop{false};
    std::atomic<int>  accepted{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < 3; ++p)
    {
        producers.emplace_back([&] {
            while (!stop.load())
            {
                if (scheduler->TrySubmit(Tagged(0)) == CommandAdmissionStatus::Accepted) ++accepted;
                std::this_thread::yield();
            }
        });
    }

    for (int round = 0; round < 200; ++round)
    {
        scheduler->Pause();
        paused_flag = true;
        std::this_thread::sleep_for(std::chrono::microseconds{200});
        paused_flag = false;
        scheduler->Resume();
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }

    stop = true;
    for (auto& t : producers) t.join();
    scheduler->WaitIdle();

    CHECK(ran_while_paused == 0);
    CHECK(handled == accepted);
}
