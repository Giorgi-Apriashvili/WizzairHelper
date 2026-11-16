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
// Test-only diagnostics of the --autotest mode (DECISIONS D-30); UI thread only.

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
        double ms = 0;            ///< exclusive time
        double ended_at_ms = 0;   ///< since the watchdog started
        std::string event;        ///< QEvent::Type name, e.g. "MetaCall", "Timer", "UpdateRequest"
        std::string receiver;     ///< the receiver's class name
        bool busy = false;        ///< the busy panel was busy when it ended
    };
    static constexpr std::size_t kKeptDispatches = 5;

    struct Stats {
        std::int64_t dispatches = 0;
        int over_50_ms = 0;
        int over_100_ms = 0;
        double longest_ms = 0;
        double longest_busy_ms = 0;       ///< the longest while busy (R-03)
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
    struct Frame {
        std::int64_t start_ns = 0;
        std::int64_t nested_ns = 0;   ///< time of dispatches nested inside this one
        int event_type = 0;
        const char* receiver = "";
    };

    [[nodiscard]] std::int64_t now_ns() const { return clock_(); }
    void record(const Frame& frame, double ms, std::int64_t now);

    std::function<bool()> is_busy_;
    Clock clock_;
    std::vector<Frame> stack_;
    std::int64_t excluding_since_ns_ = -1;
    Stats stats_;
};

} // namespace wizz::app
