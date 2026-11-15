#pragma once
// summary.json of the --autotest script (01 §17.2, §17.3): plain input structs,
// builders from the result and the grid models (`Describe`, `DescribeTrips`),
// and the writer with the exact key order:
//   startedAt, apiUrl, airports, outbound, [return, roundTrips],
//   [afterCurrencySwitch], [exception], progressScreenshots, finalStatus,
//   requestsToWizz, log
// Indented two spaces, JsonSerializer escaping (non-ASCII and HTML-sensitive
// characters as \uXXXX); consumers compare the parsed JSON.

#include "wizz/ui/flight_rows.hpp"
#include "wizz/ui/round_trip_rows.hpp"

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wizz::ui {

/// `Describe(r, grid, progressUpdates)` for a leg with a result (01 §17.3).
struct LegJson {
    std::size_t flights = 0;                     ///< r.Flights.Count
    std::size_t grid_rows = 0;                   ///< rows in the grid's current view
    std::size_t requests_planned = 0;            ///< r.Plan.Requests.Count
    int requests_completed = 0;
    bool cancelled = false;
    std::optional<std::string> abort_reason;     ///< null when none
    double elapsed_seconds = 0;                  ///< Math.Round(Elapsed.TotalSeconds, 1)
    int progress_updates = 0;                    ///< BusyPanel.UpdatesReceived (0 after a currency switch)
    std::vector<std::string> origin_airports;    ///< "BUD 0km"
    std::vector<std::string> destination_airports;
    std::vector<std::string> warnings;
    std::string fx;                              ///< "{Source} {yyyy-MM-dd}"
    std::string view;                            ///< "AllFares" / "CheapestPerDay" / "CheapestPerDestination"
    std::string current_row;                     ///< CurrentRowText
    std::vector<std::string> top_rows;           ///< VisibleRowsText(8)

    friend bool operator==(const LegJson&, const LegJson&) = default;
};
/// `{ "error": "no result" }`.
struct NoResult {
    friend bool operator==(const NoResult&, const NoResult&) = default;
};
using LegDescription = std::variant<NoResult, LegJson>;

/// `DescribeTrips()`.
struct TripsJson {
    std::size_t count = 0;
    std::vector<std::string> top;  ///< up to 5 trips
    std::string summary;           ///< the Round trips summary label

    friend bool operator==(const TripsJson&, const TripsJson&) = default;
};

/// `afterCurrencySwitch`.
struct CurrencySwitchJson {
    std::string currency;
    LegDescription outbound;
    TripsJson round_trips;
};

struct AutoTestSummary {
    std::string started_at;                            ///< local "yyyy-MM-ddTHH:mm:ss"
    std::optional<std::string> api_url;                ///< CurrentApiUrl or null
    std::optional<std::size_t> airports;               ///< RealAirports.Count or null
    std::optional<LegDescription> outbound;            ///< absent if the script failed before it
    std::optional<LegDescription> inbound;             ///< key "return"
    std::optional<TripsJson> round_trips;              ///< key "roundTrips"
    std::optional<CurrencySwitchJson> after_currency_switch;
    std::optional<std::string> exception;              ///< ex.ToString()
    std::vector<std::string> progress_screenshots;     ///< busy status text at each progress shot
    std::string final_status;                          ///< BusyPanel.StatusText
    std::int64_t requests_to_wizz = 0;                 ///< WizzApiClient.RequestCount
    std::vector<std::string> log;                      ///< log text split on CRLF, empty lines removed
};

/// `topRows` length.
inline constexpr std::size_t kSummaryTopRows = 8;
/// `DescribeTrips().top` length.
inline constexpr std::size_t kSummaryTopTrips = 5;

/// `Math.Round(Elapsed.TotalSeconds, 1)`: ties to even on the double (07 §2 "autotest elapsed values").
[[nodiscard]] double elapsed_seconds_value(Duration elapsed) noexcept;
/// "{IATA} {km:0}km" (no space before km; label rounding).
[[nodiscard]] std::string summary_airport(const AreaAirport& airport, const Locale& locale);
/// `startedAt`: "yyyy-MM-ddTHH:mm:ss" of the local now.
[[nodiscard]] std::string started_at_text(LocalDateTime now);
/// `Describe(r, grid, progressUpdates)`; `result` null → NoResult.
[[nodiscard]] LegDescription describe(const LegSearchResult* result, const FlightGridModel& grid, int progress_updates,
                                      const Locale& locale);
/// One `top` entry: "{Total:0.##} {CUR}: {outDate:yyyy-MM-dd} {OUT_FROM}->{OUT_TO} + {retDate:yyyy-MM-dd}
/// {RET_FROM}->{RET_TO} ({Nights} nights)".
[[nodiscard]] std::string describe_trip(const RoundTrip& trip, std::string_view currency, const Locale& locale);
/// `DescribeTrips()` over the grid's trips (combiner order) and summary.
[[nodiscard]] TripsJson describe_trips(const RoundTripGridModel& grid, const Locale& locale);
/// The Log text split on CRLF with empty lines removed.
[[nodiscard]] std::vector<std::string> split_log(std::string_view log_text);
/// The summary.json document.
[[nodiscard]] std::string to_json(const AutoTestSummary& summary);

} // namespace wizz::ui
