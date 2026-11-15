// EventWatchdog (05 §7.3 R-03, "a watchdog on the UI loop"): the exclusive time of
// each UI-thread dispatch, nested dispatches and harness work left out, the longest
// ones with event type and receiver, overall and while busy; and its hook in
// Application::notify.

#include "application.hpp"
#include "diagnostics_test_support.hpp"
#include "event_watchdog.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QTimer>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;
using wizz::app::EventWatchdog;

TEST(EventWatchdog, Keeps_the_exclusive_time_of_nested_dispatches)
{
    EventWatchdog watchdog{[] { return false; }};
    QObject outer_receiver;
    QTimer inner_receiver;
    const QEvent outer_event{QEvent::Timer};
    const QEvent inner_event{QEvent::MetaCall};

    watchdog.enter(&outer_receiver, &outer_event);
    std::this_thread::sleep_for(20ms);
    watchdog.enter(&inner_receiver, &inner_event);  // e.g. processEvents inside a handler
    std::this_thread::sleep_for(80ms);
    watchdog.leave();
    std::this_thread::sleep_for(10ms);
    watchdog.leave();

    const EventWatchdog::Stats& stats = watchdog.stats();
    EXPECT_EQ(stats.dispatches, 2);
    ASSERT_EQ(stats.longest.size(), 2u);
    EXPECT_GE(stats.longest[0].ms, 75.0);
    EXPECT_EQ(stats.longest[0].event, "MetaCall");
    EXPECT_EQ(stats.longest[0].receiver, "QTimer");
    EXPECT_LT(stats.longest[1].ms, 75.0) << "the outer dispatch's own work: about 30 ms";
    EXPECT_EQ(stats.longest[1].event, "Timer");
    EXPECT_EQ(stats.longest[1].receiver, "QObject");
    EXPECT_EQ(stats.over_50_ms, 1);
    EXPECT_EQ(stats.over_100_ms, 0);
    EXPECT_DOUBLE_EQ(stats.longest_ms, stats.longest[0].ms);
    EXPECT_EQ(stats.longest_busy_ms, 0.0) << "never busy";
}

TEST(EventWatchdog, Harness_work_and_busy_state)
{
    bool busy = true;
    EventWatchdog watchdog{[&busy] { return busy; }};
    QObject receiver;
    const QEvent event{QEvent::Timer};

    watchdog.enter(&receiver, &event);
    std::this_thread::sleep_for(10ms);
    watchdog.exclude_begin();
    std::this_thread::sleep_for(120ms);  // a screenshot
    watchdog.exclude_end();
    watchdog.leave();
    EXPECT_LT(watchdog.stats().longest_ms, 60.0);
    EXPECT_GT(watchdog.stats().longest_busy_ms, 0.0);
    EXPECT_TRUE(watchdog.stats().longest.front().busy);

    busy = false;
    watchdog.enter(&receiver, &event);
    std::this_thread::sleep_for(70ms);
    watchdog.leave();
    EXPECT_GE(watchdog.stats().longest_ms, 65.0);
    EXPECT_LT(watchdog.stats().longest_busy_ms, 60.0) << "idle work is not busy work";
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
