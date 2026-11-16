// JobRunner (06 §2, 01 §14): every job runs on a worker thread, its events and
// completion reach the UI thread through one ordered queued channel tagged with
// the job ID, stale events are dropped, cancellation flows through one stop
// token, and the UI thread never waits for a worker.

#include "diagnostics_test_support.hpp"
#include "job_runner.hpp"
#include "task.hpp"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::app::JobChannel;
using wizz::app::JobId;
using wizz::app::JobRunner;
using wizz::app::Task;
using wizz::test::pump_until;

/// Waits on the worker until `condition` holds or `limit` passes; returns `condition()`.
bool wait_on_worker(const std::function<bool()>& condition, std::chrono::milliseconds limit = wizz::test::kWaitCap)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!condition() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

/// Waits on the worker until `stop` fires or `limit` passes (a cancellable "request").
bool wait_for_stop(const std::stop_token& stop, std::chrono::milliseconds limit = wizz::test::kWaitCap)
{
    return wait_on_worker([&stop] { return stop.stop_requested(); }, limit);
}

/// Tells when the worker thread of a job has ended. arm() (called by the job, on its
/// worker) builds a thread-local guard that the thread destroys last, after JobRunner
/// posted the job's completion and marked the worker finished — so once ended() is true,
/// every post of the job is in the UI thread's queue and the worker can be reaped.
class WorkerEnd {
public:
    void arm()
    {
        thread_local Guard guard;
        guard.target = &ended_;
    }
    [[nodiscard]] bool ended() const noexcept { return ended_.load(std::memory_order_acquire); }

private:
    struct Guard {
        std::atomic<bool>* target = nullptr;
        ~Guard()
        {
            if (target != nullptr) {
                target->store(true, std::memory_order_release);
            }
        }
    };
    std::atomic<bool> ended_{false};
};

TEST(JobRunner, A_job_runs_on_a_worker_and_resumes_its_flow_on_the_UI_thread)
{
    JobRunner runner;
    const std::thread::id ui_thread = std::this_thread::get_id();
    std::thread::id worker_thread;
    std::thread::id resumed_on;

    auto flow = [&]() -> Task<int> {
        const JobId id = runner.new_job_id();
        const int value = co_await runner.run(id, {}, [&](const JobChannel&) {
            worker_thread = std::this_thread::get_id();
            return 7;
        });
        resumed_on = std::this_thread::get_id();
        co_return value * 6;
    };
    Task<int> task = flow();
    task.start();
    EXPECT_FALSE(task.done()) << "the job never runs inline, even when it is instant (CT-SRCH-06)";
    ASSERT_TRUE(pump_until([&] { return task.done(); }));
    EXPECT_EQ(task.result(), 42);
    EXPECT_NE(worker_thread, ui_thread);
    EXPECT_EQ(resumed_on, ui_thread);
    EXPECT_EQ(runner.live_count(), 0u);
}

TEST(JobRunner, Events_arrive_in_order_before_the_completion)
{
    JobRunner runner;
    std::vector<std::string> received;
    auto flow = [&]() -> Task<> {
        const JobId id = runner.new_job_id();
        co_await runner.run(id, {}, [&](const JobChannel& channel) {
            for (int i = 0; i < 50; ++i) {
                channel.post([&received, i] { received.push_back("event " + std::to_string(i)); });
            }
        });
        received.push_back("completed");
    };
    Task<> task = flow();
    task.start();
    ASSERT_TRUE(pump_until([&] { return task.done(); }));
    ASSERT_EQ(received.size(), 51u);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(received[static_cast<std::size_t>(i)], "event " + std::to_string(i));
    }
    EXPECT_EQ(received.back(), "completed");
}

TEST(JobRunner, The_work_exception_is_rethrown_at_the_await)
{
    JobRunner runner;
    std::string caught;
    auto flow = [&]() -> Task<> {
        try {
            co_await runner.run(runner.new_job_id(), {}, [](const JobChannel&) -> int {
                throw std::runtime_error("worker failed");
            });
        } catch (const std::runtime_error& error) {
            caught = error.what();
        }
    };
    Task<> task = flow();
    task.start();
    ASSERT_TRUE(pump_until([&] { return task.done(); }));
    EXPECT_EQ(caught, "worker failed");
}

TEST(JobRunner, The_callers_stop_request_reaches_the_job)
{
    JobRunner runner;
    std::stop_source cancel;
    std::atomic<bool> running{false};
    bool stopped = false;
    auto flow = [&]() -> Task<> {
        stopped = co_await runner.run(runner.new_job_id(), cancel.get_token(), [&running](const JobChannel& channel) {
            running.store(true);
            return wait_for_stop(channel.stop_token());
        });
    };
    Task<> task = flow();
    task.start();
    ASSERT_TRUE(pump_until([&] { return running.load(); }));
    cancel.request_stop();  // while the job runs
    ASSERT_TRUE(pump_until([&] { return task.done(); }));
    EXPECT_TRUE(stopped);
}

TEST(JobRunner, The_UI_thread_keeps_running_while_a_job_works)
{
    // The job lasts until it has seen the UI thread's 10 ms timer fire five times. A UI
    // thread that waited for the job (or ran it inline) would never let it see them.
    constexpr int kTicks = 5;
    JobRunner runner;
    std::atomic<int> ticks{0};
    QTimer timer;
    timer.setInterval(10ms);
    QObject::connect(&timer, &QTimer::timeout, [&] { ticks.fetch_add(1); });
    timer.start();
    bool saw_ticks = false;
    auto flow = [&]() -> Task<> {
        saw_ticks = co_await runner.run(runner.new_job_id(), {}, [&ticks](const JobChannel& channel) {
            const int at_start = ticks.load();
            return wait_on_worker([&] { return ticks.load() - at_start >= kTicks || channel.stop_token().stop_requested(); }) &&
                   !channel.stop_token().stop_requested();
        });
    };
    Task<> task = flow();
    task.start();
    ASSERT_TRUE(pump_until([&] { return task.done(); }));
    EXPECT_TRUE(saw_ticks) << "the 10 ms timer kept firing while the job ran";
}

TEST(JobRunner, Events_of_an_abandoned_job_are_dropped_and_its_flow_is_not_resumed)
{
    JobRunner runner;
    std::atomic<bool> release{false};
    WorkerEnd worker_end;
    int delivered = 0;
    bool resumed = false;
    JobId id = 0;
    {
        auto flow = [&]() -> Task<> {
            id = runner.new_job_id();
            co_await runner.run(id, {}, [&](const JobChannel& channel) {
                worker_end.arm();
                static_cast<void>(wait_on_worker([&] { return release.load(); }));
                channel.post([&delivered] { ++delivered; });
            });
            resumed = true;
        };
        Task<> task = flow();
        task.start();
        EXPECT_TRUE(runner.is_live(id));
    }  // the flow's frame is destroyed while the job runs (window closed)
    EXPECT_FALSE(runner.is_live(id));
    release.store(true);
    // Once the worker has ended, its event and its completion are queued; deliver them.
    ASSERT_TRUE(pump_until([&] { return worker_end.ended(); }));
    ASSERT_TRUE(wizz::test::drain_posted_events());
    EXPECT_EQ(delivered, 0) << "a stale job's events are dropped";
    EXPECT_FALSE(resumed);
}

TEST(JobRunner, Finished_workers_are_joined_when_the_next_job_starts)
{
    JobRunner runner;
    for (int round = 0; round < 3; ++round) {
        WorkerEnd worker_end;
        auto flow = [&]() -> Task<> {
            co_await runner.run(runner.new_job_id(), {}, [&worker_end](const JobChannel&) { worker_end.arm(); });
        };
        Task<> task = flow();
        task.start();
        ASSERT_TRUE(pump_until([&] { return task.done(); }));
        // The worker marks itself finished right after its completion post; wait for that.
        ASSERT_TRUE(pump_until([&] { return worker_end.ended(); }));
    }
    EXPECT_EQ(runner.thread_count(), 1u) << "the earlier workers were joined; the last waits for the next job";
}

TEST(JobRunner, Destroying_the_runner_stops_and_joins_its_workers)
{
    constexpr std::chrono::milliseconds kJobLimit = 10s;  // the job ends by itself after this
    std::atomic<bool> running{false};
    std::atomic<bool> saw_stop{false};
    std::chrono::steady_clock::time_point job_started;  // written before `running`
    const auto flow = [&](JobRunner& r) -> Task<> {
        co_await r.run(r.new_job_id(), {}, [&](const JobChannel& channel) {
            job_started = std::chrono::steady_clock::now();
            running.store(true);
            saw_stop.store(wait_for_stop(channel.stop_token(), kJobLimit));
        });
    };
    std::optional<Task<>> task;
    {
        auto runner = std::make_unique<JobRunner>();
        task.emplace(flow(*runner));
        task->start();
        ASSERT_TRUE(wait_on_worker([&] { return running.load(); })) << "the job never started";
        runner.reset();  // shutdown
    }
    EXPECT_TRUE(saw_stop.load()) << "the job ended by its stop token, not by its own limit";
    EXPECT_LT(std::chrono::steady_clock::now() - job_started, kJobLimit) << "the shutdown waited the job out";
    EXPECT_FALSE(task->done()) << "the flow is never resumed after shutdown";
}

// --- JobMailbox: one queued delivery per burst (05 §5, R-03) -------------------------------

TEST(JobMailbox, A_burst_is_delivered_in_order_in_one_delivery_before_the_completion)
{
    // The UI thread is held (it handles no event) until the worker has pushed the whole
    // burst, as when a fully cached replay outruns the UI: however the threads are
    // scheduled, every push after the first finds a delivery pending.
    JobRunner runner;
    std::vector<int> received;
    int deliveries = 0;
    std::vector<std::string> order;
    std::binary_semaphore pushed{0};
    constexpr int kItems = 500;
    auto flow = [&]() -> Task<> {
        const wizz::app::JobMailbox<int> mailbox{[&](std::vector<int> items) {
            ++deliveries;
            received.insert(received.end(), items.begin(), items.end());
            order.emplace_back("delivery");
        }};
        co_await runner.run(runner.new_job_id(), {}, [mailbox, &pushed](const JobChannel& channel) {
            for (int i = 0; i < kItems; ++i) {
                mailbox.push(channel, i);
            }
            pushed.release();
        });
        order.emplace_back("completed");
    };
    Task<> task = flow();
    task.start();
    ASSERT_TRUE(pushed.try_acquire_for(wizz::test::kWaitCap)) << "the worker never finished its burst";
    ASSERT_TRUE(pump_until([&] { return task.done(); }));
    ASSERT_EQ(received.size(), static_cast<std::size_t>(kItems));
    for (int i = 0; i < kItems; ++i) {
        EXPECT_EQ(received[static_cast<std::size_t>(i)], i);
    }
    EXPECT_EQ(deliveries, 1) << "a burst is coalesced into one queued call";
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(order.back(), "completed") << "every item arrives before the completion";
}

TEST(JobMailbox, Items_pushed_after_a_delivery_start_a_new_one)
{
    JobRunner runner;
    std::vector<std::vector<int>> deliveries;
    std::atomic<int> delivered{0};
    auto flow = [&]() -> Task<> {
        const wizz::app::JobMailbox<int> mailbox{[&](std::vector<int> items) {
            deliveries.push_back(std::move(items));
            delivered.fetch_add(1);
        }};
        co_await runner.run(runner.new_job_id(), {}, [mailbox, &delivered](const JobChannel& channel) {
            mailbox.push(channel, 1);
            mailbox.push(channel, 2);
            // The UI drained the first burst (or the runner is shutting down a failed test).
            static_cast<void>(wait_on_worker(
                [&] { return delivered.load() >= 1 || channel.stop_token().stop_requested(); }));
            mailbox.push(channel, 3);
        });
    };
    Task<> task = flow();
    task.start();
    ASSERT_TRUE(pump_until([&] { return task.done(); }));
    ASSERT_GE(deliveries.size(), 2u) << "3 came after a delivery had run";
    std::vector<int> all;
    for (const std::vector<int>& delivery : deliveries) {
        EXPECT_FALSE(delivery.empty());
        all.insert(all.end(), delivery.begin(), delivery.end());
    }
    EXPECT_EQ(all, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(deliveries.back().back(), 3);
}

TEST(JobMailbox, Nothing_is_delivered_for_an_abandoned_job)
{
    JobRunner runner;
    std::atomic<bool> release{false};
    WorkerEnd worker_end;
    int delivered = 0;
    {
        auto flow = [&]() -> Task<> {
            const wizz::app::JobMailbox<int> mailbox{[&delivered](std::vector<int>) { ++delivered; }};
            co_await runner.run(runner.new_job_id(), {}, [&, mailbox](const JobChannel& channel) {
                worker_end.arm();
                static_cast<void>(wait_on_worker([&] { return release.load(); }));
                mailbox.push(channel, 7);
            });
        };
        Task<> task = flow();
        task.start();
    }  // the flow's frame goes away while the job runs
    release.store(true);
    ASSERT_TRUE(pump_until([&] { return worker_end.ended(); }));
    ASSERT_TRUE(wizz::test::drain_posted_events());  // the delivery the push posted has been handled
    EXPECT_EQ(delivered, 0);
}

} // namespace
