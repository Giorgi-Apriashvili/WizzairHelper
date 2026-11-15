#include "wizz/ui/summary_json.hpp"

#include "wizz/ui/strings.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/format.hpp"
#include "wizz/core/json.hpp"

#include <algorithm>
#include <format>

namespace wizz::ui {

namespace {

constexpr double kTicksPerSecond = 10'000'000.0;

void write_strings(json::Writer& writer, const std::vector<std::string>& values)
{
    writer.begin_array();
    for (const std::string& value : values) {
        writer.string(value);
    }
    writer.end_array();
}

void write_optional_string(json::Writer& writer, const std::optional<std::string>& value)
{
    if (value) {
        writer.string(*value);
    } else {
        writer.null();
    }
}

void write_leg(json::Writer& writer, const LegDescription& description)
{
    if (std::holds_alternative<NoResult>(description)) {
        writer.begin_object().key("error").string(strings::kNoResult).end_object();
        return;
    }
    const LegJson& leg = std::get<LegJson>(description);
    writer.begin_object();
    writer.key("flights").integer(static_cast<std::int64_t>(leg.flights));
    writer.key("gridRows").integer(static_cast<std::int64_t>(leg.grid_rows));
    writer.key("requestsPlanned").integer(static_cast<std::int64_t>(leg.requests_planned));
    writer.key("requestsCompleted").integer(leg.requests_completed);
    writer.key("cancelled").boolean(leg.cancelled);
    writer.key("abortReason");
    write_optional_string(writer, leg.abort_reason);
    writer.key("elapsedSeconds").number(leg.elapsed_seconds);
    writer.key("progressUpdates").integer(leg.progress_updates);
    writer.key("originAirports");
    write_strings(writer, leg.origin_airports);
    writer.key("destinationAirports");
    write_strings(writer, leg.destination_airports);
    writer.key("warnings");
    write_strings(writer, leg.warnings);
    writer.key("fx").string(leg.fx);
    writer.key("view").string(leg.view);
    writer.key("currentRow").string(leg.current_row);
    writer.key("topRows");
    write_strings(writer, leg.top_rows);
    writer.end_object();
}

void write_trips(json::Writer& writer, const TripsJson& trips)
{
    writer.begin_object();
    writer.key("count").integer(static_cast<std::int64_t>(trips.count));
    writer.key("top");
    write_strings(writer, trips.top);
    writer.key("summary").string(trips.summary);
    writer.end_object();
}

} // namespace

double elapsed_seconds_value(Duration elapsed) noexcept
{
    const double seconds = static_cast<double>(elapsed.count()) / kTicksPerSecond;
    return round_half_even(seconds * 10.0) / 10.0;
}

std::string summary_airport(const AreaAirport& airport, const Locale& locale)
{
    return std::format("{} {}km", airport.airport ? std::string_view{airport.airport->iata} : std::string_view{},
                       format_label(airport.distance_km, 0, locale.number_symbols()));
}

std::string started_at_text(LocalDateTime now)
{
    return format_iso_seconds(now);
}

LegDescription describe(const LegSearchResult* result, const FlightGridModel& grid, int progress_updates,
                        const Locale& locale)
{
    if (!result) {
        return NoResult{};
    }
    LegJson leg;
    leg.flights = result->flights.size();
    leg.grid_rows = grid.row_count();
    leg.requests_planned = result->plan.requests.size();
    leg.requests_completed = result->requests_completed;
    leg.cancelled = result->cancelled;
    leg.abort_reason = result->abort_reason;
    leg.elapsed_seconds = elapsed_seconds_value(result->elapsed);
    leg.progress_updates = progress_updates;
    for (const AreaAirport& airport : result->plan.origin_airports) {
        leg.origin_airports.push_back(summary_airport(airport, locale));
    }
    for (const AreaAirport& airport : result->plan.destination_airports) {
        leg.destination_airports.push_back(summary_airport(airport, locale));
    }
    leg.warnings = result->warnings;
    if (result->rates) {
        leg.fx = std::format("{} {}", result->rates->source, result->rates->date.iso());
    }
    leg.view = std::string{view_mode_name(grid.mode())};
    leg.current_row = grid.current_row_text();
    leg.top_rows = grid.visible_rows_text(kSummaryTopRows);
    return leg;
}

std::string describe_trip(const RoundTrip& trip, std::string_view currency, const Locale& locale)
{
    const std::string total = trip.total ? trip.total->format("0.##", locale.number_symbols()) : std::string{};
    const auto iata = [](const AirportPtr& airport, const std::string& fallback) -> std::string_view {
        return airport ? std::string_view{airport->iata} : std::string_view{fallback};
    };
    return std::format("{} {}: {} {}->{} + {} {}->{} ({} nights)", total, currency, trip.outbound->date().iso(),
                       iata(trip.outbound->from_airport, trip.outbound->fare.from),
                       iata(trip.outbound->to_airport, trip.outbound->fare.to), trip.inbound->date().iso(),
                       iata(trip.inbound->from_airport, trip.inbound->fare.from),
                       iata(trip.inbound->to_airport, trip.inbound->fare.to), trip.nights);
}

TripsJson describe_trips(const RoundTripGridModel& grid, const Locale& locale)
{
    TripsJson trips;
    trips.count = grid.count();
    const std::span<const RoundTrip> all = grid.trips();
    for (std::size_t i = 0; i < std::min(all.size(), kSummaryTopTrips); ++i) {
        trips.top.push_back(describe_trip(all[i], grid.currency(), locale));
    }
    trips.summary = grid.summary_text();
    return trips;
}

std::vector<std::string> split_log(std::string_view log_text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= log_text.size()) {
        const std::size_t end = log_text.find("\r\n", start);
        const std::string_view line =
            log_text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!line.empty()) {
            lines.emplace_back(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 2;
    }
    return lines;
}

std::string to_json(const AutoTestSummary& summary)
{
    json::Writer writer{json::Layout::indented};
    writer.begin_object();
    writer.key("startedAt").string(summary.started_at);
    writer.key("apiUrl");
    write_optional_string(writer, summary.api_url);
    writer.key("airports");
    if (summary.airports) {
        writer.integer(static_cast<std::int64_t>(*summary.airports));
    } else {
        writer.null();
    }
    if (summary.outbound) {
        writer.key("outbound");
        write_leg(writer, *summary.outbound);
    }
    if (summary.inbound) {
        writer.key("return");
        write_leg(writer, *summary.inbound);
    }
    if (summary.round_trips) {
        writer.key("roundTrips");
        write_trips(writer, *summary.round_trips);
    }
    if (summary.after_currency_switch) {
        const CurrencySwitchJson& after = *summary.after_currency_switch;
        writer.key("afterCurrencySwitch").begin_object();
        writer.key("currency").string(after.currency);
        writer.key("outbound");
        write_leg(writer, after.outbound);
        writer.key("roundTrips");
        write_trips(writer, after.round_trips);
        writer.end_object();
    }
    if (summary.exception) {
        writer.key("exception").string(*summary.exception);
    }
    writer.key("progressScreenshots");
    write_strings(writer, summary.progress_screenshots);
    writer.key("finalStatus").string(summary.final_status);
    writer.key("requestsToWizz").integer(summary.requests_to_wizz);
    writer.key("log");
    write_strings(writer, summary.log);
    writer.end_object();
    return writer.take();
}

} // namespace wizz::ui
