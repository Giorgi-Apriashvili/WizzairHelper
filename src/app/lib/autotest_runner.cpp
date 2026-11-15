#include "autotest_runner.hpp"

#include "application.hpp"
#include "busy_panel.hpp"
#include "controller.hpp"
#include "crash_reporter.hpp"
#include "flight_grid.hpp"
#include "input_panel.hpp"
#include "log_view.hpp"
#include "main_window.hpp"
#include "round_trip_grid.hpp"
#include "window_capture.hpp"

#include "wizz/ui/strings.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>

#include <exception>
#include <format>
#include <fstream>
#include <utility>
#include <vector>

namespace wizz::app {

namespace {

/// Label → input panel → central widget → window: enough passes for every nesting level.
constexpr int kLayoutPasses = 6;

[[nodiscard]] std::optional<ui::DateWindow> window_of(const std::optional<ui::AutoTestDates>& dates)
{
    if (!dates) {
        return std::nullopt;
    }
    return ui::DateWindow{.start = dates->start, .end = dates->end};
}

void write_text_file(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] double elapsed_ms(const QElapsedTimer& clock)
{
    return static_cast<double>(clock.nsecsElapsed()) / 1'000'000.0;
}

} // namespace

AutoTestRunner::AutoTestRunner(MainWindow& window, ui::AutoTestOptions options, QObject* parent)
    : QObject(parent), window_(window), options_(std::move(options)), screenshotter_(&save_window_png)
{
}

AutoTestRunner::~AutoTestRunner()
{
    if (auto* application = qobject_cast<Application*>(QCoreApplication::instance());
        application != nullptr && watchdog_ && application->event_watchdog() == watchdog_.get()) {
        application->set_event_watchdog(nullptr);
    }
}

std::filesystem::path AutoTestRunner::summary_path() const
{
    return options_.output_dir / kSummaryFileName;
}

std::filesystem::path AutoTestRunner::diagnostics_path() const
{
    return options_.output_dir / kDiagnosticsFileName;
}

Task<> AutoTestRunner::run()
{
    Controller& controller = *window_.controller();
    const AppServices& services = controller.services();
    const Locale& locale = *services.locale;
    BusyPanel& busy = *window_.busy_panel();

    std::filesystem::create_directories(options_.output_dir);

    ui::AutoTestSummary summary;
    summary.started_at = ui::started_at_text(services.clock->local_now());
    summary.api_url = services.wizz->current_api_url();
    if (controller.catalog()) {
        summary.airports = controller.catalog()->real_airports().size();
    }

    // Test-only companions (D-30).
    diagnostics_.reset();
    probe_.reset();
    auto* const application = qobject_cast<Application*>(QCoreApplication::instance());
    if (options_.diagnostics) {
        diagnostics_.emplace();
        probe_ = std::make_unique<TimerGapProbe>(busy);
        if (application != nullptr && !watchdog_) {
            watchdog_ = std::make_unique<EventWatchdog>([panel = &busy] { return panel->is_busy(); });
            application->set_event_watchdog(watchdog_.get());
        }
    }
    int cancel_requests = 0;
    std::optional<CancelDiagnostics> cancel_record;
    int cancel_requests_at_press = 0;
    QElapsedTimer leg_clock;
    QTimer cancel_timer;
    cancel_timer.setSingleShot(true);
    cancel_timer.setTimerType(Qt::PreciseTimer);
    // The context of every connection to the long-lived widgets. Declared after the locals
    // its lambdas capture, so destroying this frame (window closed mid-script) cuts the
    // connections before those locals go.
    QObject scope;
    connect(&busy, &BusyPanel::cancel_requested, &scope, [&cancel_requests] { ++cancel_requests; });
    connect(&cancel_timer, &QTimer::timeout, &scope, [&] {
        cancel_requests_at_press = cancel_requests;
        cancel_record = press_cancel(elapsed_ms(leg_clock));
    });

    std::vector<std::string> progress_snapshots;
    int shot = 0;
    QTimer shot_timer;
    shot_timer.setInterval(kProgressShotInterval);
    connect(&shot_timer, &QTimer::timeout, this, [&] {
        if (!controller.is_working() || shot >= kMaxProgressShots) {
            return;
        }
        screenshot(std::format("progress-{}.png", ++shot));
        progress_snapshots.push_back(busy.status_text());
    });

    try {
        apply_inputs();
        if (options_.warm_cache) {
            // The same outbound search once before the script: the script's search then
            // replays every timetable from the client's memory cache (05 §5 "cached replay").
            const std::int64_t requests = services.wizz->request_count();
            QElapsedTimer clock;
            clock.start();
            co_await controller.search_outbound();
            record_search("warm-up outbound", requests, elapsed_ms(clock));
        }
        if (options_.cancel_after) {
            // Armed for the script's outbound leg only: its fetch phase starts the countdown.
            connect(&controller, &Controller::leg_started, &scope, [&](Controller::Leg leg) {
                if (leg == Controller::Leg::outbound && !leg_clock.isValid()) {
                    leg_clock.start();
                    cancel_timer.start(*options_.cancel_after);
                }
            });
        }
        shot_timer.start();
        const std::int64_t outbound_requests = services.wizz->request_count();
        QElapsedTimer outbound_clock;
        outbound_clock.start();
        co_await controller.search_outbound();
        cancel_timer.stop();  // never carried into a later search
        if (cancel_record) {
            cancel_record->cancel_requests = cancel_requests - cancel_requests_at_press;
        }
        record_search("outbound", outbound_requests, elapsed_ms(outbound_clock));
        summary.outbound = ui::describe(controller.outbound_result().get(), window_.outbound_grid()->model()->state(),
                                        busy.updates_received(), locale);
        screenshot("1-outbound.png");

        if (options_.runs_return()) {
            FlightGrid& outbound = *window_.outbound_grid();
            const std::int64_t return_requests = services.wizz->request_count();
            QElapsedTimer return_clock;
            return_clock.start();
            if (options_.return_from_top && !outbound.shown_results().empty()) {
                FlightResultPtr top = outbound.selected();
                if (!top) {
                    top = outbound.shown_results().front();
                }
                co_await controller.plan_return_from(std::move(top));
            } else {
                co_await controller.search_return();
            }
            record_search("return", return_requests, elapsed_ms(return_clock));
            summary.inbound = ui::describe(controller.return_result().get(), window_.return_grid()->model()->state(),
                                           busy.updates_received(), locale);
            screenshot("2-return.png");
            window_.select_page(MainWindow::Page::round_trips);
            pump();
            screenshot("3-roundtrips.png");
            summary.round_trips = ui::describe_trips(window_.round_trip_grid()->model()->state(), locale);
        }

        if (const std::optional<std::string> currency = options_.listed_then_currency()) {
            (void)window_.input_panel()->set_currency(*currency);  // fires OnCurrencyChanged (01 §11.7)
            pump();
            summary.after_currency_switch = ui::CurrencySwitchJson{
                .currency = *currency,
                .outbound = ui::describe(controller.outbound_result().get(), window_.outbound_grid()->model()->state(),
                                         0, locale),
                .round_trips = ui::describe_trips(window_.round_trip_grid()->model()->state(), locale),
            };
            window_.select_page(MainWindow::Page::outbound);
            pump();
            screenshot("5-currency-switched.png");
        }

        shot_timer.stop();
        window_.select_page(MainWindow::Page::log);
        pump();
        screenshot("4-log.png");
    } catch (...) {
        summary.exception = describe_current_exception().details;  // ex.ToString()
    }
    shot_timer.stop();
    cancel_timer.stop();
    if (diagnostics_) {
        diagnostics_->cancel = std::move(cancel_record);
        if (watchdog_ && application != nullptr && application->event_watchdog() == watchdog_.get()) {
            diagnostics_->events = watchdog_->stats();
            application->set_event_watchdog(nullptr);
        }
    }

    summary.progress_screenshots = std::move(progress_snapshots);
    summary.final_status = busy.status_text();
    summary.requests_to_wizz = services.wizz->request_count();
    summary.log = ui::split_log(window_.log_view()->text());
    write_text_file(summary_path(), ui::to_json(summary));
    summary_ = std::move(summary);
    if (diagnostics_) {
        write_diagnostics();
    }
    Q_EMIT finished();

    if (!options_.keep_open) {
        window_.close();
    }
}

void AutoTestRunner::apply_inputs()
{
    InputPanel& input = *window_.input_panel();
    input.set_from_text(options_.from);
    input.set_to_text(options_.to);
    if (options_.from_radius_km) {
        input.set_from_radius_km(*options_.from_radius_km);  // clamped 0..1500
    }
    if (options_.to_radius_km) {
        input.set_to_radius_km(*options_.to_radius_km);
    }
    if (const std::optional<std::string> currency = options_.listed_currency()) {
        (void)input.set_currency(*currency);
    }
    // outTo, outFrom, retTo, retFrom — each clamped, each running the 01 §6.1 rules.
    input.apply_autotest_dates(window_of(options_.outbound), window_of(options_.inbound));
}

void AutoTestRunner::pump()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    window_.repaint();  // paint the selected tab now, before the capture
}

CancelDiagnostics AutoTestRunner::press_cancel(double at_ms)
{
    BusyPanel& busy = *window_.busy_panel();
    CancelDiagnostics record{.after = options_.cancel_after.value_or(std::chrono::milliseconds{0}),
                             .pressed_at_ms = at_ms,
                             .busy_at_press = busy.is_busy(),
                             .status_before = busy.status_text()};
    // The key goes where a real one would: the focused control (or the window), through the
    // application-wide KeyPreview filter of 01 §6.3.
    QWidget* target = QApplication::focusWidget();
    if (target == nullptr || target->window() != &window_) {
        target = window_.focusWidget() != nullptr ? window_.focusWidget() : &window_;
    }
    const auto escape = [&] {
        QKeyEvent press{QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
        QCoreApplication::sendEvent(target, &press);
        QKeyEvent release{QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier};
        QCoreApplication::sendEvent(target, &release);
        record.presses.emplace_back("Esc");
    };
    escape();
    busy.cancel_button()->click();  // does nothing once Cancel is disabled
    record.presses.emplace_back("Cancel button");
    escape();
    busy.request_cancel();
    record.presses.emplace_back("Cancel handler");
    record.status_after_press = busy.status_text();
    record.cancel_enabled_after_press = busy.cancel_button()->isEnabled();
    return record;
}

void AutoTestRunner::record_search(std::string name, std::int64_t requests_before, double duration_ms)
{
    if (!diagnostics_) {
        return;
    }
    diagnostics_->searches.push_back(SearchDiagnostics{
        .name = std::move(name),
        .wizz_requests = window_.services().wizz->request_count() - requests_before,
        .duration_ms = duration_ms,
        .final_status = window_.busy_panel()->status_text(),
    });
}

void AutoTestRunner::write_diagnostics()
{
    const Controller& controller = *window_.controller();
    if (probe_) {
        diagnostics_->timer = probe_->stats();
    }
    diagnostics_->grids = {
        describe_grid("outbound", window_.outbound_grid()->all_results(), controller.outbound_result().get()),
        describe_grid("return", window_.return_grid()->all_results(), controller.return_result().get()),
    };
    diagnostics_->display = describe_display(window_);
    if (diagnostics_->layout.checks == 0) {
        diagnostics_->layout.merge("final", describe_layout(window_));  // no screenshot was taken
    }
    diagnostics_->environment = describe_environment(*controller.services().locale);
    try {
        write_text_file(diagnostics_path(), to_json(*diagnostics_));
    } catch (const std::exception& error) {
        window_.controller()->log(std::format("diagnostics.json not written: {}", error.what()));
    }
}

void AutoTestRunner::screenshot(std::string_view name)
{
    // The capture is the harness's work on the UI thread, not the app's: keep it out of
    // the 40 ms timer's gaps (TimerGapProbe).
    if (probe_) {
        probe_->exclude_begin();
    }
    if (watchdog_) {
        watchdog_->exclude_begin();
    }
    const auto resume = qScopeGuard([this] {
        if (probe_) {
            probe_->exclude_end();
        }
        if (watchdog_) {
            watchdog_->exclude_end();
        }
    });
    // WinForms lays out synchronously when a label's text changes; Qt posts a layout
    // request. Apply the pending layouts (and nothing else) so the capture shows the
    // window as it is now, e.g. the busy panel's columns after End(). A wrapped label that
    // grows re-lays out its parents one posted request per level, hence the few passes.
    for (int pass = 0; pass < kLayoutPasses; ++pass) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }
    window_.repaint();
    if (diagnostics_) {
        // The 01 §15 inspection, automated for every captured state (each tab, the busy panel).
        diagnostics_->layout.merge(name, describe_layout(window_));
    }
    const std::filesystem::path path = options_.output_dir / std::filesystem::path{std::u8string{
                                           reinterpret_cast<const char8_t*>(name.data()), name.size()}};
    if (const std::expected<void, std::string> saved = screenshotter_(window_, path); !saved) {
        window_.controller()->log(ui::strings::screenshot_failed(name, saved.error()));
    }
}

} // namespace wizz::app
