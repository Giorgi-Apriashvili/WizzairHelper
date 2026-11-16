// TimerGapProbe (05 §5, §7.3 R-03 / R-04): the gaps of the BusyPanel's 40 ms timer
// while busy — start to first tick, tick to tick, last tick to stop — with the idle
// time between busy periods and the harness's own work left out.

#include "busy_panel.hpp"
#include "diagnostics_test_support.hpp"
#include "timer_gap_probe.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/locale.hpp"

#include <QElapsedTimer>
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
using wizz::test::pump_until;

class TimerGapProbeTest : public ::testing::Test {
protected:
    /// Runs the event loop until the probe has seen `count` more ticks.
    [[nodiscard]] bool wait_ticks(int count)
    {
        const int target = probe_.stats().ticks + count;
        return pump_until([&] { return probe_.stats().ticks >= target; });
    }

    wizz::FixedClock clock_;
    BusyPanel panel_{clock_, wizz::Locale::en_us()};
    TimerGapProbe probe_{panel_};
};

TEST_F(TimerGapProbeTest, Counts_runs_ticks_and_the_gaps_of_a_busy_period)
{
    EXPECT_EQ(probe_.stats().runs, 0);
    panel_.begin("Loading Wizz Air route map…");
    ASSERT_TRUE(wait_ticks(4));
    panel_.end("Ready.");
    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_EQ(stats.runs, 1);
    EXPECT_GE(stats.ticks, 4);
    EXPECT_EQ(stats.gaps, stats.ticks + 1) << "start to first tick, tick to tick, last tick to stop";
    EXPECT_GE(stats.longest_gap_ms, 30.0);
    EXPECT_GT(stats.mean_gap_ms(), 0.0);
    EXPECT_FALSE(stats.longest_gaps.empty());
    EXPECT_LE(stats.longest_gaps.size(), TimerGapProbe::kKeptGaps);
    for (std::size_t i = 1; i < stats.longest_gaps.size(); ++i) {
        EXPECT_GE(stats.longest_gaps[i - 1].ms, stats.longest_gaps[i].ms) << "longest first";
    }
}

TEST_F(TimerGapProbeTest, A_blocked_UI_thread_shows_as_a_long_gap)
{
    panel_.begin("Fetching fares…");
    ASSERT_TRUE(wait_ticks(2));
    QTimer::singleShot(0, [] { std::this_thread::sleep_for(150ms); });  // a long piece of UI work
    ASSERT_TRUE(wait_ticks(2));
    panel_.end("Done.");
    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_GE(stats.longest_gap_ms, 140.0);
    EXPECT_GE(stats.gaps_over_100_ms, 1);
    EXPECT_GE(stats.gaps_over_50_ms, 1);
    EXPECT_EQ(stats.longest_gap_status, "Fetching fares…");
}

TEST_F(TimerGapProbeTest, Work_right_before_End_counts_although_no_tick_followed)
{
    panel_.begin("Fetching fares…");
    ASSERT_TRUE(wait_ticks(1));
    std::this_thread::sleep_for(120ms);  // e.g. the final rebuild, then End() stops the timer
    panel_.end("Outbound Done: 1 fares from 1 of 1 requests in 1 s.");
    EXPECT_GE(probe_.stats().longest_gap_ms, 115.0);
    EXPECT_EQ(probe_.stats().longest_gap_status, "Outbound Done: 1 fares from 1 of 1 requests in 1 s.");
}

// The two tests below compare the probe with times the test measures itself, right
// around the calls that start and stop a run: the probe's own readings lie inside them.
// That holds however slow or busy the machine is, where a fixed bound on a gap would not
// (a loaded CI runner can delay a 40 ms tick by hundreds of ms).

TEST_F(TimerGapProbeTest, Idle_time_between_busy_periods_is_not_a_gap)
{
    QElapsedTimer clock;
    clock.start();
    const qint64 start_one = clock.nsecsElapsed();
    panel_.begin("One");
    ASSERT_TRUE(wait_ticks(1));
    panel_.end("ok");
    const qint64 end_one = clock.nsecsElapsed();
    pump_for(300ms);  // idle: what a probe that measured across runs would count
    const qint64 start_two = clock.nsecsElapsed();
    panel_.begin("Two");
    ASSERT_TRUE(wait_ticks(2));
    panel_.end("ok");
    const qint64 end_two = clock.nsecsElapsed();

    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_EQ(stats.runs, 2);
    EXPECT_EQ(stats.gaps, stats.ticks + stats.runs) << "per run: start to first tick, tick to tick, last tick to stop";
    // The gaps of a run add up to the run, which lies inside the test's own measurement of
    // it; counting the idle time would add at least its 300 ms on top.
    const double runs_ms = static_cast<double>((end_one - start_one) + (end_two - start_two)) / 1e6;
    const double idle_ms = static_cast<double>(start_two - end_one) / 1e6;
    ASSERT_GE(idle_ms, 300.0);
    EXPECT_LE(stats.total_gap_ms, runs_ms) << "the " << idle_ms << " ms between the runs were counted";
}

TEST_F(TimerGapProbeTest, Harness_work_is_excluded_and_reported_apart)
{
    QElapsedTimer clock;
    clock.start();
    const qint64 started = clock.nsecsElapsed();
    panel_.begin("Fetching fares…");
    ASSERT_TRUE(wait_ticks(1));
    probe_.exclude_begin();
    const qint64 excluded_from = clock.nsecsElapsed();
    std::this_thread::sleep_for(150ms);  // a screenshot
    const qint64 excluded_to = clock.nsecsElapsed();
    probe_.exclude_end();
    ASSERT_TRUE(wait_ticks(1));
    panel_.end("ok");
    const qint64 ended = clock.nsecsElapsed();

    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_EQ(stats.exclusions, 1);
    const double inside_ms = static_cast<double>(excluded_to - excluded_from) / 1e6;  // within the exclusion
    EXPECT_GE(stats.excluded_ms, inside_ms);
    EXPECT_GE(stats.longest_exclusion_ms, inside_ms);
    // Gaps and the exclusion share the run without overlapping: the screenshot is in no gap.
    const double run_ms = static_cast<double>(ended - started) / 1e6;
    EXPECT_LE(stats.total_gap_ms + stats.excluded_ms, run_ms) << "the excluded time was counted in a gap too";
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
