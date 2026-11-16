#pragma once
// EventWatchdog: the UI-loop watchdog of 05 §7.3 R-03 ("no UI-thread work may
// run longer than 100 ms; use a watchdog on the UI loop").
//
// Application::notify reports every event it dispatches on the UI thread
// (enter() before, leave() after). The watchdog keeps the exclusive time of each
// dispatch — its own work, without the events dispatched inside it by a nested
// event loop (processEvents, a modal box) and without harness work marked with
// exclude_begin() / exclude_end() (the --autotest screenshots) — so a long value
// is one uninterrupted stretch of UI-thread work. It records how many dispatches
// ran over 50 / 100 ms and the longest ones with the event type and the
// receiver's class, separately for the time the busy panel was busy.
//
// Like TimerGapProbe, each dispatch also gets its exclusive UI-thread CPU time
// and, when it ran 50 ms or more, whether other processes held the CPUs during
// it (CpuMeter, DECISIONS D-50), so that the R-03 check can hold on a busy
// machine: `longest_busy_cpu_ms` is the UI thread's own work,
// `longest_busy_own_ms` the longest dispatch that other processes did not hold
// the CPUs during. The CPU counters cost about 50 µs, so they are read at most
// every 10 ms and at the end of a long dispatch: its window starts up to 10 ms
// early.
//
// Wall time (every `ms`, and the 10 ms between CPU readings) comes from the
// clock given at construction, by default a QElapsedTimer; tests pass one they
// move by hand. CPU time and who held the CPUs always come from CpuMeter.
//
// Test-only diagnostics of the --autotest mode (DECISIONS D-30); UI thread only.

#include "cpu_meter.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QEvent;
class QObject;

namespace wizz::app {

class EventWatchdog {
public:
    /// One long dispatch.
    struct Dispatch {
        double ms = 0;                ///< exclusive time
        double cpu_ms = 0;            ///< exclusive UI-thread CPU time
        bool held_by_others = false;  ///< other processes held the CPUs during it
        double ended_at_ms = 0;       ///< since the watchdog started
        std::string event;            ///< QEvent::Type name, e.g. "MetaCall", "Timer", "UpdateRequest"
        std::string receiver;         ///< the receiver's class name
        bool busy = false;            ///< the busy panel was busy when it ended
    };
    static constexpr std::size_t kKeptDispatches = 5;

    struct Stats {
        std::int64_t dispatches = 0;
        int over_50_ms = 0;
        int over_100_ms = 0;
        double longest_ms = 0;
        double longest_busy_ms = 0;       ///< the longest while busy (R-03)
        double longest_busy_cpu_ms = 0;   ///< the most exclusive UI-thread CPU time of one dispatch while busy
        double longest_busy_own_ms = 0;   ///< the longest while busy that other processes did not hold the CPUs during
        std::vector<Dispatch> longest;    ///< the kKeptDispatches longest, longest first
    };

    /// Monotonic nanoseconds since an arbitrary start.
    using Clock = std::function<std::int64_t()>;

    /// `is_busy` tells whether the progress UI is busy (BusyPanel::is_busy). Time comes from
    /// `clock`, by default a QElapsedTimer started now; tests pass one they move by hand.
    explicit EventWatchdog(std::function<bool()> is_busy, Clock clock = {});

    /// A dispatch of `event` to `receiver` starts (read before dispatch: the receiver may
    /// be deleted by it).
    void enter(const QObject* receiver, const QEvent* event);
    /// The innermost dispatch ended.
    void leave();

    /// Harness work starts / ends inside the current dispatch: not counted.
    void exclude_begin();
    void exclude_end();

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    /// How often the CPU counters are read at most (at the start of a dispatch).
    static constexpr std::int64_t kSampleEveryNs = 10'000'000;

    struct Frame {
        std::int64_t start_ns = 0;
        std::int64_t nested_ns = 0;      ///< time of dispatches nested inside this one
        std::uint64_t start_cycles = 0;  ///< the UI thread's cycles at the start
        std::uint64_t nested_cycles = 0; ///< UI-thread cycles of nested dispatches and harness work
        CpuMeter::Sample window_start;   ///< the CPU counters at most kSampleEveryNs before the start
        int event_type = 0;
        const char* receiver = "";
    };

    [[nodiscard]] std::int64_t now_ns() const { return clock_(); }
    void record(const Frame& frame, const Dispatch& measured, std::int64_t now);

    std::function<bool()> is_busy_;
    Clock clock_;
    std::vector<Frame> stack_;
    CpuMeter::Sample sampled_;            ///< the latest CPU counters
    std::int64_t sampled_ns_ = -1;        ///< when they were read
    std::int64_t excluding_since_ns_ = -1;
    std::uint64_t excluding_since_cycles_ = 0;
    Stats stats_;
};

} // namespace wizz::app
