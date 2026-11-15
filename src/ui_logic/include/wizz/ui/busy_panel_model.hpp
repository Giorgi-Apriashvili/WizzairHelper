#pragma once
// The BusyPanel of 01 §10 (HARD REQUIREMENT) as a pure, window-free state
// machine: Begin / Report / End / RequestCancel and the 40 ms Animate tick
// produce a BusyView (status line and colour role, elapsed + ETA text, wait
// note, bar style and value, spinner, Cancel state) plus the de-duplicated
// taskbar calls of §10.9. The Qt widget only copies the view into its controls
// and forwards the taskbar calls to ITaskbarList3 (D-04).
//
// Time comes from the injected Clock (`utc_now`, like the .NET panel's
// DateTime.UtcNow); tests drive it with FixedClock (05 U-05..U-08, R-04). The
// wait-note numbers use the locale's decimal separator and the 07 §2 label
// rounding ("0.0" / "0", halves away from zero).
//
// Threading: UI thread only (not thread-safe). Worker progress must be
// marshalled to the UI thread first; `report` ignores anything that arrives
// after `end` (01 §10.3, §14).

#include "wizz/core/locale.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/core/time.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wizz::ui {

/// Colour role of the status line: ControlText while busy, DarkGreen (0,100,0)
/// after a successful End, Firebrick (178,34,34) after a failed one (01 §10.3).
enum class StatusTone { normal, success, failure };

/// Progress bar style: determinate (Continuous) or indeterminate (Marquee, 25 ms).
enum class BarStyle { continuous, marquee };

/// ITaskbarList3 TBPF_* states (01 §10.9).
enum class TaskbarState : int { none = 0, indeterminate = 1, normal = 2, error = 4, paused = 8 };

/// One call the widget must forward to ITaskbarList3.
struct TaskbarCall {
    enum class Kind { set_state, set_value };
    Kind kind = Kind::set_state;
    TaskbarState state = TaskbarState::none; ///< set_state: SetProgressState(hwnd, state)
    int value = 0;                           ///< set_value: SetProgressValue(hwnd, value, 1000)

    friend bool operator==(const TaskbarCall&, const TaskbarCall&) = default;
};

/// The de-duplicating taskbar wrapper of 01 §10.9: SetState skips a repeat and
/// forces the next value to be sent; SetValue clamps to 0..1000, skips a repeat
/// and implies the Normal state. Records the calls that would reach COM.
class TaskbarProgress {
public:
    /// `_lastValue` sentinel ("MAX"): the next SetValue is always sent.
    static constexpr int kNoValue = 2'147'483'647;
    static constexpr int kMaxValue = 1000;

    void set_state(TaskbarState state);
    void set_value(int permille);

    [[nodiscard]] TaskbarState last_state() const noexcept { return last_state_; }
    [[nodiscard]] int last_value() const noexcept { return last_value_; }
    /// The calls made since the previous take, oldest first.
    [[nodiscard]] std::vector<TaskbarCall> take_calls();

private:
    TaskbarState last_state_ = TaskbarState::none; ///< the window starts without taskbar progress
    int last_value_ = kNoValue;
    std::vector<TaskbarCall> calls_;
};

/// What the panel shows (01 §10.1 initial values: "Ready.", Cancel disabled,
/// Continuous bar at 0, empty elapsed and wait texts, idle spinner dot).
struct BusyView {
    std::string status_text{"Ready."};
    StatusTone status_tone = StatusTone::normal;
    std::string elapsed_text;   ///< Courier New 9.5 pt bold, e.g. "0:12  ~25 s left"
    std::string wait_text;      ///< DarkOrange, e.g. "⏳ retrying in 4.2 s"
    bool cancel_enabled = false;
    BarStyle bar_style = BarStyle::continuous;
    int bar_value = 0;          ///< 0..1000 (range 0..1001, 01 §10.6)
    bool spinning = false;
    int spinner_angle = 0;      ///< degrees, GDI+ convention (clockwise from 3 o'clock)
    std::uint64_t frame = 0;    ///< incremented on every repaint (Begin, Report, End, Cancel, Animate)

    friend bool operator==(const BusyView&, const BusyView&) = default;
};

/// The `Report` status text (01 §10.3, U-05): Stage, then ":  " + Detail when the
/// detail is not empty, then "    (" + min(Completed + 1, Total) + " of " + Total + ")"
/// when Total > 0.
[[nodiscard]] std::string progress_status_text(const SearchProgress& progress);

/// Spinner `Step()`: (angle + 24) mod 360 (600°/s at 40 ms, 01 §10.8).
[[nodiscard]] constexpr int next_spinner_angle(int angle) noexcept
{
    return (angle + 24) % 360;
}

/// Spinner drawing geometry for a control of `width` × `height` px (01 §10.8).
struct SpinnerGeometry {
    int left = 0;            ///< the centred square of side `size`
    int top = 0;
    int size = 0;            ///< min(W, H) − 8
    float thickness = 0;     ///< max(2.5, size / 7)
    int dot_diameter = 0;    ///< (int)thickness + 2
    int dot_left = 0;        ///< left + size/2 − d/2 (integer maths)
    int dot_top = 0;
    int sweep_degrees = 100; ///< arc sweep, clockwise from `angle`

    friend bool operator==(const SpinnerGeometry&, const SpinnerGeometry&) = default;
};
/// Nothing is drawn (nullopt) when min(W, H) − 8 < 8.
[[nodiscard]] std::optional<SpinnerGeometry> spinner_geometry(int width, int height) noexcept;

class BusyPanelModel {
public:
    /// The Animate timer interval; R-04 requires a repaint at least every 50 ms while busy.
    static constexpr std::chrono::milliseconds kTimerInterval{40};
    /// One-shot delay after a failed End before the taskbar Error state is cleared.
    static constexpr std::chrono::milliseconds kErrorResetDelay{4000};
    static constexpr int kBarMax = 1000;
    /// Easing: the remaining gap × 0.3 per tick; snap when the gap is below 0.002.
    static constexpr double kEasing = 0.3;
    static constexpr double kSnapGap = 0.002;
    /// "waiting for server…" appears once a step has run longer than this.
    static constexpr std::chrono::seconds kSlowStep{3};

    /// `clock` must outlive the model. `locale` supplies the wait-note separators.
    BusyPanelModel(const Clock& clock, std::shared_ptr<const Locale> locale);

    /// Begin(text, cancellable): busy, counters reset, status in ControlText, wait note
    /// cleared, Cancel enabled when `cancellable`, spinner on, Marquee, taskbar
    /// Indeterminate, timer started, then one Animate.
    void begin(std::string_view text, bool cancellable = true);

    /// Report(p): ignored unless busy. Stores p, counts it, sets the target
    /// (Completed / Total clamped to 0..1, or indeterminate for Total 0), restarts the ETA
    /// base when a new determinate stage starts, sets the status text, then Animate.
    void report(const SearchProgress& progress);

    /// End(summary, success): not busy, timer stopped, spinner idle, Cancel disabled,
    /// wait note cleared, Continuous bar at (int)(shown × 1000) with shown = 1 on success
    /// (truncation, 07 §2), elapsed = FormatElapsed(now − last Begin) without ETA, status
    /// coloured green / red, taskbar None / Error. A failed End requests the 4 s one-shot
    /// (`take_error_reset_request`). Callable when not busy (01 §10.3 [WART]).
    void end(std::string_view summary, bool success = true);

    /// The one idempotent Cancel / Esc handler (07 §4, D-11). Returns true exactly when a
    /// cancellation must be signalled: busy and Cancel enabled. It then disables Cancel and
    /// shows "Cancelling… (finishing the current request)". Repeats return false until a new
    /// cancellable Begin.
    [[nodiscard]] bool request_cancel();

    /// The 40 ms tick (and the immediate refresh after Report): elapsed + ETA text, spinner
    /// step, eased bar or Marquee, taskbar, wait note. Does nothing when not busy.
    void animate();

    /// True once after a failed End: the widget starts a kErrorResetDelay one-shot timer
    /// and calls `error_reset_elapsed` when it fires.
    [[nodiscard]] bool take_error_reset_request() noexcept;
    /// The one-shot fired: taskbar None unless a new busy period has begun.
    void error_reset_elapsed();

    [[nodiscard]] const BusyView& view() const noexcept { return view_; }
    [[nodiscard]] bool is_busy() const noexcept { return busy_; }
    /// The 40 ms timer runs exactly while busy.
    [[nodiscard]] bool timer_running() const noexcept { return busy_; }
    /// Reports received since the last Begin (autotest `progressUpdates`).
    [[nodiscard]] int updates_received() const noexcept { return updates_received_; }
    [[nodiscard]] const std::string& status_text() const noexcept { return view_.status_text; }
    /// Target 0..1, or < 0 for indeterminate.
    [[nodiscard]] double target() const noexcept { return target_; }
    /// The eased value 0..1 the bar shows.
    [[nodiscard]] double shown() const noexcept { return shown_; }
    [[nodiscard]] const std::optional<SearchProgress>& last_report() const noexcept { return last_; }
    [[nodiscard]] const TaskbarProgress& taskbar() const noexcept { return taskbar_; }
    /// Taskbar calls made since the previous take (forward them to ITaskbarList3).
    [[nodiscard]] std::vector<TaskbarCall> take_taskbar_calls() { return taskbar_.take_calls(); }

private:
    void set_bar_value(int value) noexcept;
    [[nodiscard]] std::string wait_note(UtcInstant now) const;

    const Clock& clock_;
    std::shared_ptr<const Locale> locale_;
    BusyView view_;
    TaskbarProgress taskbar_;
    bool busy_ = false;
    std::optional<SearchProgress> last_;
    UtcInstant started_utc_{};
    double target_ = 0.0;
    double shown_ = 0.0;
    std::optional<std::string> eta_stage_;
    UtcInstant eta_start_utc_{};
    int eta_base_completed_ = 0;
    int updates_received_ = 0;
    bool error_reset_requested_ = false;
};

} // namespace wizz::ui
