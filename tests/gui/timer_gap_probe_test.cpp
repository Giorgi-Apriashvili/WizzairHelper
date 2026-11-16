// TimerGapProbe (05 §5, §7.3 R-03 / R-04): the gaps of the BusyPanel's 40 ms timer
// while busy — start to first tick, tick to tick, last tick to stop — with the idle
// time between busy periods and the harness's own work left out; each gap with the
// UI thread's own CPU time and who held the CPUs (D-50).
//
// Sleeps stand in for a blocked UI thread and spin_cpu() for UI work. A busy machine
// may stretch any of them and delay any tick, so the checks never assume how long a
// stretch took: they bracket it with the test's own clock or read lower bounds only.

#include "busy_panel.hpp"
#include "diagnostics_test_support.hpp"
#include "timer_gap_probe.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/locale.hpp"

#include <QEventLoop>
#include <QTimer>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using wizz::app::BusyPanel;
using wizz::app::TimerGapProbe;
using wizz::test::ms_between;
using wizz::test::pump_for;
using wizz::test::spin_cpu;

class TimerGapProbeTest : public ::testing::Test {
protected:
    /// Runs the event loop until the probe has seen `count` more ticks. It waits in the
    /// loop (pump_until polls, which would put the test's own spinning into every gap's
    /// CPU time).
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
        QTimer::singleShot(5s, &loop, &QEventLoop::quit);
        loop.exec();
        return probe_.stats().ticks >= target;
    }

    wizz::FixedClock clock_;
    BusyPanel panel_{clock_, wizz::Locale::en_us()};
    TimerGapProbe probe_{panel_};
};

TEST_F(TimerGapProbeTest, Counts_runs_ticks_and_the_gaps_of_a_busy_period)
{
    EXPECT_EQ(probe_.stats().runs, 0);
    const Clock::time_point begin = Clock::now();
    panel_.begin("Loading Wizz Air route map…");
    ASSERT_TRUE(wait_ticks(4));
    panel_.end("Ready.");
    const Clock::time_point end = Clock::now();
    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_EQ(stats.runs, 1);
    EXPECT_GE(stats.ticks, 4);
    EXPECT_EQ(stats.gaps, stats.ticks + 1) << "start to first tick, tick to tick, last tick to stop";
    EXPECT_GE(stats.longest_gap_ms, 30.0);
    EXPECT_GT(stats.mean_gap_ms(), 0.0);
    EXPECT_LE(stats.total_gap_ms, ms_between(begin, end) + 0.5) << "the gaps tile the run";
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
    const Clock::time_point first_begin = Clock::now();
    panel_.begin("One");
    ASSERT_TRUE(wait_ticks(1));
    panel_.end("ok");
    const Clock::time_point first_end = Clock::now();
    pump_for(300ms);
    const Clock::time_point second_begin = Clock::now();
    panel_.begin("Two");
    ASSERT_TRUE(wait_ticks(2));
    panel_.end("ok");
    const Clock::time_point second_end = Clock::now();
    EXPECT_EQ(probe_.stats().runs, 2);
    // The gaps tile the two runs only: the 300 ms between them are in no gap.
    EXPECT_LE(probe_.stats().total_gap_ms,
              ms_between(first_begin, first_end) + ms_between(second_begin, second_end) + 0.5);
}

TEST_F(TimerGapProbeTest, Harness_work_is_excluded_and_reported_apart)
{
    const Clock::time_point begin = Clock::now();
    panel_.begin("Fetching fares…");
    ASSERT_TRUE(wait_ticks(1));
    probe_.exclude_begin();
    std::this_thread::sleep_for(150ms);  // a screenshot: waiting ...
    spin_cpu(60.0);                      // ... and work
    probe_.exclude_end();
    ASSERT_TRUE(wait_ticks(1));
    panel_.end("ok");
    const Clock::time_point end = Clock::now();
    const TimerGapProbe::Stats& stats = probe_.stats();
    EXPECT_EQ(stats.exclusions, 1);
    EXPECT_GE(stats.excluded_ms, 200.0);
    EXPECT_GE(stats.longest_exclusion_ms, 200.0);
    // The gaps tile the run without the excluded stretch, and none holds its CPU time.
    EXPECT_LE(stats.total_gap_ms + stats.excluded_ms, ms_between(begin, end) + 0.5);
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
