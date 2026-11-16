// CpuMeter (D-50): the UI thread's own CPU time and who held the CPUs during an
// interval, which the R-03 / 05 §5 timing checks read so that they hold on a busy
// machine. The rule and the arithmetic on made-up counters; the counters themselves
// on real work and real waiting (lower bounds and brackets only: the machine may be
// busy while this runs).

#include "cpu_meter.hpp"
#include "diagnostics_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using namespace std::chrono_literals;
using wizz::app::CpuMeter;
using wizz::test::spin_cpu;

[[nodiscard]] CpuMeter::Interval interval(double idle_cpus, double other_cpus)
{
    return CpuMeter::Interval{.wall_ms = 120, .thread_ms = 5, .idle_cpus = idle_cpus, .own_cpus = 1,
                              .other_cpus = other_cpus};
}

TEST(CpuMeter, Other_processes_held_the_CPUs_when_little_stayed_idle_and_they_used_it)
{
    EXPECT_TRUE(interval(0.0, 1.4).held_by_others()) << "2 CPUs beside 4 busy loops";
    EXPECT_TRUE(interval(0.49, 0.5).held_by_others());
    EXPECT_FALSE(interval(0.5, 0.5).held_by_others()) << "half a CPU stayed free";
    EXPECT_FALSE(interval(2.0, 1.0).held_by_others()) << "free CPUs: a waiting UI thread was the app's doing";
    EXPECT_FALSE(interval(0.0, 0.2).held_by_others()) << "the app's own threads kept the CPUs busy";
}

TEST(CpuMeter, Intervals_from_the_counters)
{
    const double per_ms = CpuMeter::cycles_per_ms();
    ASSERT_GT(per_ms, 1000.0) << "a time stamp counter of at least 1 MHz";
    const auto cycles = [per_ms](double ms) { return static_cast<std::uint64_t>(ms * per_ms); };
    const CpuMeter::Sample from{.tsc = cycles(1000), .thread = cycles(10), .idle = cycles(500), .process = cycles(40)};
    const CpuMeter::Sample to{.tsc = cycles(1100), .thread = cycles(40), .idle = cycles(500 + 50),
                              .process = cycles(40 + 120)};
    const CpuMeter::Interval measured = CpuMeter::between(from, to);
    EXPECT_NEAR(measured.wall_ms, 100.0, 0.01);
    EXPECT_NEAR(measured.thread_ms, 30.0, 0.01);
    EXPECT_NEAR(measured.idle_cpus, 0.5, 0.001);
    EXPECT_NEAR(measured.own_cpus, 1.2, 0.001);
    EXPECT_NEAR(measured.other_cpus, std::max(0.0, CpuMeter::cpus() - 0.5 - 1.2), 0.001);

    // Skipping [skip_from, skip_to] moves every counter of the start on by that stretch.
    const CpuMeter::Sample skip_from{.tsc = 1100, .thread = 300, .idle = 7000, .process = 900};
    const CpuMeter::Sample skip_to{.tsc = 1160, .thread = 320, .idle = 7090, .process = 950};
    const CpuMeter::Sample start{.tsc = 1000, .thread = 200, .idle = 5000, .process = 800};
    const CpuMeter::Sample moved = CpuMeter::skip(start, skip_from, skip_to);
    EXPECT_EQ(moved.tsc, 1060u);
    EXPECT_EQ(moved.thread, 220u);
    EXPECT_EQ(moved.idle, 5090u);
    EXPECT_EQ(moved.process, 850u);
}

TEST(CpuMeter, Work_counts_as_the_thread_s_CPU_time_and_waiting_does_not)
{
    EXPECT_GE(CpuMeter::cpus(), 1);
    EXPECT_LE(CpuMeter::cpus(), 64);

    const CpuMeter::Sample before_work = CpuMeter::sample();
    spin_cpu(50.0);
    const CpuMeter::Interval work = CpuMeter::between(before_work, CpuMeter::sample());
    EXPECT_GE(work.thread_ms, 50.0);
    EXPECT_LT(work.thread_ms, 55.0);
    EXPECT_GE(work.wall_ms, work.thread_ms - 0.5);
    EXPECT_GT(work.own_cpus, 0.0);

    const CpuMeter::Sample before_wait = CpuMeter::sample();
    std::this_thread::sleep_for(50ms);
    const CpuMeter::Interval wait = CpuMeter::between(before_wait, CpuMeter::sample());
    EXPECT_GE(wait.wall_ms, 45.0);
    EXPECT_LT(wait.thread_ms, 5.0);
}

} // namespace
