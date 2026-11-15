#pragma once
// The hidden self-test script of 01 §17.2 (`RunAutoTestAsync`), run after the
// start-up map load even when that failed:
//
//   create outdir; summary {startedAt, apiUrl, airports}
//   progress shots every 2200 ms while working (at most 4): progress-{n}.png + the
//     busy status text
//   inputs (From, To, radii, currency, dates outTo, outFrom, retTo, retFrom)
//   outbound search → summary.outbound, 1-outbound.png
//   [--ret / --return-from-top] return search (right-click flow on the current or
//     top outbound fare for --return-from-top) → summary.return, 2-return.png;
//     Round trips tab, pump, 3-roundtrips.png, summary.roundTrips
//   [--then-currency] switch the currency, pump → summary.afterCurrencySwitch;
//     Outbound tab, pump, 5-currency-switched.png
//   Log tab, pump, 4-log.png          (numbering fixed: 5 is taken before 4)
//   exception → summary.exception; then progressScreenshots, finalStatus,
//   requestsToWizz (WizzApiClient::request_count), log; write summary.json;
//   close the window unless --keep-open.
//
// Test-only companions (DECISIONS D-30): --warm-cache runs the outbound search
// once before the script (the script's search then replays cached timetables);
// --cancel-after-ms n presses Esc, Cancel, Esc and the Cancel handler n ms after
// the script's outbound leg began fetching (Controller::leg_started), so one
// cancellation transition can be checked; --diagnostics writes diagnostics.json
// (autotest_diagnostics.hpp) next to summary.json: the 40 ms timer gaps
// (TimerGapProbe), the UI-loop watchdog (EventWatchdog, through
// Application::notify), the cancel record, each search's requests and duration, the
// final grids against the stored results, display and layout checks taken at
// every screenshot.
//
// "Pump messages" = QCoreApplication::processEvents, then a synchronous repaint,
// so the selected tab is painted before the capture. Screenshots are full-window
// PrintWindow(PW_RENDERFULLCONTENT) captures (window_capture.hpp); a failure is
// logged as "screenshot {name} failed: {message}" and the script continues.

#include "autotest_diagnostics.hpp"
#include "task.hpp"

#include "wizz/ui/autotest_options.hpp"
#include "wizz/ui/summary_json.hpp"

#include <QObject>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class QWidget;

namespace wizz::app {

class MainWindow;

class AutoTestRunner final : public QObject {
    Q_OBJECT

public:
    /// The progress-screenshot timer (01 §17.2) and its shot limit.
    static constexpr std::chrono::milliseconds kProgressShotInterval{2200};
    static constexpr int kMaxProgressShots = 4;
    static constexpr std::string_view kSummaryFileName = "summary.json";

    /// Writes a PNG of `window` to `path` (PrintWindow by default; tests record instead).
    using Screenshotter = std::function<std::expected<void, std::string>(const QWidget& window,
                                                                         const std::filesystem::path& path)>;

    AutoTestRunner(MainWindow& window, ui::AutoTestOptions options, QObject* parent = nullptr);
    /// Detaches the --diagnostics UI-loop watchdog from the Application.
    ~AutoTestRunner() override;

    AutoTestRunner(const AutoTestRunner&) = delete;
    AutoTestRunner& operator=(const AutoTestRunner&) = delete;

    /// RunAutoTestAsync. Exceptions from the script are recorded in summary.exception;
    /// only a failure to create the output directory escapes (like .NET).
    [[nodiscard]] Task<> run();

    [[nodiscard]] const ui::AutoTestOptions& options() const noexcept { return options_; }
    /// The summary of the last run (written to summary.json).
    [[nodiscard]] const std::optional<ui::AutoTestSummary>& summary() const noexcept { return summary_; }
    [[nodiscard]] std::filesystem::path summary_path() const;
    /// The --diagnostics record of the last run (written to diagnostics.json), or nullopt.
    [[nodiscard]] const std::optional<AutoTestDiagnostics>& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] std::filesystem::path diagnostics_path() const;
    void set_screenshotter(Screenshotter screenshotter) { screenshotter_ = std::move(screenshotter); }

Q_SIGNALS:
    /// summary.json was written (before the window closes).
    void finished();

private:
    /// Applies the script's inputs to the input panel (01 §17.2 first lines of `try`).
    void apply_inputs();
    void screenshot(std::string_view name);
    void pump();
    /// --cancel-after-ms: Esc to the focused control, a click on Cancel, Esc again and
    /// BusyPanel::request_cancel — the user pressing everything at once (D-11).
    [[nodiscard]] CancelDiagnostics press_cancel(double at_ms);
    /// Appends a search record (--diagnostics).
    void record_search(std::string name, std::int64_t requests_before, double duration_ms);
    /// Finishes and writes diagnostics.json (--diagnostics).
    void write_diagnostics();

    MainWindow& window_;
    ui::AutoTestOptions options_;
    Screenshotter screenshotter_;
    std::optional<ui::AutoTestSummary> summary_;
    std::optional<AutoTestDiagnostics> diagnostics_;
    std::unique_ptr<TimerGapProbe> probe_;
    /// Kept for the runner's lifetime: a dispatch that was running when it was detached
    /// still leaves it.
    std::unique_ptr<EventWatchdog> watchdog_;
};

} // namespace wizz::app
