// JobRunner (06 §2, 01 §14): every job runs on a worker thread, its events and
// completion reach the UI thread through one ordered queued channel tagged with
// the job ID, stale events are dropped, cancellation flows through one stop
// token, and the UI thread never waits for a worker.

#include "diagnostics_test_support.hpp"
#include "job_runner.hpp"
#include "task.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <chrono>
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

/// Waits on the worker until `stop` fires or `limit` passes (a cancellable "request").
bool wait_for_stop(const std::stop_token& stop, std::chrono::milliseconds limit)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(2ms);
    }
    return stop.stop_requested();
}

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
    bool stopped = false;
    auto flow = [&]() -> Task<> {
        stopped = co_await runner.run(runner.new_job_id(), cancel.get_token(), [](const JobChannel& channel) {
            return wait_for_stop(channel.stop_token(), 5s);
        });
    };
    Task<> task = flow();
    task.start();
    QTimer::singleShot(50ms, [&] { cancel.request_stop(); });
    ASSERT_TRUE(pump_until([&] { return task.done(); }, 5s));
    EXPECT_TRUE(stopped);
}

TEST(JobRunner, The_UI_thread_keeps_running_while_a_job_works)
{
    JobRunner runner;
    int ticks = 0;
    QTimer timer;
    timer.setInterval(10ms);
    QObject::connect(&timer, &QTimer::timeout, [&] { ++ticks; });
    timer.start();
    auto flow = [&]() -> Task<> {
        co_await runner.run(runner.new_job_id(), {}, [](const JobChannel&) { std::this_thread::sleep_for(300ms); });
    };
    Task<> task = flow();
    task.start();
    ASSERT_TRUE(pump_until([&] { return task.done(); }, 5s));
    EXPECT_GE(ticks, 5) << "the 10 ms timer kept firing during the 300 ms job";
}

TEST(JobRunner, Events_of_an_abandoned_job_are_dropped_and_its_flow_is_not_resumed)
{
    JobRunner runner;
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    int delivered = 0;
    bool resumed = false;
    JobId id = 0;
    {
        auto flow = [&]() -> Task<> {
            id = runner.new_job_id();
            co_await runner.run(id, {}, [&](const JobChannel& channel) {
                while (!release.load()) {
                    std::this_thread::sleep_for(1ms);
                }
                channel.post([&delivered] { ++delivered; });
                finished.store(true);
            });
            resumed = true;
        };
        Task<> task = flow();
        task.start();
        EXPECT_TRUE(runner.is_live(id));
    }  // the flow's frame is destroyed while the job runs (window closed)
    EXPECT_FALSE(runner.is_live(id));
    release.store(true);
    ASSERT_TRUE(pump_until([&] { return finished.load(); }, 5s));
    wizz::test::pump_for(50ms);
    EXPECT_EQ(delivered, 0) << "a stale job's events are dropped";
    EXPECT_FALSE(resumed);
}

TEST(JobRunner, Finished_workers_are_joined_when_the_next_job_starts)
{
    JobRunner runner;
    for (int round = 0; round < 3; ++round) {
        auto flow = [&]() -> Task<> { co_await runner.run(runner.new_job_id(), {}, [](const JobChannel&) {}); };
        Task<> task = flow();
        task.start();
        ASSERT_TRUE(pump_until([&] { return task.done(); }));
        wizz::test::pump_for(20ms);  // the worker thread itself ends right after its completion post
    }
    EXPECT_LE(runner.thread_count(), 1u);
}

TEST(JobRunner, Destroying_the_runner_stops_and_joins_its_workers)
{
    std::atomic<bool> saw_stop{false};
    const auto flow = [&](JobRunner& r) -> Task<> {
        co_await r.run(r.new_job_id(), {}, [&](const JobChannel& channel) {
            saw_stop.store(wait_for_stop(channel.stop_token(), 10s));
        });
    };
    std::optional<Task<>> task;
    QElapsedTimer elapsed;
    {
        auto runner = std::make_unique<JobRunner>();
        task.emplace(flow(*runner));
        task->start();
        elapsed.start();
        runner.reset();  // shutdown
    }
    EXPECT_TRUE(saw_stop.load());
    EXPECT_LT(elapsed.elapsed(), 5000);
    EXPECT_FALSE(task->done()) << "the flow is never resumed after shutdown";
}

// --- JobMailbox: one queued delivery per burst (05 §5, R-03) -------------------------------

TEST(JobMailbox, A_burst_is_delivered_in_order_in_few_deliveries_before_the_completion)
{
    JobRunner runner;
    std::vector<int> received;
    int deliveries = 0;
    std::vector<std::string> order;
    constexpr int kItems = 500;
    auto flow = [&]() -> Task<> {
        const wizz::app::JobMailbox<int> mailbox{[&](std::vector<int> items) {
            ++deliveries;
            received.insert(received.end(), items.begin(), items.end());
            order.emplace_back("delivery");
        }};
        co_await runner.run(runner.new_job_id(), {}, [mailbox](const JobChannel& channel) {
            for (int i = 0; i < kItems; ++i) {
                mailbox.push(channel, i);  // far faster than the UI drains
            }
        });
        order.emplace_back("completed");
    };
    Task<> task = flow();
    task.start();
    ASSERT_TRUE(pump_until([&] { return task.done(); }));
    ASSERT_EQ(received.size(), static_cast<std::size_t>(kItems));
    for (int i = 0; i < kItems; ++i) {
        EXPECT_EQ(received[static_cast<std::size_t>(i)], i);
    }
    EXPECT_GE(deliveries, 1);
    EXPECT_LT(deliveries, kItems / 10) << "a burst is coalesced into few queued calls";
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
            while (delivered.load() < 1) {
                std::this_thread::sleep_for(1ms);  // the UI drained the first burst
            }
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
    std::atomic<bool> finished{false};
    int delivered = 0;
    {
        auto flow = [&]() -> Task<> {
            const wizz::app::JobMailbox<int> mailbox{[&delivered](std::vector<int>) { ++delivered; }};
            co_await runner.run(runner.new_job_id(), {}, [&, mailbox](const JobChannel& channel) {
                while (!release.load()) {
                    std::this_thread::sleep_for(1ms);
                }
                mailbox.push(channel, 7);
                finished.store(true);
            });
        };
        Task<> task = flow();
        task.start();
    }  // the flow's frame goes away while the job runs
    release.store(true);
    ASSERT_TRUE(pump_until([&] { return finished.load(); }, 5s));
    wizz::test::pump_for(50ms);
    EXPECT_EQ(delivered, 0);
}

} // namespace
