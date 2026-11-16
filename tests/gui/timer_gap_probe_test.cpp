// TimerGapProbe (05 §5, §7.3 R-03 / R-04): the gaps of the BusyPanel's 40 ms timer
// while busy — start to first tick, tick to tick, last tick to stop — with the idle
// time between busy periods and the harness's own work left out; each gap with the
// UI thread's own CPU time and who held the CPUs (D-50).
//
// Sleeps stand in for a blocked UI thread and spin_cpu() for UI work. A busy machine
// may stretch any of them and delay any tick (a loaded CI runner by hundreds of ms), so
// the checks never assume how long a stretch took: they compare the probe with times
// the test measures itself, right around the calls that start and stop a run or an
// exclusion (the probe's own readings lie inside them), or read lower bounds only.

#include "busy_panel.hpp"
#include "diagnostics_test_support.hpp"
#include "timer_gap_probe.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/locale.hpp"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QTimer>
#include <QtGlobal>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;
using wizz::app::BusyPanel;
using wizz::app::TimerGapProbe;
using wizz::test::pump_for;
using wizz::test::spin_cpu;

/// Milliseconds between two readings of the test's clock.
[[nodiscard]] double span_ms(qint64 from_ns, qint64 to_ns)
{
    return static_cast<double>(to_ns - from_ns) / 1e6;
}

class TimerGapProbeTest : public ::testing::Test {
protected:
    TimerGapProbeTest() { stopwatch_.start(); }

    /// Runs the event loop until the probe has seen `count` more ticks (at most kWaitCap).
    /// It waits for the timer's own ticks: pump_until's 5 ms poll would add dispatches of
    /// the test's own to every gap and its CPU time.
    [[nodiscard]] bool wait_ticks(int count)
    {
        const int target = probe_.stats().ticks + count;
        QEventLoop loop;
        // Connected after the probe, so it runs after the probe counted the tick.
        QObject::connect(panel_.animation_timer(), &QTimer::timeout, &loop, [&] {
            if (probe_.stats().ticks >= target) {
                loop.quit();
            }
        });
        QTimer::singleShot(wizz::test::kWaitCap, &loop, &QEventLoop::quit);
        loop.exec();
        return probe_.stats().ticks >= target;
    }

    /// The test's own clock: a QElapsedTimer, like the probe's.
    [[nodiscard]] qint64 now_ns() const { return stopwatch_.nsecsElapsed(); }

    wizz::FixedClock clock_;
    BusyPanel panel_{clock_, wizz::Locale::en_us()};
    TimerGapProbe probe_{panel_};
    QElapsedTimer stopwatch_;
};

TEST_F(TimerGapProbeTest, Counts_runs_ticks_and_the_gaps_of_a_busy_period)
{
    EXPECT_EQ(probe_.stats().runs, 0);
    const qint64 started = now_ns();
    panel_.begin("Loading Wizz Air route map…");
    ASSERT_TRUE(wait_ticks(4));
    panel_.end("Ready.");
    const qint64 ended = now_ns();
    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_EQ(stats.runs, 1);
    EXPECT_GE(stats.ticks, 4);
    EXPECT_EQ(stats.gaps, stats.ticks + 1) << "start to first tick, tick to tick, last tick to stop";
    EXPECT_GE(stats.longest_gap_ms, 30.0);
    EXPECT_GT(stats.mean_gap_ms(), 0.0);
    EXPECT_LE(stats.total_gap_ms, span_ms(started, ended)) << "the gaps tile the run";
    EXPECT_FALSE(stats.longest_gaps.empty());
    EXPECT_LE(stats.longest_gaps.size(), TimerGapProbe::kKeptGaps);
    for (std::size_t i = 1; i < stats.longest_gaps.size(); ++i) {
        EXPECT_GE(stats.longest_gaps[i - 1].ms, stats.longest_gaps[i].ms) << "longest first";
    }
    for (const TimerGapProbe::Gap& gap : stats.longest_gaps) {
        EXPECT_LE(gap.cpu_ms, gap.ms + 0.5) << "a thread's CPU time never exceeds the time that passed";
    }
}

TEST_F(TimerGapProbeTest, A_blocked_UI_thread_shows_as_a_long_gap)
{
    panel_.begin("Fetching fares…");
    ASSERT_TRUE(wait_ticks(2));
    QTimer::singleShot(0, [] { std::this_thread::sleep_for(150ms); });  // the UI thread blocked
    ASSERT_TRUE(wait_ticks(2));
    panel_.end("Done.");
    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_GE(stats.longest_gap_ms, 140.0);
    EXPECT_GE(stats.gaps_over_100_ms, 1);
    EXPECT_GE(stats.gaps_over_50_ms, 1);
    EXPECT_EQ(stats.longest_gap_status, "Fetching fares…");
    EXPECT_LT(stats.longest_gap_cpu_ms, 50.0) << "waiting is not UI work";
    // Blocked while CPUs were free is the app's own gap; while other processes held them, not.
    const TimerGapProbe::Gap& longest = stats.longest_gaps.front();
    if (longest.held_by_others) {
        EXPECT_GE(stats.held_gaps_over_100_ms, 1);
    } else {
        EXPECT_DOUBLE_EQ(stats.longest_own_gap_ms, longest.ms);
    }
}

TEST_F(TimerGapProbeTest, UI_work_shows_as_CPU_time_in_its_gap)
{
    panel_.begin("Fetching fares…");
    ASSERT_TRUE(wait_ticks(2));
    QTimer::singleShot(0, [] { spin_cpu(120.0); });  // a long piece of UI work
    ASSERT_TRUE(wait_ticks(2));
    panel_.end("Done.");
    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_GE(stats.longest_gap_cpu_ms, 119.0);
    EXPECT_GE(stats.longest_gap_ms, stats.longest_gap_cpu_ms - 0.5);
    EXPECT_GE(stats.gaps_over_100_ms, 1);
}

TEST_F(TimerGapProbeTest, Work_right_before_End_counts_although_no_tick_followed)
{
    panel_.begin("Fetching fares…");
    ASSERT_TRUE(wait_ticks(1));
    spin_cpu(120.0);  // e.g. the final rebuild, then End() stops the timer
    panel_.end("Outbound Done: 1 fares from 1 of 1 requests in 1 s.");
    EXPECT_GE(probe_.stats().longest_gap_ms, 115.0);
    EXPECT_GE(probe_.stats().longest_gap_cpu_ms, 119.0);
    EXPECT_EQ(probe_.stats().longest_gap_status, "Outbound Done: 1 fares from 1 of 1 requests in 1 s.");
}

TEST_F(TimerGapProbeTest, Idle_time_between_busy_periods_is_not_a_gap)
{
    const qint64 start_one = now_ns();
    panel_.begin("One");
    ASSERT_TRUE(wait_ticks(1));
    panel_.end("ok");
    const qint64 end_one = now_ns();
    pump_for(300ms);  // idle: what a probe that measured across runs would count
    const qint64 start_two = now_ns();
    panel_.begin("Two");
    ASSERT_TRUE(wait_ticks(2));
    panel_.end("ok");
    const qint64 end_two = now_ns();

    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_EQ(stats.runs, 2);
    EXPECT_EQ(stats.gaps, stats.ticks + stats.runs) << "per run: start to first tick, tick to tick, last tick to stop";
    // The gaps of a run add up to the run, which lies inside the test's own measurement of
    // it; counting the idle time would add at least its 300 ms on top.
    const double runs_ms = span_ms(start_one, end_one) + span_ms(start_two, end_two);
    const double idle_ms = span_ms(end_one, start_two);
    ASSERT_GE(idle_ms, 300.0);
    EXPECT_LE(stats.total_gap_ms, runs_ms) << "the " << idle_ms << " ms between the runs were counted";
}

TEST_F(TimerGapProbeTest, Harness_work_is_excluded_and_reported_apart)
{
    const qint64 started = now_ns();
    panel_.begin("Fetching fares…");
    ASSERT_TRUE(wait_ticks(1));
    probe_.exclude_begin();
    const qint64 excluded_from = now_ns();
    std::this_thread::sleep_for(150ms);  // a screenshot: waiting ...
    spin_cpu(60.0);                      // ... and work
    const qint64 excluded_to = now_ns();
    probe_.exclude_end();
    ASSERT_TRUE(wait_ticks(1));
    panel_.end("ok");
    const qint64 ended = now_ns();

    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_EQ(stats.exclusions, 1);
    const double inside_ms = span_ms(excluded_from, excluded_to);  // within the exclusion
    ASSERT_GE(inside_ms, 200.0) << "150 ms asleep and 60 ms of CPU time";
    EXPECT_GE(stats.excluded_ms, inside_ms);
    EXPECT_GE(stats.longest_exclusion_ms, inside_ms);
    // Gaps and the exclusion share the run without overlapping: the screenshot is in no
    // gap, and no gap holds its CPU time.
    EXPECT_LE(stats.total_gap_ms + stats.excluded_ms, span_ms(started, ended))
        << "the excluded time was counted in a gap too";
    EXPECT_LT(stats.longest_gap_cpu_ms, 30.0);
}

TEST_F(TimerGapProbeTest, Exclusions_while_idle_change_nothing)
{
    probe_.exclude_begin();
    probe_.exclude_end();
    probe_.exclude_end();  // unbalanced: ignored
    EXPECT_EQ(probe_.stats().exclusions, 0);
    EXPECT_EQ(probe_.stats().gaps, 0);
}

} // namespace
