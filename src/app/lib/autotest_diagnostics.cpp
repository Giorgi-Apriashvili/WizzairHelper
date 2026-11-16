#include "autotest_diagnostics.hpp"

#include "busy_panel.hpp"
#include "layout_check.hpp"
#include "main_window.hpp"
#include "modern_style.hpp"
#include "qt_strings.hpp"
#include "ui_style.hpp"

#include "wizz/core/json.hpp"

#include <QApplication>
#include <QFontInfo>
#include <QGuiApplication>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QSysInfo>
#include <QWindow>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace wizz::app {

namespace {

/// Milliseconds rounded to 0.1 for the report.
[[nodiscard]] double tenths(double value) noexcept
{
    return std::round(value * 10.0) / 10.0;
}

[[nodiscard]] std::optional<std::string> environment_variable(const char* name)
{
    if (!qEnvironmentVariableIsSet(name)) {
        return std::nullopt;
    }
    return to_utf8(qEnvironmentVariable(name));
}

void write_rect(json::Writer& writer, const QRect& rect)
{
    writer.begin_array().integer(rect.x()).integer(rect.y()).integer(rect.width()).integer(rect.height()).end_array();
}

void write_size(json::Writer& writer, const QSize& size)
{
    writer.begin_array().integer(size.width()).integer(size.height()).end_array();
}

void write_optional_string(json::Writer& writer, const std::optional<std::string>& value)
{
    if (value) {
        writer.string(*value);
    } else {
        writer.null();
    }
}

void write_strings(json::Writer& writer, const std::vector<std::string>& values)
{
    writer.begin_array();
    for (const std::string& value : values) {
        writer.string(value);
    }
    writer.end_array();
}

void write_timer(json::Writer& writer, const TimerGapProbe::Stats& timer)
{
    writer.begin_object();
    writer.key("intervalMs").integer(ui::BusyPanelModel::kTimerInterval.count());
    writer.key("runs").integer(timer.runs);
    writer.key("ticks").integer(timer.ticks);
    writer.key("longestGapMs").number(tenths(timer.longest_gap_ms));
    writer.key("meanGapMs").number(tenths(timer.mean_gap_ms()));
    writer.key("gapsOver50Ms").integer(timer.gaps_over_50_ms);
    writer.key("gapsOver100Ms").integer(timer.gaps_over_100_ms);
    writer.key("longestGapStatus").string(timer.longest_gap_status);
    writer.key("longestGapCpuMs").number(tenths(timer.longest_gap_cpu_ms));
    writer.key("longestOwnGapMs").number(tenths(timer.longest_own_gap_ms));
    writer.key("heldGapsOver100Ms").integer(timer.held_gaps_over_100_ms);
    writer.key("harnessExclusions").integer(timer.exclusions);
    writer.key("harnessExcludedMs").number(tenths(timer.excluded_ms));
    writer.key("longestHarnessExclusionMs").number(tenths(timer.longest_exclusion_ms));
    writer.key("longestGaps").begin_array();
    for (const TimerGapProbe::Gap& gap : timer.longest_gaps) {
        writer.begin_object();
        writer.key("ms").number(tenths(gap.ms));
        writer.key("cpuMs").number(tenths(gap.cpu_ms));
        writer.key("heldByOthers").boolean(gap.held_by_others);
        writer.key("endedAtMs").number(tenths(gap.ended_at_ms));
        writer.key("status").string(gap.status);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
}

void write_events(json::Writer& writer, const std::optional<EventWatchdog::Stats>& events)
{
    if (!events) {
        writer.null();
        return;
    }
    writer.begin_object();
    writer.key("dispatches").integer(events->dispatches);
    writer.key("over50Ms").integer(events->over_50_ms);
    writer.key("over100Ms").integer(events->over_100_ms);
    writer.key("longestMs").number(tenths(events->longest_ms));
    writer.key("longestBusyMs").number(tenths(events->longest_busy_ms));
    writer.key("longestBusyCpuMs").number(tenths(events->longest_busy_cpu_ms));
    writer.key("longestBusyOwnMs").number(tenths(events->longest_busy_own_ms));
    writer.key("longest").begin_array();
    for (const EventWatchdog::Dispatch& dispatch : events->longest) {
        writer.begin_object();
        writer.key("ms").number(tenths(dispatch.ms));
        writer.key("cpuMs").number(tenths(dispatch.cpu_ms));
        writer.key("heldByOthers").boolean(dispatch.held_by_others);
        writer.key("endedAtMs").number(tenths(dispatch.ended_at_ms));
        writer.key("event").string(dispatch.event);
        writer.key("receiver").string(dispatch.receiver);
        writer.key("busy").boolean(dispatch.busy);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
}

void write_cancel(json::Writer& writer, const std::optional<CancelDiagnostics>& cancel)
{
    if (!cancel) {
        writer.null();
        return;
    }
    writer.begin_object();
    writer.key("afterMs").integer(cancel->after.count());
    writer.key("pressedAtMs").number(tenths(cancel->pressed_at_ms));
    writer.key("busyAtPress").boolean(cancel->busy_at_press);
    writer.key("statusBefore").string(cancel->status_before);
    writer.key("presses");
    write_strings(writer, cancel->presses);
    writer.key("cancelRequests").integer(cancel->cancel_requests);
    writer.key("statusAfterPress").string(cancel->status_after_press);
    writer.key("cancelEnabledAfterPress").boolean(cancel->cancel_enabled_after_press);
    writer.end_object();
}

} // namespace

void LayoutDiagnostics::merge(std::string_view state, const LayoutDiagnostics& check)
{
    cancel_visible = (checks == 0 || cancel_visible) && check.cancel_visible;
    checks += std::max(check.checks, 1);
    for (const std::string& problem : check.problems) {
        problems.push_back(std::format("{}: {}", state, problem));
    }
}

GridDiagnostics describe_grid(std::string name, std::span<const FlightResultPtr> rows, const LegSearchResult* result)
{
    GridDiagnostics grid{.name = std::move(name), .rows = rows.size()};
    std::unordered_set<const FlightResult*> in_result;
    if (result != nullptr) {
        grid.result_flights = result->flights.size();
        for (const FlightResultPtr& flight : result->flights) {
            in_result.insert(flight.get());
        }
    }
    std::set<std::tuple<std::string, std::string, Date>> keys;
    for (const FlightResultPtr& row : rows) {
        if (!in_result.contains(row.get())) {
            ++grid.rows_not_in_result;
        }
        if (!keys.emplace(row->fare.from, row->fare.to, row->fare.date).second) {
            ++grid.duplicate_rows;
        }
    }
    return grid;
}

DisplayDiagnostics describe_display(const MainWindow& window)
{
    DisplayDiagnostics display;
    display.platform = to_utf8(QGuiApplication::platformName());
    display.scale_factor = environment_variable("QT_SCALE_FACTOR");
    display.screen_scale_factors = environment_variable("QT_SCREEN_SCALE_FACTORS");
    const QScreen* screen = window.screen();
    if (screen != nullptr) {
        display.screen_name = to_utf8(screen->name());
        display.screen_geometry = screen->geometry();
        display.screen_available = screen->availableGeometry();
        display.logical_dpi = screen->logicalDotsPerInch();
        display.physical_dpi = screen->physicalDotsPerInch();
    }
    display.device_pixel_ratio = window.devicePixelRatioF();
    display.window_frame = window.frameGeometry();
    display.window_client = window.geometry();
    display.outer_device_size = window.normal_outer_device_size();
    display.minimum_window = MainWindow::kMinimumSize;
    display.window_on_screen = screen != nullptr && display.screen_available.contains(display.window_frame);
    return display;
}

LayoutDiagnostics describe_layout(const MainWindow& window)
{
    LayoutDiagnostics layout;
    layout.checks = 1;
    layout.problems = layout_problems(window);
    const QPushButton* cancel = window.busy_panel()->cancel_button();
    layout.cancel_visible = cancel != nullptr && is_fully_visible(*cancel);
    return layout;
}

EnvironmentDiagnostics describe_environment(const Locale& locale)
{
    EnvironmentDiagnostics environment;
    environment.os = to_utf8(QSysInfo::prettyProductName());
    environment.kernel = to_utf8(QSysInfo::kernelVersion());
    environment.qt = qVersion();
    environment.locale = locale.name();
    if (const auto* modern = dynamic_cast<const ModernStyle*>(QApplication::style()); modern != nullptr) {
        environment.style = "modern/" + to_utf8(modern->baseStyle()->name());  // a proxy has no name of its own
    } else if (const QStyle* other = QApplication::style(); other != nullptr) {
        environment.style = to_utf8(other->name());
    }
    switch (style::appearance()) {
    case style::Appearance::light:
        environment.look = "light";
        break;
    case style::Appearance::dark:
        environment.look = "dark";
        break;
    case style::Appearance::high_contrast:
        environment.look = "high_contrast";
        break;
    }
    environment.window_colour = to_utf8(QApplication::palette().color(QPalette::Window).name());
    environment.font_family = to_utf8(QFontInfo(QApplication::font()).family());
    return environment;
}

std::string to_json(const AutoTestDiagnostics& diagnostics)
{
    json::Writer writer{json::Layout::indented};
    writer.begin_object();
    writer.key("timer");
    write_timer(writer, diagnostics.timer);
    writer.key("uiEvents");
    write_events(writer, diagnostics.events);
    writer.key("cancel");
    write_cancel(writer, diagnostics.cancel);

    writer.key("searches").begin_array();
    for (const SearchDiagnostics& search : diagnostics.searches) {
        writer.begin_object();
        writer.key("name").string(search.name);
        writer.key("wizzRequests").integer(search.wizz_requests);
        writer.key("durationMs").number(tenths(search.duration_ms));
        writer.key("finalStatus").string(search.final_status);
        writer.end_object();
    }
    writer.end_array();

    writer.key("grids").begin_object();
    for (const GridDiagnostics& grid : diagnostics.grids) {
        writer.key(grid.name).begin_object();
        writer.key("rows").integer(static_cast<std::int64_t>(grid.rows));
        writer.key("resultFlights");
        if (grid.result_flights) {
            writer.integer(static_cast<std::int64_t>(*grid.result_flights));
        } else {
            writer.null();
        }
        writer.key("rowsNotInResult").integer(static_cast<std::int64_t>(grid.rows_not_in_result));
        writer.key("duplicateRows").integer(static_cast<std::int64_t>(grid.duplicate_rows));
        writer.end_object();
    }
    writer.end_object();

    const DisplayDiagnostics& display = diagnostics.display;
    writer.key("display").begin_object();
    writer.key("platform").string(display.platform);
    writer.key("qtScaleFactor");
    write_optional_string(writer, display.scale_factor);
    writer.key("qtScreenScaleFactors");
    write_optional_string(writer, display.screen_scale_factors);
    writer.key("screen").string(display.screen_name);
    writer.key("screenGeometry");
    write_rect(writer, display.screen_geometry);
    writer.key("screenAvailable");
    write_rect(writer, display.screen_available);
    writer.key("devicePixelRatio").number(display.device_pixel_ratio);
    writer.key("logicalDpi").number(tenths(display.logical_dpi));
    writer.key("physicalDpi").number(tenths(display.physical_dpi));
    writer.key("windowFrame");
    write_rect(writer, display.window_frame);
    writer.key("windowClient");
    write_rect(writer, display.window_client);
    writer.key("outerDeviceSize");
    write_size(writer, display.outer_device_size);
    writer.key("minimumWindow");
    write_size(writer, display.minimum_window);
    writer.key("windowOnScreen").boolean(display.window_on_screen);
    writer.end_object();

    writer.key("layout").begin_object();
    writer.key("statesChecked").integer(diagnostics.layout.checks);
    writer.key("problems");
    write_strings(writer, diagnostics.layout.problems);
    writer.key("cancelVisible").boolean(diagnostics.layout.cancel_visible);
    writer.end_object();

    const EnvironmentDiagnostics& environment = diagnostics.environment;
    writer.key("environment").begin_object();
    writer.key("os").string(environment.os);
    writer.key("kernel").string(environment.kernel);
    writer.key("qt").string(environment.qt);
    writer.key("locale").string(environment.locale);
    writer.key("style").string(environment.style);
    writer.key("look").string(environment.look);
    writer.key("windowColour").string(environment.window_colour);
    writer.key("fontFamily").string(environment.font_family);
    writer.end_object();

    writer.end_object();
    return writer.take();
}

} // namespace wizz::app
