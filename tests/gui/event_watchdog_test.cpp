// EventWatchdog (05 §7.3 R-03, "a watchdog on the UI loop"): the exclusive time of
// each UI-thread dispatch, nested dispatches and harness work left out, the longest
// ones with event type and receiver, overall and while busy, with their own CPU
// time (D-50); and its hook in Application::notify.
//
// Sleeps stand in for waiting and spin_cpu() for work. A busy machine may stretch
// any of them, so the checks never assume how long a stretch took: they bracket
// it with the test's own clock or read lower bounds only.

#include "application.hpp"
#include "diagnostics_test_support.hpp"
#include "event_watchdog.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QTimer>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using wizz::app::EventWatchdog;
using wizz::test::ms_between;
using wizz::test::spin_cpu;

/// The kept dispatch of event type `event`, or null.
[[nodiscard]] const EventWatchdog::Dispatch* find(const EventWatchdog::Stats& stats, std::string_view event)
{
    const auto found = std::ranges::find(stats.longest, event, &EventWatchdog::Dispatch::event);
    return found == stats.longest.end() ? nullptr : &*found;
}

TEST(EventWatchdog, Keeps_the_exclusive_time_of_nested_dispatches)
{
    EventWatchdog watchdog{[] { return false; }};
    QObject outer_receiver;
    QTimer inner_receiver;
    const QEvent outer_event{QEvent::Timer};
    const QEvent inner_event{QEvent::MetaCall};

    const Clock::time_point begin = Clock::now();
    watchdog.enter(&outer_receiver, &outer_event);
    std::this_thread::sleep_for(20ms);
    watchdog.enter(&inner_receiver, &inner_event);  // e.g. processEvents inside a handler
    std::this_thread::sleep_for(80ms);
    watchdog.leave();
    std::this_thread::sleep_for(10ms);
    watchdog.leave();
    const Clock::time_point end = Clock::now();

    const EventWatchdog::Stats& stats = watchdog.stats();
    EXPECT_EQ(stats.dispatches, 2);
    ASSERT_EQ(stats.longest.size(), 2u);
    EXPECT_GE(stats.longest[0].ms, stats.longest[1].ms) << "longest first";
    const EventWatchdog::Dispatch* inner = find(stats, "MetaCall");
    const EventWatchdog::Dispatch* outer = find(stats, "Timer");
    ASSERT_NE(inner, nullptr);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(inner->receiver, "QTimer");
    EXPECT_EQ(outer->receiver, "QObject");
    EXPECT_GE(inner->ms, 75.0);
    EXPECT_GE(outer->ms, 25.0) << "its own 20 + 10 ms";
    // The outer dispatch's own time leaves the nested one out: both fit in the bracket.
    EXPECT_LE(outer->ms + inner->ms, ms_between(begin, end) + 0.5);
    // Waiting is not work.
    EXPECT_LT(inner->cpu_ms, 10.0);
    EXPECT_LT(outer->cpu_ms, 10.0);
    EXPECT_EQ(stats.over_50_ms, (inner->ms > 50.0 ? 1 : 0) + (outer->ms > 50.0 ? 1 : 0));
    EXPECT_EQ(stats.over_100_ms, (inner->ms >= 100.0 ? 1 : 0) + (outer->ms >= 100.0 ? 1 : 0));
    EXPECT_DOUBLE_EQ(stats.longest_ms, std::max(inner->ms, outer->ms));
    EXPECT_EQ(stats.longest_busy_ms, 0.0) << "never busy";
}

TEST(EventWatchdog, A_dispatch_s_CPU_time_is_its_own_work_without_the_nested_one)
{
    EventWatchdog watchdog{[] { return true; }};
    QObject receiver;
    const QEvent outer_event{QEvent::Timer};
    const QEvent inner_event{QEvent::MetaCall};

    watchdog.enter(&receiver, &outer_event);
    spin_cpu(60.0);
    watchdog.enter(&receiver, &inner_event);
    spin_cpu(30.0);
    watchdog.leave();
    std::this_thread::sleep_for(30ms);  // waiting: in the outer dispatch's time, not its CPU time
    watchdog.leave();

    const EventWatchdog::Stats& stats = watchdog.stats();
    const EventWatchdog::Dispatch* outer = find(stats, "Timer");
    const EventWatchdog::Dispatch* inner = find(stats, "MetaCall");
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);
    EXPECT_GE(outer->cpu_ms, 59.5);
    EXPECT_LT(outer->cpu_ms, 70.0) << "without the nested 30 ms";
    EXPECT_GE(inner->cpu_ms, 29.5);
    EXPECT_LT(inner->cpu_ms, 40.0);
    EXPECT_GE(outer->ms, outer->cpu_ms + 25.0) << "the wait is in its time";
    EXPECT_DOUBLE_EQ(stats.longest_busy_cpu_ms, outer->cpu_ms);
    // The longest busy dispatch that other processes did not hold the CPUs during.
    EXPECT_LE(stats.longest_busy_own_ms, stats.longest_busy_ms);
    if (!outer->held_by_others) {
        EXPECT_DOUBLE_EQ(stats.longest_busy_own_ms, std::max(outer->ms, inner->held_by_others ? 0.0 : inner->ms));
    }
}

TEST(EventWatchdog, Harness_work_and_busy_state)
{
    bool busy = true;
    EventWatchdog watchdog{[&busy] { return busy; }};
    QObject receiver;
    const QEvent event{QEvent::Timer};

    const Clock::time_point begin = Clock::now();
    watchdog.enter(&receiver, &event);
    std::this_thread::sleep_for(10ms);
    watchdog.exclude_begin();
    std::this_thread::sleep_for(120ms);  // a screenshot
    spin_cpu(40.0);
    watchdog.exclude_end();
    watchdog.leave();
    const Clock::time_point end = Clock::now();
    const double first_ms = watchdog.stats().longest_ms;
    EXPECT_GE(first_ms, 9.5);
    EXPECT_LE(first_ms, ms_between(begin, end) - 120.0 + 0.5) << "the harness work is left out";
    EXPECT_LT(watchdog.stats().longest_busy_cpu_ms, 20.0) << "so is its CPU time";
    EXPECT_DOUBLE_EQ(watchdog.stats().longest_busy_ms, first_ms);
    EXPECT_TRUE(watchdog.stats().longest.front().busy);

    busy = false;
    watchdog.enter(&receiver, &event);
    std::this_thread::sleep_for(70ms);
    watchdog.leave();
    EXPECT_GE(watchdog.stats().longest_ms, 65.0);
    EXPECT_DOUBLE_EQ(watchdog.stats().longest_busy_ms, first_ms) << "idle work is not busy work";
    const auto idle = std::ranges::find(watchdog.stats().longest, false, &EventWatchdog::Dispatch::busy);
    ASSERT_NE(idle, watchdog.stats().longest.end());
    EXPECT_GE(idle->ms, 65.0);

    watchdog.leave();  // unbalanced: ignored
    EXPECT_EQ(watchdog.stats().dispatches, 2);
}

TEST(EventWatchdog, Keeps_only_the_longest_dispatches)
{
    EventWatchdog watchdog{{}};
    QObject receiver;
    const QEvent event{QEvent::None};
    for (int i = 0; i < 20; ++i) {
        watchdog.enter(&receiver, &event);
        watchdog.leave();
    }
    EXPECT_EQ(watchdog.stats().dispatches, 20);
    EXPECT_EQ(watchdog.stats().longest.size(), EventWatchdog::kKeptDispatches);
}

TEST(EventWatchdog, Application_reports_every_UI_thread_dispatch)
{
    auto* application = qobject_cast<wizz::app::Application*>(QCoreApplication::instance());
    ASSERT_NE(application, nullptr) << "the GUI tests run on wizz::app::Application";
    EventWatchdog watchdog{[] { return true; }};
    application->set_event_watchdog(&watchdog);
    EXPECT_EQ(application->event_watchdog(), &watchdog);
    bool ran = false;
    QTimer::singleShot(0, application, [&ran] {
        std::this_thread::sleep_for(60ms);
        ran = true;
    });
    const bool done = wizz::test::pump_until([&ran] { return ran; });
    application->set_event_watchdog(nullptr);
    ASSERT_TRUE(done);
    EXPECT_GT(watchdog.stats().dispatches, 0);
    EXPECT_GE(watchdog.stats().longest_ms, 55.0);
    EXPECT_GE(watchdog.stats().longest_busy_ms, 55.0);
    const std::int64_t dispatches = watchdog.stats().dispatches;
    wizz::test::pump_for(20ms);
    EXPECT_EQ(watchdog.stats().dispatches, dispatches) << "removed: no more reports";
}

} // namespace
