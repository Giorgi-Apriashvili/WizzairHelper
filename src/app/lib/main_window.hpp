#pragma once
// The main window (01 §3.3, §4, §5, §6.3, §6.4, §12, §13): the only top-level
// window; no menus, toolbar or tray icon.
//
//   * Title "WizzairHelper — cheapest Wizz Air flights from nearby airports".
//   * Geometry (§4.1): outer size L(1320) × L(820), or the saved device-pixel size
//     clamped to [L(980) × L(600), work area of the cursor's screen]; centred on that
//     screen's work area; maximised from settings unless --autotest. The position is
//     never saved.
//   * Layout (§4.2): the BusyPanel docked at the bottom with its fixed height (it gets
//     its height first), the InputPanel at the top, the result tabs filling the rest
//     with header padding (12, 4): Outbound / Return / Round trips / Log (§5).
//   * Keyboard (§6.3): an Enter nobody handled presses the AcceptButton that follows
//     focus (InputPanel::trigger_accept) — grids open their row and consume Enter, the
//     chooser and drop-downs are separate windows; a focused push button is clicked.
//     Esc anywhere in the window, while working and with no drop-down open, is the
//     Cancel button (BusyPanel::request_cancel, D-11). Focus starts in From.
//   * Start-up (§11.1): the first Shown loads the route map, then runs --autotest.
//   * Closing (§13): stop running work first, then save settings.json (not in autotest).
//   * The Log starts with the start-up line (§12.2).

#include "app_services.hpp"
#include "task.hpp"

#include "wizz/ui/autotest_options.hpp"
#include "wizz/ui/settings.hpp"

#include <QMainWindow>
#include <QSize>

#include <memory>
#include <optional>

class QTabWidget;

namespace wizz::app {

class AutoTestRunner;
class BusyPanel;
class Controller;
class FlightGrid;
class InputPanel;
class LogView;
class RoundTripGrid;

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

    // --- Parts ----------------------------------------------------------------------------
    [[nodiscard]] InputPanel* input_panel() const noexcept { return input_; }
    [[nodiscard]] BusyPanel* busy_panel() const noexcept { return busy_; }
    [[nodiscard]] QTabWidget* tabs() const noexcept { return tabs_; }
    [[nodiscard]] FlightGrid* outbound_grid() const noexcept { return outbound_; }
    [[nodiscard]] FlightGrid* return_grid() const noexcept { return inbound_; }
    [[nodiscard]] RoundTripGrid* round_trip_grid() const noexcept { return trips_; }
    [[nodiscard]] LogView* log_view() const noexcept { return log_; }
    [[nodiscard]] Controller* controller() const noexcept { return controller_; }
    /// The --autotest script, or null in a normal run.
    [[nodiscard]] AutoTestRunner* autotest_runner() const noexcept { return autotest_runner_; }
    [[nodiscard]] const AppServices& services() const noexcept { return *services_; }
    [[nodiscard]] bool is_autotest() const noexcept { return autotest_.has_value(); }

    [[nodiscard]] QWidget* page(Page page) const;
    void select_page(Page page);

    // --- Settings (01 §13) --------------------------------------------------------------------
    /// The settings as closing would save them: the inputs, Maximized and the
    /// normal-state outer size in device pixels.
    [[nodiscard]] ui::AppSettings current_settings() const;
    /// The normal-state outer size in device pixels (`RestoreBounds`).
    [[nodiscard]] QSize normal_outer_device_size() const;

Q_SIGNALS:
    /// OnShownAsync finished: the start-up map load and, in autotest, the script.
    void startup_finished();

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// Application-wide filter: the KeyPreview Esc of 01 §6.3.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void build();
    void wire();
    /// OnShownAsync (01 §11.1).
    [[nodiscard]] Task<> on_shown();
    /// Esc pressed in `watched`: Cancel when it belongs to this window, work is running and
    /// no drop-down is open. Returns whether it was handled.
    bool handle_escape(QObject* watched);

    std::shared_ptr<const AppServices> services_;
    ui::AppSettings settings_;
    std::optional<ui::AutoTestOptions> autotest_;
    bool shown_once_ = false;

    InputPanel* input_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    FlightGrid* outbound_ = nullptr;
    FlightGrid* inbound_ = nullptr;
    RoundTripGrid* trips_ = nullptr;
    LogView* log_ = nullptr;
    BusyPanel* busy_ = nullptr;
    Controller* controller_ = nullptr;
    AutoTestRunner* autotest_runner_ = nullptr;
};

} // namespace wizz::app
