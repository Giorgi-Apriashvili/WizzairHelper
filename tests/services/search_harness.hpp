#pragma once
// Flight-search test harness (05 §3.7 `Service(responder)`, §3.11, §1.6
// RouteResponder, §1.8 synchronous sinks), shared by flight_search_service_test
// and search_regression_test.
//
// `SearchHarness` builds the three pieces of 05 §3.7, each with the FIXED clock
// unless a test says otherwise: a Wizz harness (05 §1.4: spacing 0, jitter 0,
// recording delay, cache disabled) over its own FakeTransport, a CurrencyService
// whose own fake transport answers every call with fx_frankfurter_v2.json (cache
// disabled), and FlightSearchService(client, fx, clock, en-US).

#include "wizz/core/catalog.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/services/currency_service.hpp"
#include "wizz/services/flight_search_service.hpp"
#include "wizz/services/wizz_client.hpp"

#include "support/fake_transport.hpp"
#include "support/recording_delay.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wizz::test::search {

/// 05 §2.7 B1: the BTS timetable (one row, 2026-11-12 07:15, 14.99 EUR).
inline constexpr std::string_view kTimetableB1 =
    R"({"outboundFlights":[{"departureStation":"BTS","arrivalStation":"LTN","departureDate":"2026-11-12T07:15:00",)"
    R"("price":{"amount":14.99,"currencyCode":"EUR"},"priceType":"price","departureDates":[{"date":)"
    R"("2026-11-12T07:15:00","isCheapestOfTheDay":true}],"hasMacFlight":false}],"returnFlights":null})";
/// 05 §2.7 short bodies.
inline constexpr std::string_view kInvalidMarketBody = R"({"validationCodes":["InvalidMarket"]})";
inline constexpr std::string_view kEmptyTimetableBody = R"({"outboundFlights":[],"returnFlights":null})";

/// D8 (01 §18.5): the Kasada block message, the abort reason of CT-FSS-02.
inline constexpr std::string_view kKasadaBlocked =
    "Wizz Air's bot protection blocked the request (HTTP 429 challenge). Wait a few minutes and try again.";

/// The request is a timetableV2 POST.
[[nodiscard]] bool is_timetable_request(const RecordedRequest& request);
/// `flightList[0].departureStation` / `arrivalStation` of a timetable request body ("" when absent).
[[nodiscard]] std::string departure_station(const RecordedRequest& request);
[[nodiscard]] std::string arrival_station(const RecordedRequest& request);

/// `RouteResponder` (05 §1.6): non-timetable URLs → the default responder; a
/// timetable departing BUD → timetable_v2_bud_ltn.json, BTS → B1, anything else →
/// 400 `{"validationCodes":["InvalidMarket"]}`.
[[nodiscard]] Responder route_responder();

/// Every FX call → 200 JSON fx_frankfurter_v2.json.
[[nodiscard]] Responder frankfurter_responder();

/// One synthetic timetable row for `timetable_json`.
struct FareRow {
    std::string from;
    std::string to;
    std::string departure;           ///< "2026-11-15T06:00:00"
    std::string amount;              ///< JSON number lexeme, e.g. "60.0"
    std::string currency = "EUR";    ///< empty = no currencyCode member
    bool check_on_site = false;      ///< priceType "checkPrice" (amount should be "0.0")
};
/// A timetableV2 response body holding `rows` in order.
[[nodiscard]] std::string timetable_json(std::span<const FareRow> rows);

struct SearchHarnessOptions {
    /// The FIXED clock shared by the client, the service and (by default) the FX
    /// service; nullptr = a new FixedClock() at 2026-11-02T10:00:00Z, local zone UTC.
    std::shared_ptr<FixedClock> clock;
    /// Runs inside every recorded delay of the Wizz client (gate waits and back-offs).
    RecordingDelay::Hook on_delay;
    /// The CurrencyService clock; nullptr = the harness's FIXED clock (05 §3.11 uses SYSTEM).
    std::shared_ptr<const Clock> fx_clock;
    /// The FX transport's responder; empty = `frankfurter_responder()`.
    Responder fx_responder;
    /// Last-minute changes to the Wizz client options (e.g. spacing for R-01).
    std::function<void(WizzClientOptions&)> customize;
};

/// 05 §3.7 `Service(responder)`.
class SearchHarness {
public:
    explicit SearchHarness(Responder wizz_responder, SearchHarnessOptions options = {});

    std::shared_ptr<FixedClock> clock;
    RecordingDelay delay;
    std::shared_ptr<FakeTransport> wizz_transport;
    std::shared_ptr<WizzApiClient> wizz;
    std::shared_ptr<FakeTransport> fx_transport;
    std::shared_ptr<CurrencyService> fx;
    std::unique_ptr<FlightSearchService> service;

    /// `svc.Wizz.GetAirports()` (05 §3.7): warms the map and the API URL; the
    /// catalog is CAT. Also remembered for `place`.
    AirportCatalogPtr warm_map();
    /// `CAT.MatchPlace(text)`; throws std::logic_error when it does not match.
    [[nodiscard]] Place place(std::string_view text);

    /// The timetable request bodies sent so far, in order.
    [[nodiscard]] std::vector<std::string> timetable_bodies() const;
    /// The number of timetable requests sent so far.
    [[nodiscard]] std::size_t timetable_count() const;

private:
    AirportCatalogPtr catalog_;
};

/// Thread-safe progress collector (05 §1.8: a plain synchronous callback). It
/// also records the thread each report ran on.
class ProgressLog {
public:
    [[nodiscard]] ProgressSink sink();
    [[nodiscard]] std::vector<SearchProgress> entries() const;
    [[nodiscard]] std::vector<std::thread::id> threads() const;

private:
    mutable std::mutex mutex_;
    std::vector<SearchProgress> entries_;
    std::vector<std::thread::id> threads_;
};

/// Thread-safe partial-batch collector.
class BatchLog {
public:
    [[nodiscard]] PartialResultsSink sink();
    [[nodiscard]] std::vector<std::vector<FlightResultPtr>> batches() const;
    [[nodiscard]] std::vector<std::size_t> sizes() const;
    [[nodiscard]] std::size_t total_rows() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::vector<FlightResultPtr>> batches_;
};

/// "Stage / Detail / completed/total", plus " wait=<iso>" when WaitingUntilUtc is
/// set, " step=<iso>" (or " step=unset") and " user" when WaitingForUser is set.
[[nodiscard]] std::string describe(const SearchProgress& progress);
[[nodiscard]] std::vector<std::string> describe(std::span<const SearchProgress> progress);
/// "BTS→LTN 2026-11-12 07:15" (route, date, cheapest departure).
[[nodiscard]] std::string describe(const FlightResult& flight);
[[nodiscard]] std::vector<std::string> describe(std::span<const FlightResultPtr> flights);

/// A LegSearchRequest with the given places and window (other fields default).
[[nodiscard]] LegSearchRequest make_request(Place origin, double origin_radius_km, std::optional<Place> destination,
                                            double destination_radius_km, Date from, Date to, int adults = 1,
                                            std::string currency = "EUR");

/// `await Search(request)`: search_async(...).get() with optional collectors.
[[nodiscard]] LegSearchResultPtr run_search(FlightSearchService& service, const LegSearchRequest& request,
                                            ProgressLog* progress = nullptr, BatchLog* batches = nullptr,
                                            std::stop_token stop = {});

/// An exact decimal from its literal text (throws std::logic_error on a typo).
[[nodiscard]] Decimal dec(std::string_view lexeme);

} // namespace wizz::test::search
