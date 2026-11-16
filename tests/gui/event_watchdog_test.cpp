// EventWatchdog (05 §7.3 R-03, "a watchdog on the UI loop"): the exclusive time of
// each UI-thread dispatch, nested dispatches and harness work left out, the longest
// ones with event type and receiver, overall and while busy, with their own CPU
// time (D-50); and its hook in Application::notify.
//
// The wall-time arithmetic runs on a clock the test moves by hand (ManualClock), so it
// is checked exactly, however fast or busy the machine is. CPU time is always the UI
// thread's real CPU time: spin_cpu() stands in for work, moving the manual clock or
// sleeping for waiting. The tests on the default clock (a QElapsedTimer) may be
// stretched by a busy machine, so they read lower bounds only.

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
#include <cstdint>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using wizz::app::EventWatchdog;
using wizz::test::spin_cpu;

/// A clock the test moves by hand, so the watchdog's arithmetic is checked exactly and
/// independently of how fast or busy the machine is (the default clock is a QElapsedTimer;
/// the CPU-time test and the last two tests below run on it).
class ManualClock {
public:
    void advance(std::chrono::milliseconds by) noexcept { ns_ += std::chrono::nanoseconds{by}.count(); }
    [[nodiscard]] EventWatchdog::Clock reader() noexcept
    {
        return [this] { return ns_; };
    }

private:
    std::int64_t ns_ = 0;
};

/// The kept dispatch of event type `event`, or null.
[[nodiscard]] const EventWatchdog::Dispatch* find(const EventWatchdog::Stats& stats, std::string_view event)
{
    const auto found = std::ranges::find(stats.longest, event, &EventWatchdog::Dispatch::event);
    return found == stats.longest.end() ? nullptr : &*found;
}

TEST(EventWatchdog, Keeps_the_exclusive_time_of_nested_dispatches)
{
    ManualClock clock;
    EventWatchdog watchdog{[] { return false; }, clock.reader()};
    QObject outer_receiver;
    QTimer inner_receiver;
    const QEvent outer_event{QEvent::Timer};
    const QEvent inner_event{QEvent::MetaCall};

    watchdog.enter(&outer_receiver, &outer_event);
    clock.advance(20ms);
    watchdog.enter(&inner_receiver, &inner_event);  // e.g. processEvents inside a handler
    clock.advance(80ms);
    watchdog.leave();
    clock.advance(10ms);
    watchdog.leave();

    const EventWatchdog::Stats& stats = watchdog.stats();
    EXPECT_EQ(stats.dispatches, 2);
    ASSERT_EQ(stats.longest.size(), 2u);
    EXPECT_DOUBLE_EQ(stats.longest[0].ms, 80.0);
    EXPECT_DOUBLE_EQ(stats.longest[0].ended_at_ms, 100.0);
    EXPECT_EQ(stats.longest[0].event, "MetaCall");
    EXPECT_EQ(stats.longest[0].receiver, "QTimer");
    EXPECT_DOUBLE_EQ(stats.longest[1].ms, 30.0) << "the outer dispatch's own work: 20 + 10 ms";
    EXPECT_DOUBLE_EQ(stats.longest[1].ended_at_ms, 110.0);
    EXPECT_EQ(stats.longest[1].event, "Timer");
    EXPECT_EQ(stats.longest[1].receiver, "QObject");
    // Waiting is not work: the time the clock moved is in neither dispatch's CPU time.
    EXPECT_LT(stats.longest[0].cpu_ms, 10.0);
    EXPECT_LT(stats.longest[1].cpu_ms, 10.0);
    EXPECT_EQ(stats.over_50_ms, 1);
    EXPECT_EQ(stats.over_100_ms, 0);
    EXPECT_DOUBLE_EQ(stats.longest_ms, 80.0);
    EXPECT_EQ(stats.longest_busy_ms, 0.0) << "never busy";
}

TEST(EventWatchdog, Counts_dispatches_over_50_ms_and_from_100_ms)
{
    ManualClock clock;
    EventWatchdog watchdog{[] { return true; }, clock.reader()};
    QObject receiver;
    const QEvent event{QEvent::Timer};
    for (const std::chrono::milliseconds length : {50ms, 51ms, 99ms, 100ms}) {
        watchdog.enter(&receiver, &event);
        clock.advance(length);
        watchdog.leave();
    }
    EXPECT_EQ(watchdog.stats().over_50_ms, 3) << "51, 99 and 100 ms";
    EXPECT_EQ(watchdog.stats().over_100_ms, 1) << "100 ms: R-03 allows less than 100 ms";
    EXPECT_DOUBLE_EQ(watchdog.stats().longest_busy_ms, 100.0);
}

TEST(EventWatchdog, A_dispatch_s_CPU_time_is_its_own_work_without_the_nested_one)
{
    EventWatchdog watchdog{[] { return true; }};  // the default clock: real time
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
    ManualClock clock;
    bool busy = true;
    EventWatchdog watchdog{[&busy] { return busy; }, clock.reader()};
    QObject receiver;
    const QEvent event{QEvent::Timer};

    watchdog.enter(&receiver, &event);
    clock.advance(10ms);
    watchdog.exclude_begin();
    clock.advance(120ms);  // a screenshot: waiting ...
    spin_cpu(40.0);        // ... and work
    watchdog.exclude_end();
    watchdog.leave();
    EXPECT_DOUBLE_EQ(watchdog.stats().longest_ms, 10.0) << "the screenshot is not the dispatch's work";
    EXPECT_LT(watchdog.stats().longest_busy_cpu_ms, 20.0) << "nor is its CPU time";
    EXPECT_DOUBLE_EQ(watchdog.stats().longest_busy_ms, 10.0);
    EXPECT_TRUE(watchdog.stats().longest.front().busy);

    busy = false;
    watchdog.enter(&receiver, &event);
    clock.advance(70ms);
    watchdog.leave();
    EXPECT_DOUBLE_EQ(watchdog.stats().longest_ms, 70.0);
    EXPECT_DOUBLE_EQ(watchdog.stats().longest_busy_ms, 10.0) << "idle work is not busy work";
    EXPECT_DOUBLE_EQ(watchdog.stats().longest.front().ms, 70.0);
    EXPECT_FALSE(watchdog.stats().longest.front().busy);

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
