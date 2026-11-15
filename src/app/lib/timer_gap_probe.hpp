#pragma once
// TimerGapProbe: measures the BusyPanel's 40 ms animation timer (05 §5, §7.3
// R-03 / R-04) — "under a heavy cached replay, measure the 40 ms progress
// timer: its longest gap must remain below 100 ms".
//
// A gap is the real (monotonic) time in which a run of the timer delivered no
// tick: from the start of a run to its first tick, between two consecutive
// ticks, and from the last tick to the stop of the run. A run begins when the
// panel starts its timer (BusyPanel::animation_started) and ends when the panel
// stops it (animation_stopped), so idle time between busy periods never counts,
// while UI-thread work that delays a tick (a long batch append, the final
// rebuild before End()) always does — also when End() stops the timer before
// the overdue tick could be delivered. The probe's tick slot runs right after
// BusyPanel::animate.
//
// Work of the test harness itself on the UI thread (the --autotest progress
// screenshots: layout passes, PrintWindow, PNG encoding) is not the app's and is
// excluded: between exclude_begin() and exclude_end() the clock of the current
// gap stands still; the excluded time is reported separately.
//
// Test-only diagnostics of the --autotest mode (DECISIONS D-30); UI thread only.

#include <QElapsedTimer>
#include <QObject>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wizz::app {

class BusyPanel;

class TimerGapProbe final : public QObject {
    Q_OBJECT

public:
    /// A gap above this misses R-04's "repaint at least every 50 ms".
    static constexpr double kRepaintLimitMs = 50.0;
    /// 05 §5: the longest gap must remain below 100 ms.
    static constexpr double kGapLimitMs = 100.0;

    /// One long gap: its length, when it ended (ms since the probe started) and the status.
    struct Gap {
        double ms = 0;
        double ended_at_ms = 0;
        std::string status;
    };
    /// How many of the longest gaps are kept.
    static constexpr std::size_t kKeptGaps = 5;

    struct Stats {
        int runs = 0;                    ///< timer runs (busy periods) seen
        int ticks = 0;                   ///< ticks seen
        double longest_gap_ms = 0;       ///< the longest gap of any run
        double total_gap_ms = 0;         ///< sum of all gaps
        int gaps = 0;                    ///< gaps measured
        int gaps_over_50_ms = 0;         ///< gaps above kRepaintLimitMs
        int gaps_over_100_ms = 0;        ///< gaps at or above kGapLimitMs
        std::string longest_gap_status;  ///< the busy panel's status line when the longest gap ended
        int exclusions = 0;              ///< harness work excluded while the timer ran
        double excluded_ms = 0;          ///< its total time
        double longest_exclusion_ms = 0;
        std::vector<Gap> longest_gaps;   ///< the kKeptGaps longest gaps, longest first

        [[nodiscard]] double mean_gap_ms() const noexcept { return gaps > 0 ? total_gap_ms / gaps : 0.0; }
    };

    /// Watches `busy`'s animation timer from now on; a run already in progress is
    /// measured from its next tick. `busy` must outlive the probe.
    explicit TimerGapProbe(BusyPanel& busy, QObject* parent = nullptr);

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    /// Harness work starts on the UI thread: the current gap's clock stops.
    void exclude_begin();
    /// The harness work ended: the current gap's clock runs on (the excluded time is not
    /// part of any gap).
    void exclude_end();

private:
    void on_started();
    void on_tick();
    void on_stopped();
    void add_gap(std::int64_t until_ns);
    [[nodiscard]] std::int64_t now_ns() const { return clock_.nsecsElapsed(); }

    BusyPanel& busy_;
    QElapsedTimer clock_;
    std::optional<std::int64_t> last_ns_;  ///< the run's start or its previous tick
    std::optional<std::int64_t> excluding_since_ns_;
    Stats stats_;
};

} // namespace wizz::app
