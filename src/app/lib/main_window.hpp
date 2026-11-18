#pragma once
// The main window (01 §3.3, §4, §5, §6.3, §6.4, §12, §13): the only top-level
// window; no menus, toolbar or tray icon.
//
//   * Title "WizzairHelper — cheapest Wizz Air flights from nearby airports".
//   * Geometry (§4.1): outer size L(1320) × L(820), or the saved device-pixel size
//     clamped to [L(980) × L(600), work area of the cursor's screen]; centred on that
//     screen's work area, never above or left of it; maximised from settings unless
//     --autotest. The position is never saved.
//   * Layout (§4.2): the BusyPanel docked at the bottom with its fixed height (it gets
//     its height first), the InputPanel at the top, the result tabs filling the rest
//     with header padding (12, 4): Outbound / Return / Round trips / Log (§5). Between
//     the input panel and the tabs sits the update bar (D-46), hidden unless an update
//     is ready or available.
//   * Keyboard (§6.3): an Enter nobody handled presses the AcceptButton that follows
//     focus (InputPanel::trigger_accept) — grids open their row and consume Enter, the
//     chooser and drop-downs are separate windows; a focused push button is clicked.
//     Esc anywhere in the window, while working and with no drop-down open, is the
//     Cancel button (BusyPanel::request_cancel, D-11). Focus starts in From.
//   * Start-up (§11.1): the first Shown loads the route map, then runs --autotest. While
//     Windows animates the window in (window_animation.hpp), that first work starts
//     kStartDelay after the first Shown, else in the next event-loop pass (D-65): during
//     the open animation a flush of the window can block the UI thread for over 100 ms,
//     and the map load repaints most of the window at once. Until then the window shows
//     what it was built with ("Ready."); closing it first starts nothing. A search the
//     user starts meanwhile loads the map itself (§11.4), so the start-up load is skipped.
//   * Closing (§13): stop running work first, then commit a number typed but not yet
//     entered and save settings.json (not in autotest), then let the updater start a
//     verified installer (D-47).
//   * The Log starts with the start-up line (§12.2).
//   * Updates (D-45): enable_updates() turns the self-updater on in a normal run; it
//     starts with OnShownAsync, after the start delay (its first evaluation may show the
//     update bar, which moves the tabs).
//   * Exchange rates at start (D-69): enable_rate_prefetch() makes a normal run fetch the
//     rates on a worker when the start-up work begins, right after OnShownAsync started,
//     silently: no busy panel, no Log line, the map load is not delayed.
//   * Settings (D-63): the gear "Settings" button at the right end of the tab row (the
//     tab widget's corner, after the tab bar in the Tab order) opens the window-modal
//     Settings window (open_settings). A change of its update options applies at once:
//     settings_.updates, the update controller's options, and settings.json is saved
//     (never in autotest). Without an update controller (--autotest, --replay) it shows
//     ui::updates_off_view(). Its "Searches" card sets how long searches reuse fetched
//     fares (D-68): settings_.fare_reuse and the Wizz client's reuse window, applied to
//     the next search, and settings.json saved like the update options. The window built
//     from settings.json gives the client that window from the start. Closing the main
//     window closes the Settings window first.

#include "app_services.hpp"
#include "task.hpp"
#include "update_controller.hpp"
#include "window_animation.hpp"

#include "wizz/ui/autotest_options.hpp"
#include "wizz/ui/settings.hpp"

#include <QMainWindow>
#include <QMargins>
#include <QPointer>
#include <QSize>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QTabWidget;
class QTimer;

namespace wizz::app {

class AutoTestRunner;
class BusyPanel;
class Controller;
class FlightGrid;
class InputPanel;
class LogView;
class RoundTripGrid;
class SettingsButton;
class SettingsDialog;
class UpdateBar;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// The tab pages in their fixed order (01 §5).
    enum class Page : int { outbound = 0, inbound = 1, round_trips = 2, log = 3 };

    /// L(980) × L(600) and L(1320) × L(820): outer sizes in logical pixels (01 §4.1).
    static constexpr QSize kMinimumSize{980, 600};
    static constexpr QSize kDefaultSize{1320, 820};
    /// Tab-header padding (horizontal, vertical) (01 §5).
    static constexpr QSize kTabPadding{12, 4};
    /// Around the results card: the input cards' side margins (D-53).
    static constexpr QMargins kResultsMargins{16, 4, 16, 8};
    /// From the first Shown to the first work while Windows animates the window in (D-65).
    /// The Shown comes about 25 ms before show() returns. On the owner's PC a flush of the
    /// window during the animation blocked the UI thread for up to 138 ms; flushes that began
    /// 240-300 ms after show() took up to 19.5 ms, and from 300 ms on 11 ms at most.
    static constexpr std::chrono::milliseconds kStartDelay{300};

    /// Whether Windows animates `window` in when it is first shown (opens_with_animation).
    using OpenAnimationProbe = std::function<bool(const QWidget& window)>;
    /// Monotonic nanoseconds since an arbitrary start.
    using MonotonicClock = std::function<std::int64_t()>;

    /// How the start-up went (D-65; diagnostics.json's `startup`). Times are in ms after
    /// the first Shown, which show() sends just before the native window appears.
    struct StartupRecord {
        bool window_animates = false;              ///< the probe's answer at the first Shown
        std::optional<WindowAnimationSettings> settings;  ///< Windows' settings read at that Shown
        std::chrono::milliseconds start_delay{0};  ///< kStartDelay when it animates, else 0
        std::optional<double> work_started_ms;     ///< OnShownAsync and the updater started
        std::optional<double> first_busy_ms;       ///< the busy panel's first busy period began
    };

    /// `settings` were loaded before the window is built (01 §3); `autotest` turns on the
    /// self-test mode and its runner (01 §17).
    MainWindow(std::shared_ptr<const AppServices> services, ui::AppSettings settings,
               std::optional<ui::AutoTestOptions> autotest = std::nullopt, QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    /// 01 §4.1 size, position and maximised state; call before the first show().
    void apply_start_geometry();
    /// apply_start_geometry() + show().
    void show_main_window();

    /// Replaces how the start delay is decided and timed (tests; opens_with_animation and a
    /// QElapsedTimer by default; an empty one keeps its default). Call before the first show().
    void set_start_timing(OpenAnimationProbe probe, MonotonicClock clock = {});
    [[nodiscard]] const StartupRecord& startup_record() const noexcept { return startup_; }
    /// The single-shot timer that starts the first work (runs from the first Shown until then).
    [[nodiscard]] QTimer* start_timer() const noexcept { return start_timer_; }
    /// OnShownAsync and the updater have started.
    [[nodiscard]] bool startup_begun() const noexcept { return startup_.work_started_ms.has_value(); }

    /// Turns the self-updater on (D-45): an UpdateController over `hooks` drives the
    /// update bar and the Log, follows the controller's working state, starts with
    /// OnShownAsync (after the start delay, D-65) and may start the installer when the
    /// window closes. Returns false and does nothing in an --autotest run (never with
    /// --autotest, D-30) or when the updater is already on.
    bool enable_updates(UpdateHooks hooks);

    /// Fetch the exchange rates when the start-up work begins (D-69): main calls it in a
    /// normal run only, like enable_updates. Returns false and does nothing in an
    /// --autotest run or when it is already on; after the start-up work began it starts
    /// at once.
    bool enable_rate_prefetch();
    [[nodiscard]] bool rate_prefetch_enabled() const noexcept { return rate_prefetch_; }

    // --- Parts ----------------------------------------------------------------------------
    [[nodiscard]] InputPanel* input_panel() const noexcept { return input_; }
    [[nodiscard]] BusyPanel* busy_panel() const noexcept { return busy_; }
    [[nodiscard]] QTabWidget* tabs() const noexcept { return tabs_; }
    [[nodiscard]] FlightGrid* outbound_grid() const noexcept { return outbound_; }
    [[nodiscard]] FlightGrid* return_grid() const noexcept { return inbound_; }
    [[nodiscard]] RoundTripGrid* round_trip_grid() const noexcept { return trips_; }
    [[nodiscard]] LogView* log_view() const noexcept { return log_; }
    [[nodiscard]] UpdateBar* update_bar() const noexcept { return update_bar_; }
    /// The gear at the right end of the tab row (the tab widget's top-right corner).
    [[nodiscard]] SettingsButton* settings_button() const noexcept { return settings_button_; }
    /// The Settings window while it is open, else null.
    [[nodiscard]] SettingsDialog* settings_dialog() const noexcept;
    [[nodiscard]] Controller* controller() const noexcept { return controller_; }
    /// The self-updater, or null when enable_updates() was not called (or refused).
    [[nodiscard]] UpdateController* update_controller() const noexcept { return updates_; }
    /// The --autotest script, or null in a normal run.
    [[nodiscard]] AutoTestRunner* autotest_runner() const noexcept { return autotest_runner_; }
    [[nodiscard]] const AppServices& services() const noexcept { return *services_; }
    [[nodiscard]] bool is_autotest() const noexcept { return autotest_.has_value(); }

    [[nodiscard]] QWidget* page(Page page) const;
    void select_page(Page page);

    /// The Settings button: shows the Settings window (window-modal, deleted when it
    /// closes) with settings_.updates and the update controller's status, or raises the
    /// one already open. Returns it.
    SettingsDialog* open_settings();

    // --- Settings (01 §13) --------------------------------------------------------------------
    /// The settings as closing saves them (after it committed pending numeric edits): the
    /// inputs, Maximized (WindowState == Maximized; a minimised window is not) and the
    /// outer size of normal_outer_device_size().
    [[nodiscard]] ui::AppSettings current_settings() const;
    /// The normal-state outer size in device pixels, `(WindowState == Normal) ? Bounds :
    /// RestoreBounds`: the current size in the normal state (an Aero-snapped window
    /// included), the size before maximising or minimising otherwise.
    [[nodiscard]] QSize normal_outer_device_size() const;

Q_SIGNALS:
    /// OnShownAsync finished: the start-up map load and, in autotest, the script.
    void startup_finished();

protected:
    /// QEvent::ThemeChange (Windows changed its colours or contrast): re-evaluate the look.
    bool event(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// Application-wide filter: the KeyPreview Esc of 01 §6.3.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void build();
    void wire();
    /// The start timer fired: the first work starts once the start delay has passed on the
    /// clock (a timer may fire a little early), else the timer waits the rest.
    void start_when_due();
    /// OnShownAsync and the updater's first evaluation start (D-45, D-65).
    void begin_startup();
    /// Milliseconds since the first Shown, on the start clock.
    [[nodiscard]] double since_shown_ms() const;
    /// OnShownAsync (01 §11.1).
    [[nodiscard]] Task<> on_shown();
    /// Esc pressed in `watched`: Cancel when it belongs to this window, work is running and
    /// no drop-down is open. Returns whether it was handled.
    bool handle_escape(QObject* watched);
    /// The Settings window changed the update options: keep them, apply them to the update
    /// controller and save settings.json (not in autotest).
    void apply_update_options(const update::UpdateOptions& options);
    /// The Settings window changed the reuse window of fetched fares (D-68): keep it, give it
    /// to the Wizz client for the next search and save settings.json (not in autotest).
    void apply_fare_reuse(fare_days::ReuseWindow window);

    std::shared_ptr<const AppServices> services_;
    ui::AppSettings settings_;
    std::optional<ui::AutoTestOptions> autotest_;
    bool shown_once_ = false;
    bool closed_ = false;
    bool rate_prefetch_ = false;
    OpenAnimationProbe open_animation_probe_;
    MonotonicClock clock_;
    std::int64_t shown_ns_ = 0;
    StartupRecord startup_;
    QTimer* start_timer_ = nullptr;

    InputPanel* input_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    FlightGrid* outbound_ = nullptr;
    FlightGrid* inbound_ = nullptr;
    RoundTripGrid* trips_ = nullptr;
    LogView* log_ = nullptr;
    BusyPanel* busy_ = nullptr;
    UpdateBar* update_bar_ = nullptr;
    SettingsButton* settings_button_ = nullptr;
    QPointer<SettingsDialog> settings_dialog_;
    Controller* controller_ = nullptr;
    AutoTestRunner* autotest_runner_ = nullptr;
    UpdateController* updates_ = nullptr;
};

} // namespace wizz::app
