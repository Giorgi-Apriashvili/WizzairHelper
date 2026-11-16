#pragma once
// The progress panel of 01 §10 (HARD REQUIREMENT): spinner, eased bar or
// marquee, current step, elapsed time + ETA, retry countdown / "waiting for
// server" note, taskbar progress and Cancel. All rules live in
// wizz::ui::BusyPanelModel (Qt-free, unit-tested); this widget copies the
// model's view into its controls after every operation and on a 40 ms precise
// timer that runs exactly while busy (R-04), forwards the model's
// de-duplicated taskbar calls to TaskbarProgress and runs the 4 s one-shot that
// clears the taskbar Error state.
//
// Layout (01 §10.1): background SystemColors.ControlLight (COLOR_3DLIGHT, read
// from Windows like the status line's ControlText, see system_colours.hpp),
// padding (8, 5, 8, 5), a 4 × 2 grid:
//   col 0: 36 px  spinner (both rows, 6 px right margin)
//   col 1: 100 %  progress bar (margin 0, 2, 8, 2) / status line (elided)
//   col 2: auto   elapsed (Courier New 9.5 pt bold) / wait note (DarkOrange, bold)
//   col 3: auto   Cancel (both rows, right, default 3 px button margin)
// Row heights come from font metrics and are recomputed on font, style and
// device-pixel-ratio changes (UI-6).
//
// Cancel and Esc share request_cancel (D-11, 07 §4): it disables Cancel, shows
// "Cancelling… (finishing the current request)" and emits cancel_requested()
// once; repeats are ignored until the next cancellable begin().
//
// UI thread only. Worker progress must be marshalled to the UI thread before
// report(); reports that arrive after end() are ignored.

#include "panel_progress_bar.hpp"
#include "taskbar_progress.hpp"

#include "wizz/core/locale.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/core/time.hpp"
#include "wizz/ui/busy_panel_model.hpp"

#include <QColor>
#include <QMargins>
#include <QWidget>

#include <memory>
#include <span>
#include <string>
#include <string_view>

class QFont;
class QGridLayout;
class QLabel;
class QPaintDevice;
class QProgressBar;
class QPushButton;
class QTimer;

namespace wizz::app {

class ElidedLabel;
class SpinnerWidget;

/// The measured layout of 01 §10.1, in logical (Qt) pixels.
struct BusyPanelMetrics {
    int line = 0;              ///< max(textHeight("Ag", panel font), textHeight("Ag", wait font))
    int bar_row = 0;           ///< max(22, textHeight("0:00", elapsed font) + 4)
    int status_row = 0;        ///< line + 6
    int spinner_column = 36;   ///< absolute column 0
    QMargins padding{8, 5, 8, 5};
    int height = 0;            ///< bar_row + status_row + padding.top + padding.bottom

    friend bool operator==(const BusyPanelMetrics&, const BusyPanelMetrics&) = default;
};

class BusyPanel final : public QWidget {
    Q_OBJECT

public:
    /// Range of the progress bar: 0..BarMax + 1 (01 §10.1, §10.6).
    static constexpr int kBarRangeMax = ui::BusyPanelModel::kBarMax + 1;
    /// Status colours (01 §10.3) and the wait note colour (01 §10.1).
    static constexpr QColor kSuccessColour{0, 100, 0};    // DarkGreen
    static constexpr QColor kFailureColour{178, 34, 34};  // Firebrick
    static constexpr QColor kWaitColour{255, 140, 0};     // DarkOrange

    /// `clock` must outlive the panel (elapsed time, ETA and wait notes use utc_now).
    /// `locale` supplies the decimal separator of the wait notes. `taskbar_factory`
    /// creates the ITaskbarList3 (empty = the shell's).
    BusyPanel(const Clock& clock, std::shared_ptr<const Locale> locale, const TaskbarProgress::Factory& taskbar_factory,
              QWidget* parent = nullptr);
    BusyPanel(const Clock& clock, std::shared_ptr<const Locale> locale, QWidget* parent = nullptr);
    ~BusyPanel() override;

    BusyPanel(const BusyPanel&) = delete;
    BusyPanel& operator=(const BusyPanel&) = delete;

    /// Begin(text, cancellable): status in ControlText, marquee, spinner on, Cancel enabled
    /// when `cancellable`, taskbar Indeterminate, 40 ms timer started.
    void begin(std::string_view text, bool cancellable = true);
    /// Report(p): ignored unless busy; status "Stage:  Detail    (k of N)", immediate refresh.
    void report(const SearchProgress& progress);
    /// Report(p) for each of `reports` in order (each counts in updates_received()), then
    /// one refresh: a burst delivered together (JobMailbox) is drawn once, as its last report.
    void report(std::span<const SearchProgress> reports);
    /// End(summary, success): green / red summary, bar at 100 % (success) or left in
    /// place, elapsed without ETA, timer stopped, taskbar None / Error (then None after 4 s).
    void end(std::string_view summary, bool success = true);
    /// The safety net of every phase's `finally` (01 §3.2 step 3, UI-3): when still busy,
    /// End(UnexpectedErrorText, false).
    void end_if_still_busy();

    [[nodiscard]] bool is_busy() const noexcept;
    /// The full status line (autotest `finalStatus`, progress screenshots).
    [[nodiscard]] const std::string& status_text() const noexcept;
    /// Reports received since the last begin() (autotest `progressUpdates`).
    [[nodiscard]] int updates_received() const noexcept;
    [[nodiscard]] const ui::BusyPanelModel& model() const noexcept { return model_; }
    [[nodiscard]] const BusyPanelMetrics& metrics() const noexcept { return metrics_; }

    /// 01 §10.1 metrics for the given fonts, measured on `device` (screen when null).
    [[nodiscard]] static BusyPanelMetrics compute_metrics(const QFont& panel_font, const QFont& wait_font,
                                                          const QFont& elapsed_font, const QPaintDevice* device = nullptr);
    /// Courier New 9.5 pt bold (monospace fallback).
    [[nodiscard]] static QFont elapsed_font();

    // The controls, for the window (focus, Esc) and for tests.
    [[nodiscard]] SpinnerWidget* spinner() const noexcept { return spinner_; }
    [[nodiscard]] QProgressBar* progress_bar() const noexcept { return bar_; }
    [[nodiscard]] QLabel* elapsed_label() const noexcept { return elapsed_; }
    [[nodiscard]] ElidedLabel* status_label() const noexcept { return status_; }
    [[nodiscard]] QLabel* wait_label() const noexcept { return wait_; }
    [[nodiscard]] QPushButton* cancel_button() const noexcept { return cancel_; }
    /// The 40 ms animation timer (runs exactly while busy).
    [[nodiscard]] QTimer* animation_timer() const noexcept { return timer_; }
    [[nodiscard]] TaskbarProgress& taskbar() noexcept { return taskbar_; }
    /// The status colour for the current tone: ControlText, DarkGreen or Firebrick.
    [[nodiscard]] QColor status_colour() const;
    /// SystemColors.ControlLight, the panel background.
    [[nodiscard]] QColor background_colour() const { return control_light_; }

public Q_SLOTS:
    /// The one Cancel / Esc handler (D-11): busy and Cancel enabled → disable Cancel, show
    /// "Cancelling… (finishing the current request)", emit cancel_requested(); else nothing.
    void request_cancel();

Q_SIGNALS:
    /// Emitted once per cancellable busy period (MainWindow: request_stop on the job).
    void cancel_requested();
    /// The 40 ms animation timer (objectName "animationTimer") was started: a new run of
    /// ticks begins (the --autotest timer-gap diagnostics, R-04, 05 §5).
    void animation_started();
    /// The animation timer was stopped (End): the run of ticks is over.
    void animation_stopped();

protected:
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void animate();
    void error_reset_elapsed();
    void render();
    void apply_metrics();
    void refresh_system_colours();
    void apply_status_colour();
    void set_bar_value(int value);

    ui::BusyPanelModel model_;
    BusyPanelMetrics metrics_;
    ui::StatusTone shown_tone_ = ui::StatusTone::normal;
    QColor control_light_;
    QColor control_text_;

    QGridLayout* grid_ = nullptr;
    QWidget* bar_box_ = nullptr;
    SpinnerWidget* spinner_ = nullptr;
    PanelProgressBar* bar_ = nullptr;  ///< still while idle (no style glow)
    QLabel* elapsed_ = nullptr;
    ElidedLabel* status_ = nullptr;
    QLabel* wait_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QTimer* timer_ = nullptr;
    QTimer* error_reset_timer_ = nullptr;
    TaskbarProgress taskbar_;
};

} // namespace wizz::app
