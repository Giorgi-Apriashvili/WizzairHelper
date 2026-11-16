#pragma once
// CpuMeter: what a thread and the CPUs did during an interval, so the timing
// checks of R-03 / R-04 and 05 §5 ("no UI-thread work longer than 100 ms", "the
// 40 ms timer's longest gap below 100 ms") mean the same on a busy machine as on
// an idle one (DECISIONS D-50). From the Windows cycle counters:
//
//   * the calling thread's own CPU time (QueryThreadCycleTime): the work the UI
//     thread did itself. Time in which the OS ran other threads does not count,
//     so it does not grow when other processes compete for the CPUs;
//   * how the CPUs this process may run on were used, as average CPUs: idle
//     (QueryIdleProcessorCycleTime), this process (QueryProcessCycleTime) and
//     the rest, i.e. other processes.
//
// A wall-clock interval is the app's own unless other processes held the CPUs
// during it: fewer than half a CPU stayed idle while other processes used at
// least half a CPU, on average (Interval::held_by_others). The app's own threads
// keeping every CPU busy is still the app's doing.
//
// Cycle counts are time stamp counter ticks; the TSC rate is measured once
// against QueryPerformanceCounter (about 30 ms, on first use). Test-only
// diagnostics of the --autotest mode (D-30) and the GUI tests.

#include <cstdint>

namespace wizz::app {

class CpuMeter {
public:
    /// The counters at one instant, read on the thread whose CPU time is measured.
    struct Sample {
        std::uint64_t tsc = 0;      ///< time stamp counter (wall time)
        std::uint64_t thread = 0;   ///< the sampling thread's cycles
        std::uint64_t idle = 0;     ///< idle cycles, summed over the CPUs of the process
        std::uint64_t process = 0;  ///< cycles of every thread of this process
    };

    /// Between two samples of the same thread.
    struct Interval {
        double wall_ms = 0;
        double thread_ms = 0;   ///< the thread's own CPU time
        double idle_cpus = 0;   ///< average idle CPUs (of the process's CPUs)
        double own_cpus = 0;    ///< average CPUs this process used
        double other_cpus = 0;  ///< average CPUs other processes used

        /// Other processes held the CPUs: fewer than half a CPU stayed idle while they used
        /// at least half a CPU, on average over the interval.
        [[nodiscard]] bool held_by_others() const noexcept
        {
            return idle_cpus < kHeldIdleCpus && other_cpus >= kHeldOtherCpus;
        }
    };

    static constexpr double kHeldIdleCpus = 0.5;
    static constexpr double kHeldOtherCpus = 0.5;

    /// The counters now, for the calling thread. About 50 µs (the process and idle
    /// counters visit every thread and CPU); thread_cycles() alone is far cheaper.
    [[nodiscard]] static Sample sample();
    /// The calling thread's cycles now (Sample::thread; well under a microsecond).
    [[nodiscard]] static std::uint64_t thread_cycles() noexcept;
    [[nodiscard]] static Interval between(const Sample& from, const Sample& to);
    /// `start` moved on by the interval [skip_from, skip_to] (all counters): an interval
    /// measured from the result leaves that stretch out.
    [[nodiscard]] static Sample skip(Sample start, const Sample& skip_from, const Sample& skip_to) noexcept;
    /// Cycles of the time stamp counter per millisecond (measured once).
    [[nodiscard]] static double cycles_per_ms();
    /// The number of CPUs this process may run on.
    [[nodiscard]] static int cpus();
};

} // namespace wizz::app
