#include "autotest_runner.hpp"

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

#include <QCoreApplication>
#include <QTimer>

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

} // namespace

AutoTestRunner::AutoTestRunner(MainWindow& window, ui::AutoTestOptions options, QObject* parent)
    : QObject(parent), window_(window), options_(std::move(options)), screenshotter_(&save_window_png)
{
}

std::filesystem::path AutoTestRunner::summary_path() const
{
    return options_.output_dir / kSummaryFileName;
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
        shot_timer.start();
        co_await controller.search_outbound();
        summary.outbound = ui::describe(controller.outbound_result().get(), window_.outbound_grid()->model()->state(),
                                        busy.updates_received(), locale);
        screenshot("1-outbound.png");

        if (options_.runs_return()) {
            FlightGrid& outbound = *window_.outbound_grid();
            if (options_.return_from_top && !outbound.shown_results().empty()) {
                FlightResultPtr top = outbound.selected();
                if (!top) {
                    top = outbound.shown_results().front();
                }
                co_await controller.plan_return_from(std::move(top));
            } else {
                co_await controller.search_return();
            }
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

    summary.progress_screenshots = std::move(progress_snapshots);
    summary.final_status = busy.status_text();
    summary.requests_to_wizz = services.wizz->request_count();
    summary.log = ui::split_log(window_.log_view()->text());
    write_text_file(summary_path(), ui::to_json(summary));
    summary_ = std::move(summary);
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

void AutoTestRunner::screenshot(std::string_view name)
{
    // WinForms lays out synchronously when a label's text changes; Qt posts a layout
    // request. Apply the pending layouts (and nothing else) so the capture shows the
    // window as it is now, e.g. the busy panel's columns after End(). A wrapped label that
    // grows re-lays out its parents one posted request per level, hence the few passes.
    for (int pass = 0; pass < kLayoutPasses; ++pass) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }
    window_.repaint();
    const std::filesystem::path path = options_.output_dir / std::filesystem::path{std::u8string{
                                           reinterpret_cast<const char8_t*>(name.data()), name.size()}};
    if (const std::expected<void, std::string> saved = screenshotter_(window_, path); !saved) {
        window_.controller()->log(ui::strings::screenshot_failed(name, saved.error()));
    }
}

} // namespace wizz::app
