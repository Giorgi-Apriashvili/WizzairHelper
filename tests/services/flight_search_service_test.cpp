// FlightSearchService (03 §5): 05 §3.7 CT-FSS-01..04, the 05 §7.1 search
// scenarios P-14..P-18, P-31, P-32, the 05 §7.3 responsiveness checks R-01 and
// R-02, the 03 §5.3 rules one by one (warnings order, catch order, the failure
// counter, the effective-date filter, de-duplication and batches, cancellation
// before and inside the loop, unexpected errors), LoadAirports (03 §5.10), the
// worker hop and the worker ownership.
//
// Harness: 05 §3.7 `Service(responder)` (search_harness.hpp) — the Wizz harness of
// 05 §1.4 (spacing 0, jitter 0, recording delay, FIXED clock 2026-11-02T10:00:00Z
// with local zone UTC, cache disabled), a CurrencyService over its own fake
// transport answering fx_frankfurter_v2.json, and FlightSearchService(client, fx,
// FIXED, en-US). Exception types are asserted exactly (05 §1.9).

#include "wizz/services/flight_search_service.hpp"

#include "search_harness.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/catalog.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/fare_days.hpp"
#include "wizz/core/fx_rates.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/pricing.hpp"
#include "wizz/core/result_views.hpp"
#include "wizz/services/file_cache.hpp"
#include "wizz/services/wizz_errors.hpp"
#include "wizz/services/wizz_protocol.hpp"

#include "support/fake_transport.hpp"
#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

namespace {

using namespace wizz::test::search;
using namespace std::chrono_literals;

using wizz::AirportCatalog;
using wizz::AirportCatalogPtr;
using wizz::Date;
using wizz::Decimal;
using wizz::Duration;
using wizz::FareStatus;
using wizz::FixedClock;
using wizz::FlightResult;
using wizz::FlightResultPtr;
using wizz::FlightSearchService;
using wizz::LegSearchRequest;
using wizz::LegSearchResult;
using wizz::LegSearchResultPtr;
using wizz::Locale;
using wizz::OperationCancelled;
using wizz::SearchProgress;
using wizz::UtcInstant;
using wizz::WizzApiError;
using wizz::WizzBlockedError;
using wizz::test::default_responder;
using wizz::test::html_response;
using wizz::test::json_response;
using wizz::test::read_fixture;
using wizz::test::RecordedRequest;
using wizz::test::Responder;
using wizz::test::ScriptedResponse;
using wizz::test::string_content_response;

using Strings = std::vector<std::string>;
using Sizes = std::vector<std::size_t>;

namespace fx = wizz::fx_rates;

// ---------------------------------------------------------------------------
// Expected texts, spelled out independently of the implementation
// ---------------------------------------------------------------------------

const UtcInstant kNow = FixedClock::kDefaultUtcNow;  // 2026-11-02T10:00:00Z
const std::string kStep = " step=2026-11-02T10:00:00Z";

const std::string kLoading = "Loading Wizz Air route map /  / 0/0" + kStep;
const std::string kRates = "Getting exchange rates /  / 0/0" + kStep;
const std::string kFrankfurterNotice = "Getting exchange rates / Downloading exchange rates (Frankfurter) / 0/0" + kStep;

const std::string kBudLondon = "BUD → London (LON)   10 Nov – 20 Nov";
const std::string kBtsLondon = "BTS → London (LON)   10 Nov – 20 Nov";
const std::string kDebLondon = "DEB → London (LON)   10 Nov – 20 Nov";

const std::string kNope = R"(Unexpected Wizz Air response (HTTP 404): {"message":"nope"})";

[[nodiscard]] constexpr Date day(int year, unsigned month, unsigned d) noexcept
{
    return Date::from_ymd(year, month, d);
}

[[nodiscard]] std::string fetching(std::string_view detail, int completed, int total)
{
    return std::format("Fetching fares / {} / {}/{}", detail, completed, total) + kStep;
}

[[nodiscard]] std::string final_stage(std::string_view stage, int completed, int total)
{
    return std::format("{} /  / {}/{}", stage, completed, total) + kStep;
}

/// 04 §9.3 body (05 CT-API-01 [D]) for one window.
[[nodiscard]] std::string body(std::string_view from, std::string_view to, std::string_view start,
                               std::string_view end, int adults)
{
    return std::format(R"({{"flightList":[{{"departureStation":"{}","arrivalStation":"{}","from":"{}","to":"{}"}}],)"
                       R"("priceType":"regular","adultCount":{},"childCount":0,"infantCount":0}})",
                       from, to, start, end, adults);
}

[[nodiscard]] std::vector<std::optional<Decimal>> totals(std::span<const FlightResultPtr> flights)
{
    std::vector<std::optional<Decimal>> values;
    for (const FlightResultPtr& flight : flights) {
        values.push_back(flight->total_converted);
    }
    return values;
}

[[nodiscard]] std::vector<std::optional<Decimal>> per_person(std::span<const FlightResultPtr> flights)
{
    std::vector<std::optional<Decimal>> values;
    for (const FlightResultPtr& flight : flights) {
        values.push_back(flight->price_per_person_converted);
    }
    return values;
}

[[nodiscard]] std::vector<std::optional<Decimal>> decs(std::initializer_list<std::string_view> lexemes)
{
    std::vector<std::optional<Decimal>> values;
    for (const std::string_view lexeme : lexemes) {
        values.emplace_back(lexeme.empty() ? std::optional<Decimal>{} : std::optional<Decimal>{dec(lexeme)});
    }
    return values;
}

/// Runs `action` and returns the exception it threw (nullptr if none).
template <class Action>
[[nodiscard]] std::exception_ptr thrown_by(Action&& action)
{
    try {
        std::forward<Action>(action)();
    } catch (...) {
        return std::current_exception();
    }
    return nullptr;
}

/// `Assert.ThrowsAsync<T>` (05 §1.9): the dynamic type is exactly T. Returns the
/// message, or "" after a failure.
template <class Expected>
[[nodiscard]] std::string expect_exactly(const std::exception_ptr& error)
{
    if (!error) {
        ADD_FAILURE() << "nothing was thrown, expected " << typeid(Expected).name();
        return {};
    }
    try {
        std::rethrow_exception(error);
    } catch (const Expected& e) {
        EXPECT_EQ(typeid(e), typeid(Expected)) << "a subclass was thrown: " << typeid(e).name();
        return e.what();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "unexpected exception " << typeid(e).name() << ": " << e.what();
    } catch (...) {
        ADD_FAILURE() << "unexpected non-standard exception";
    }
    return {};
}

/// The departure stations of the timetable requests sent so far.
[[nodiscard]] Strings timetable_departures(const SearchHarness& harness)
{
    Strings stations;
    for (const RecordedRequest& request : harness.wizz_transport->requests()) {
        if (is_timetable_request(request)) {
            stations.push_back(departure_station(request));
        }
    }
    return stations;
}

/// Budapest r200 → London r`london_radius`, 10–20 Nov 2026 (CT-FSS-01 / 03 §11 shape).
[[nodiscard]] LegSearchRequest budapest_to_london(SearchHarness& harness, double london_radius, int adults,
                                                  std::string currency = "EUR")
{
    return make_request(harness.place("Budapest"), 200, harness.place("London"), london_radius, day(2026, 11, 10),
                        day(2026, 11, 20), adults, std::move(currency));
}

/// Budapest r0 → London (default r100), 10–20 Nov 2026: one request BUD→LGW [LON].
[[nodiscard]] LegSearchRequest budapest_r0_to_london(SearchHarness& harness, std::string currency = "EUR")
{
    return make_request(harness.place("Budapest"), 0, harness.place("London"), 100, day(2026, 11, 10),
                        day(2026, 11, 20), 1, std::move(currency));
}

/// A responder that answers timetable calls with `timetable` and everything else with the default responder.
[[nodiscard]] Responder timetable_responder(std::function<ScriptedResponse(const RecordedRequest&)> timetable)
{
    return [fallback = default_responder(), timetable = std::move(timetable)](const RecordedRequest& request) {
        return is_timetable_request(request) ? timetable(request) : fallback(request);
    };
}

// ===========================================================================
// 05 §3.7 CT-FSS-01..04
// ===========================================================================

TEST(FlightSearchService, CT_FSS_01_Search_ranks_fares_from_all_nearby_airports_in_one_currency)
{
    SearchHarness harness{route_responder()};
    harness.warm_map();
    ProgressLog progress;
    BatchLog batches;

    const LegSearchRequest request = budapest_to_london(harness, 30, 2);
    const LegSearchResultPtr result = run_search(*harness.service, request, &progress, &batches);
    ASSERT_NE(result, nullptr);

    // [A]
    EXPECT_FALSE(result->cancelled);
    EXPECT_EQ(result->abort_reason, std::nullopt);
    ASSERT_EQ(result->flights.size(), 12u);
    EXPECT_EQ(batches.total_rows(), 12u);
    EXPECT_EQ(result->routes_not_served, 1);

    const FlightResult& first = *result->flights[0];
    EXPECT_EQ(first.from_airport->iata, "BTS");
    EXPECT_EQ(first.price_per_person_converted, dec("14.99"));
    EXPECT_EQ(first.total_converted, dec("29.98"));
    EXPECT_GE(first.from_distance_km, 170.0);
    EXPECT_LE(first.from_distance_km, 175.0);
    EXPECT_TRUE(first.booking_url.contains("/BTS/LTN/2026-11-12/null/2/0/0/null"));

    const auto first_bud = std::ranges::find_if(result->flights, [](const FlightResultPtr& flight) {
        return flight->from_airport->iata == "BUD";
    });
    ASSERT_NE(first_bud, result->flights.end());
    EXPECT_EQ((*first_bud)->fare.price_per_person.currency, "HUF");
    EXPECT_EQ((*first_bud)->price_per_person_converted, dec("52.66"));  // round(19290 / 366.3, 2)

    EXPECT_TRUE(std::ranges::is_sorted(result->flights, std::less<>{}, [](const FlightResultPtr& flight) {
        return *flight->total_converted;
    }));

    const std::vector<SearchProgress> entries = progress.entries();
    ASSERT_FALSE(entries.empty());
    EXPECT_TRUE(std::ranges::any_of(entries, [](const SearchProgress& entry) {
        return entry.stage == "Fetching fares" && entry.total == 3;
    }));
    EXPECT_EQ(entries.back().stage, "Done");
    EXPECT_EQ(entries.back().completed, 3);

    // [D] request bodies, batches, warnings.
    EXPECT_EQ(harness.timetable_bodies(), (Strings{body("BUD", "LGW", "2026-11-10", "2026-11-20", 2),
                                                   body("BTS", "LTN", "2026-11-10", "2026-11-20", 2),
                                                   body("DEB", "LTN", "2026-11-10", "2026-11-20", 2)}));
    EXPECT_EQ(batches.sizes(), (Sizes{11, 1}));
    EXPECT_TRUE(result->warnings.empty());
    EXPECT_EQ(result->requests_completed, 3);

    // [D] progress sequence: StepStartedUtc 10:00:00Z, no WaitingUntilUtc, WaitingForUser false.
    EXPECT_EQ(describe(entries), (Strings{
                                     kLoading,
                                     kRates,
                                     kFrankfurterNotice,
                                     fetching(kBudLondon, 0, 3),
                                     fetching(kBtsLondon, 1, 3),
                                     fetching(kDebLondon, 2, 3),
                                     final_stage("Done", 3, 3),
                                 }));
    for (const SearchProgress& entry : entries) {
        EXPECT_EQ(entry.step_started_utc, kNow);
        EXPECT_EQ(entry.waiting_until_utc, std::nullopt);
        EXPECT_FALSE(entry.waiting_for_user);
    }
    // Three spaces before the dates, U+2192 and U+2013 (03 §5.3), byte for byte.
    EXPECT_EQ(entries[3].detail, "BUD \xE2\x86\x92 London (LON)   10 Nov \xE2\x80\x93 20 Nov");

    // [D] flight order and values (per person / total in EUR).
    EXPECT_EQ(describe(result->flights), (Strings{
                                             "BTS→LTN 2026-11-12 07:15",
                                             "BUD→LTN 2026-11-14 06:00",
                                             "BUD→LTN 2026-11-18 06:00",
                                             "BUD→LTN 2026-11-19 12:50",
                                             "BUD→LTN 2026-11-20 06:00",
                                             "BUD→LTN 2026-11-11 06:00",
                                             "BUD→LTN 2026-11-12 06:00",
                                             "BUD→LTN 2026-11-13 12:50",
                                             "BUD→LTN 2026-11-17 06:00",
                                             "BUD→LTN 2026-11-16 06:00",
                                             "BUD→LTN 2026-11-10 06:00",
                                             "BUD→LGW 2026-11-15 08:30",
                                         }));
    EXPECT_EQ(per_person(result->flights), decs({"14.99", "52.66", "52.66", "52.66", "52.66", "58.94", "58.94",
                                                 "58.94", "58.94", "69.31", "84.06", "94.70"}));
    EXPECT_EQ(totals(result->flights), decs({"29.98", "105.32", "105.32", "105.32", "105.32", "117.88", "117.88",
                                             "117.88", "117.88", "138.62", "168.12", "189.40"}));
    for (const FlightResultPtr& flight : result->flights) {
        ASSERT_TRUE(flight->to_distance_km.has_value());
        const double expected = flight->fare.to == "LGW" ? 40.558 : 44.072;
        EXPECT_NEAR(*flight->to_distance_km, expected, 0.0005) << describe(*flight);
        EXPECT_FALSE(flight->outside_destination_radius) << describe(*flight);
        EXPECT_EQ(flight->target_currency, "EUR");
        EXPECT_EQ(flight->adults, 2);
    }

    // The result carries its context (03 §5.1).
    EXPECT_EQ(result->request.currency, "EUR");
    EXPECT_EQ(result->request.adults, 2);
    EXPECT_EQ(result->plan.requests.size(), 3u);
    ASSERT_NE(result->rates, nullptr);
    EXPECT_EQ(result->rates->source, "Frankfurter");
    EXPECT_EQ(result->elapsed, Duration::zero());  // the FIXED monotonic clock never moves

    // Callbacks ran on the worker thread, never on the caller's (03 §5.2).
    for (const std::thread::id id : progress.threads()) {
        EXPECT_NE(id, std::this_thread::get_id());
    }
}

TEST(FlightSearchService, CT_FSS_02_Blocked_response_aborts_but_keeps_results_so_far)
{
    const auto responder = [] {
        return [route = route_responder()](const RecordedRequest& request) {
            if (is_timetable_request(request) && request.body && request.body->contains(R"("BTS")")) {
                return string_content_response(429).with_header("x-kpsdk-r", "1-AA");
            }
            return route(request);
        };
    };

    {
        SearchHarness harness{responder()};
        harness.warm_map();
        const LegSearchRequest request = budapest_to_london(harness, 100, 1);
        // No sinks.
        const LegSearchResultPtr result = harness.service->search_async(request, {}, {}, {}).get();

        // [A]
        ASSERT_TRUE(result->abort_reason.has_value());
        EXPECT_EQ(result->flights.size(), 11u);
        EXPECT_TRUE(std::ranges::any_of(result->warnings, [](const std::string& warning) {
            return warning.contains("bot protection");
        }));
        // [D]: no per-request warning for a block; the abort reason is the last (and only) warning.
        EXPECT_EQ(*result->abort_reason, kKasadaBlocked);
        EXPECT_EQ(result->warnings, (Strings{std::string{kKasadaBlocked}}));
        EXPECT_EQ(result->requests_completed, 1);
        EXPECT_FALSE(result->cancelled);
        EXPECT_EQ(timetable_departures(harness), (Strings{"BUD", "BTS"}));  // DEB is never requested
        EXPECT_TRUE(harness.delay.delays().empty());                        // a block is never retried
    }
    {
        // [D] "The last progress entry would be Stage Stopped with 1/3."
        SearchHarness harness{responder()};
        harness.warm_map();
        ProgressLog progress;
        const LegSearchResultPtr result = run_search(*harness.service, budapest_to_london(harness, 100, 1), &progress);
        EXPECT_EQ(describe(progress.entries()), (Strings{
                                                    kLoading,
                                                    kRates,
                                                    kFrankfurterNotice,
                                                    fetching(kBudLondon, 0, 3),
                                                    fetching(kBtsLondon, 1, 3),
                                                    final_stage("Stopped", 1, 3),
                                                }));
        EXPECT_EQ(result->requests_completed, 1);
    }
}

TEST(FlightSearchService, CT_FSS_03_Cancellation_mid_search_returns_partial_results)
{
    std::stop_source cancel;
    std::atomic<int> timetable_calls{0};
    SearchHarness harness{[&cancel, &timetable_calls, route = route_responder()](const RecordedRequest& request) {
        ScriptedResponse response = route(request);
        if (is_timetable_request(request) && timetable_calls.fetch_add(1) == 0) {
            cancel.request_stop();  // after building its response
        }
        return response;
    }};
    harness.warm_map();
    ProgressLog progress;
    BatchLog batches;

    const LegSearchResultPtr result =
        run_search(*harness.service, budapest_to_london(harness, 100, 1), &progress, &batches, cancel.get_token());

    // [A]
    EXPECT_TRUE(result->cancelled);
    EXPECT_LT(result->requests_completed, 3);
    // [D] the BUD response already received is processed; the cancel is seen at the next request.
    EXPECT_EQ(result->flights.size(), 11u);
    EXPECT_EQ(result->requests_completed, 1);
    EXPECT_EQ(harness.timetable_count(), 1u);
    EXPECT_EQ(result->abort_reason, std::nullopt);
    EXPECT_TRUE(result->warnings.empty());
    EXPECT_EQ(batches.sizes(), (Sizes{11}));
    EXPECT_EQ(describe(progress.entries()), (Strings{
                                                kLoading,
                                                kRates,
                                                kFrankfurterNotice,
                                                fetching(kBudLondon, 0, 3),
                                                fetching(kBtsLondon, 1, 3),
                                                final_stage("Cancelled", 1, 3),
                                            }));
}

TEST(FlightSearchService, CT_FSS_04_Check_on_site_rows_have_no_converted_price_and_sort_last)
{
    // Pure: ParseTimetable → BuildResult(CAT, request, RATES, f) → SortByPrice.
    const AirportCatalogPtr cat = AirportCatalog::from_map_json(read_fixture("map_subset.json"), Locale::en_us());
    const wizz::FxRates rates = fx::parse_frankfurter_v2(read_fixture("fx_frankfurter_v2.json"));
    LegSearchRequest request;
    request.origin = *cat->match_place("Budapest");
    request.from = day(2026, 11, 2);
    request.to = day(2026, 11, 5);

    std::vector<FlightResultPtr> built;
    for (const wizz::DayFare& fare : wizz::protocol::parse_timetable(read_fixture("timetable_checkprice.json"))) {
        built.push_back(wizz::pricing::build_result(*cat, request, rates, fare));
    }
    const std::vector<FlightResultPtr> sorted = wizz::result_views::sort_by_price(built);

    // [A]
    ASSERT_EQ(sorted.size(), 4u);
    EXPECT_EQ(sorted.back()->fare.status, FareStatus::check_on_site);
    EXPECT_EQ(sorted.back()->total_converted, std::nullopt);
    // [D]
    EXPECT_EQ(describe(sorted), (Strings{"BUD→LTN 2026-11-05 06:00", "BUD→LGW 2026-11-04 08:30",
                                         "BUD→LTN 2026-11-03 17:30", "BUD→LTN 2026-11-02 17:30"}));
    EXPECT_EQ(totals(sorted), decs({"84.06", "105.08", "120.91", ""}));
    EXPECT_EQ(sorted.back()->price_per_person_converted, std::nullopt);
}

TEST(FlightSearchService, CT_FSS_04_Check_on_site_rows_sort_last_through_the_service)
{
    SearchHarness harness{default_responder("timetable_checkprice.json")};
    harness.warm_map();
    BatchLog batches;
    const LegSearchRequest request = make_request(harness.place("Budapest"), 0, harness.place("London"), 100,
                                                  day(2026, 11, 2), day(2026, 11, 5));
    const LegSearchResultPtr result = run_search(*harness.service, request, nullptr, &batches);

    EXPECT_EQ(harness.timetable_bodies(), (Strings{body("BUD", "LGW", "2026-11-02", "2026-11-05", 1)}));
    EXPECT_EQ(batches.sizes(), (Sizes{4}));
    EXPECT_EQ(describe(result->flights), (Strings{"BUD→LTN 2026-11-05 06:00", "BUD→LGW 2026-11-04 08:30",
                                                  "BUD→LTN 2026-11-03 17:30", "BUD→LTN 2026-11-02 17:30"}));
    EXPECT_EQ(totals(result->flights), decs({"84.06", "105.08", "120.91", ""}));
    EXPECT_EQ(result->flights.back()->fare.status, FareStatus::check_on_site);
    EXPECT_TRUE(result->warnings.empty());  // a CheckOnSite row is not a missing currency
}

// ===========================================================================
// 05 §7.1 P-14 FX warnings in a search (03 §5.8 items 2, 3, 5)
// ===========================================================================

TEST(FlightSearchService, P_14a_Offline_rates_are_flagged_approximate)
{
    SearchHarnessOptions options;
    options.fx_responder = [](const RecordedRequest&) -> ScriptedResponse {
        throw wizz::http::TransportError("offline");
    };
    SearchHarness harness{default_responder(), std::move(options)};
    harness.warm_map();
    ProgressLog progress;

    const LegSearchResultPtr result = run_search(*harness.service, budapest_r0_to_london(harness), &progress);

    EXPECT_EQ(result->warnings, (Strings{"Exchange rates are approximate (built-in table, 2026-11-02); converted "
                                         "prices may be off by a few percent."}));
    EXPECT_EQ(result->rates, fx::built_in_rates());  // the same instance
    ASSERT_EQ(result->flights.size(), 11u);
    EXPECT_EQ(result->flights[0]->price_per_person_converted, dec("52.66"));  // built-in HUF 366.3
    // The FX notices are forwarded during the rate stage (03 §5.4).
    EXPECT_EQ(describe(progress.entries()),
              (Strings{
                  kLoading,
                  kRates,
                  kFrankfurterNotice,
                  "Getting exchange rates / Downloading exchange rates (currency-api (jsDelivr)) / 0/0" + kStep,
                  "Getting exchange rates / Downloading exchange rates (currency-api (Cloudflare)) / 0/0" + kStep,
                  "Getting exchange rates / Exchange-rate services unreachable — using built-in approximate rates / 0/0" +
                      kStep,
                  fetching(kBudLondon, 0, 1),
                  final_stage("Done", 1, 1),
              }));
}

TEST(FlightSearchService, P_14b_Unsupported_target_currency_is_named_as_typed)
{
    for (const std::string currency : {"XYZ", "xYz"}) {
        SearchHarness harness{default_responder("timetable_v2_bud_ltn.json")};
        harness.warm_map();
        const LegSearchResultPtr result = run_search(*harness.service, budapest_r0_to_london(harness, currency));

        // Raw, not upper-cased; no per-fare W6 because the target itself is unsupported.
        EXPECT_EQ(result->warnings,
                  (Strings{std::format("No exchange rate for {}; converted prices are unavailable.", currency)}));
        ASSERT_EQ(result->flights.size(), 11u);
        for (const FlightResultPtr& flight : result->flights) {
            EXPECT_EQ(flight->target_currency, "XYZ");
            EXPECT_EQ(flight->total_converted, std::nullopt);
        }
        EXPECT_EQ(result->request.currency, currency);  // the request as passed in
    }
}

TEST(FlightSearchService, P_14c_Fare_currencies_missing_from_the_rates_are_named_once_in_first_seen_order)
{
    const std::vector<FareRow> rows{
        {.from = "BUD", .to = "LTN", .departure = "2026-11-10T06:00:00", .amount = "10.0", .currency = "QQQ"},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-11T06:00:00", .amount = "11.0", .currency = ""},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-12T06:00:00", .amount = "12.0", .currency = "qqq"},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-13T06:00:00", .amount = "13.0", .currency = "EUR"},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-14T06:00:00", .amount = "14.0", .currency = "ZZZ"},
    };
    SearchHarness harness{
        timetable_responder([json = timetable_json(rows)](const RecordedRequest&) { return json_response(json); })};
    harness.warm_map();

    const LegSearchResultPtr result = run_search(*harness.service, budapest_r0_to_london(harness));

    EXPECT_EQ(result->warnings,
              (Strings{"No exchange rate for QQQ; those fares are shown without a converted price.",
                       "No exchange rate for ; those fares are shown without a converted price.",
                       "No exchange rate for ZZZ; those fares are shown without a converted price."}));
    ASSERT_EQ(result->flights.size(), 5u);
    EXPECT_EQ(describe(*result->flights[0]), "BUD→LTN 2026-11-13 06:00");  // the only converted fare
    EXPECT_EQ(result->flights[0]->total_converted, dec("13.0"));
}

TEST(FlightSearchService, P_14c_A_fixture_in_an_unknown_currency_gives_one_warning)
{
    std::string json = read_fixture("timetable_v2_bud_ltn.json");
    for (std::size_t at = json.find("\"HUF\""); at != std::string::npos; at = json.find("\"HUF\"", at)) {
        json.replace(at, 5, "\"ZZZ\"");
    }
    SearchHarness harness{timetable_responder([json](const RecordedRequest&) { return json_response(json); })};
    harness.warm_map();

    const LegSearchResultPtr result = run_search(*harness.service, budapest_r0_to_london(harness));

    EXPECT_EQ(result->warnings, (Strings{"No exchange rate for ZZZ; those fares are shown without a converted price."}));
    EXPECT_EQ(result->flights.size(), 11u);
}

// ===========================================================================
// 05 §7.1 P-15 .. P-18
// ===========================================================================

TEST(FlightSearchService, P_15_Four_consecutive_failures_abort)
{
    SearchHarness harness{timetable_responder(
        [](const RecordedRequest&) { return json_response(R"({"message":"nope"})", 404); })};
    harness.warm_map();
    ProgressLog progress;

    const LegSearchRequest anywhere = make_request(harness.place("Budapest"), 200, std::nullopt, 100,
                                                   day(2026, 11, 10), day(2026, 11, 20));
    const LegSearchResultPtr result = run_search(*harness.service, anywhere, &progress);

    EXPECT_EQ(result->plan.requests.size(), 8u);
    EXPECT_EQ(harness.timetable_count(), 4u);
    EXPECT_EQ(result->requests_completed, 3);
    EXPECT_EQ(progress.entries().back().stage, "Stopped");
    EXPECT_EQ(describe(progress.entries().back()), final_stage("Stopped", 3, 8));
    const std::string abort = "Stopped after 4 failed requests in a row. Last error: " + kNope;
    EXPECT_EQ(result->abort_reason, abort);
    EXPECT_EQ(result->warnings,
              (Strings{
                  "3 route(s) skipped because their next flight is after your dates: BUD → Alghero (Sardinia): next "
                  "flight 07 Jun 2027; BUD → Naples: next flight 01 Dec 2026; BTS → Naples: next flight 03 Dec 2026.",
                  "BUD → Barcelona El Prat (BCN) (10 Nov–20 Nov): " + kNope,
                  "BUD → London (LON) (10 Nov–20 Nov): " + kNope,
                  "BUD → Katowice / Kraków (10 Nov–20 Nov): " + kNope,
                  "BUD → Tirana (TIA) (10 Nov–20 Nov): " + kNope,
                  abort,
              }));
    EXPECT_FALSE(result->cancelled);
}

TEST(FlightSearchService, P_16_Dedupe_keeps_one_row_per_key_and_reports_only_winners)
{
    // Budapest r200 → London r30 plans BUD→LGW, BTS→LTN, DEB→LTN. Rows answered for
    // another planned origin are kept and de-duplicated (03 §5.5 step 2).
    const std::vector<FareRow> bud{
        {.from = "BUD", .to = "LTN", .departure = "2026-11-15T06:00:00", .amount = "70.0"},  // A new
        {.from = "BUD", .to = "LGW", .departure = "2026-11-15T06:00:00", .amount = "60.0"},  // B new
        {.from = "BUD", .to = "LTN", .departure = "2026-11-11T06:00:00", .amount = "0.0", .check_on_site = true},  // C
        {.from = "BUD", .to = "LTN", .departure = "2026-11-12T06:00:00", .amount = "50.0"},  // D new
        {.from = "BUD", .to = "LTN", .departure = "2026-11-14T06:00:00", .amount = "40.0"},  // E new
        {.from = "BUD", .to = "LTN", .departure = "2026-11-14T06:00:00", .amount = "35.0"},  // E lower: replaces
        {.from = "BUD", .to = "LTN", .departure = "2026-11-14T06:00:00", .amount = "35.0"},  // E tie: kept out
    };
    const std::vector<FareRow> bts{
        {.from = "BUD", .to = "LTN", .departure = "2026-11-15T06:00:00", .amount = "60.0"},  // A lower: replaces
        {.from = "BUD", .to = "LTN", .departure = "2026-11-11T06:00:00", .amount = "80.0"},  // C available beats check
        {.from = "BUD", .to = "LTN", .departure = "2026-11-12T06:00:00", .amount = "50.0"},  // D tie: kept out
        {.from = "BUD", .to = "LTN", .departure = "2026-11-12T06:00:00", .amount = "55.0"},  // D higher: kept out
        {.from = "BTS", .to = "LTN", .departure = "2026-11-13T07:15:00", .amount = "30.0"},  // F new
        {.from = "BUD", .to = "LTN", .departure = "2026-11-10T06:00:00", .amount = "0.0", .check_on_site = true},  // G
    };
    const std::vector<FareRow> deb{
        {.from = "BUD", .to = "LTN", .departure = "2026-11-10T06:00:00", .amount = "90.0"},  // G available beats check
        {.from = "BTS", .to = "LTN", .departure = "2026-11-13T07:15:00", .amount = "0.0", .check_on_site = true},  // F
    };
    SearchHarness harness{timetable_responder(
        [bud = timetable_json(bud), bts = timetable_json(bts), deb = timetable_json(deb)](const RecordedRequest& r) {
            const std::string from = departure_station(r);
            return json_response(from == "BUD" ? bud : from == "BTS" ? bts : deb);
        })};
    harness.warm_map();
    BatchLog batches;

    const LegSearchResultPtr result = run_search(*harness.service, budapest_to_london(harness, 30, 1), nullptr, &batches);

    // Batches: every new or replacing row, in response order; ties and losers are not reported.
    EXPECT_EQ(batches.sizes(), (Sizes{6, 4, 1}));
    const auto all_batches = batches.batches();
    ASSERT_EQ(all_batches.size(), 3u);
    EXPECT_EQ(totals(all_batches[0]), decs({"70.0", "60.0", "", "50.0", "40.0", "35.0"}));
    EXPECT_EQ(totals(all_batches[1]), decs({"60.0", "80.0", "30.0", ""}));
    EXPECT_EQ(totals(all_batches[2]), decs({"90.0"}));

    // One row per key; the replaced A keeps its first position, so the A/B tie
    // (same total, date and departure) is broken in A's favour (03 §5.5 step 4).
    EXPECT_EQ(describe(result->flights), (Strings{
                                             "BTS→LTN 2026-11-13 07:15",
                                             "BUD→LTN 2026-11-14 06:00",
                                             "BUD→LTN 2026-11-12 06:00",
                                             "BUD→LTN 2026-11-15 06:00",
                                             "BUD→LGW 2026-11-15 06:00",
                                             "BUD→LTN 2026-11-11 06:00",
                                             "BUD→LTN 2026-11-10 06:00",
                                         }));
    EXPECT_EQ(totals(result->flights), decs({"30.0", "35.0", "50.0", "60.0", "60.0", "80.0", "90.0"}));
    for (const FlightResultPtr& flight : result->flights) {
        EXPECT_EQ(flight->fare.status, FareStatus::available) << describe(*flight);
    }
    // The tie on D kept the row that was there first (the BUD response's instance).
    EXPECT_EQ(result->flights[2], all_batches[0][3]);
    EXPECT_TRUE(result->warnings.empty());
}

TEST(FlightSearchService, P_17_Rows_outside_the_effective_window_are_dropped)
{
    SearchHarness harness{default_responder()};  // the full 11-row fixture whatever is asked
    harness.warm_map();
    BatchLog batches;
    const LegSearchRequest request = make_request(harness.place("Budapest"), 0, harness.place("London"), 100,
                                                  day(2026, 11, 12), day(2026, 11, 14));

    const LegSearchResultPtr result = run_search(*harness.service, request, nullptr, &batches);

    EXPECT_EQ(harness.timetable_bodies(), (Strings{body("BUD", "LGW", "2026-11-12", "2026-11-14", 1)}));
    EXPECT_EQ(describe(result->flights),
              (Strings{"BUD→LTN 2026-11-14 06:00", "BUD→LTN 2026-11-12 06:00", "BUD→LTN 2026-11-13 12:50"}));
    EXPECT_EQ(batches.sizes(), (Sizes{3}));
    EXPECT_TRUE(result->warnings.empty());
}

TEST(FlightSearchService, P_17_The_effective_window_starts_today_when_the_request_starts_earlier)
{
    // EffectiveFrom = max(From, today): rows before today are dropped too (03 §5.5 step 1).
    const std::vector<FareRow> rows{
        {.from = "BUD", .to = "LTN", .departure = "2026-11-01T06:00:00", .amount = "10.0"},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-02T06:00:00", .amount = "20.0"},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-04T06:00:00", .amount = "30.0"},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-05T06:00:00", .amount = "40.0"},
    };
    SearchHarness harness{
        timetable_responder([json = timetable_json(rows)](const RecordedRequest&) { return json_response(json); })};
    harness.warm_map();
    const LegSearchRequest request = make_request(harness.place("Budapest"), 0, harness.place("London"), 100,
                                                  day(2026, 10, 30), day(2026, 11, 4));

    const LegSearchResultPtr result = run_search(*harness.service, request);

    EXPECT_EQ(describe(result->flights), (Strings{"BUD→LTN 2026-11-02 06:00", "BUD→LTN 2026-11-04 06:00"}));
    EXPECT_EQ(result->warnings, (Strings{"Dates before today were skipped (searching from 2026-11-02)."}));
}

TEST(FlightSearchService, P_18_Arrivals_outside_the_destination_radius_are_flagged)
{
    SearchHarness harness{default_responder()};
    harness.warm_map();
    const LegSearchRequest request = make_request(harness.place("Budapest"), 0, harness.place("London Luton"), 0,
                                                  day(2026, 11, 10), day(2026, 11, 20));

    const LegSearchResultPtr result = run_search(*harness.service, request);

    const std::string note = "Served but outside your 0 km destination radius: London Gatwick (LGW) 82 km.";
    ASSERT_EQ(result->plan.requests.size(), 1u);
    EXPECT_EQ(result->plan.requests[0].from->iata, "BUD");
    EXPECT_EQ(result->plan.requests[0].query_to, "LTN");
    EXPECT_EQ(result->plan.requests[0].group_key, "LON");
    EXPECT_EQ(result->plan.notes, (Strings{note}));
    EXPECT_EQ(result->warnings, (Strings{note}));
    ASSERT_EQ(result->flights.size(), 11u);
    for (const FlightResultPtr& flight : result->flights) {
        const bool lgw = flight->fare.to == "LGW";
        EXPECT_EQ(flight->outside_destination_radius, lgw) << describe(*flight);
        if (lgw) {
            EXPECT_EQ(flight->fare.date, day(2026, 11, 15));
            ASSERT_TRUE(flight->to_distance_km.has_value());
            EXPECT_NEAR(*flight->to_distance_km, 81.74, 0.005);
        }
    }
}

// ===========================================================================
// 05 §7.1 P-31, P-32; 05 §7.3 R-01, R-02
// ===========================================================================

TEST(FlightSearchService, P_31_Progress_during_a_back_off_inside_a_search)
{
    std::atomic<int> timetable_calls{0};
    SearchHarness harness{timetable_responder(
        [&timetable_calls, json = read_fixture("timetable_v2_bud_ltn.json")](const RecordedRequest&) {
            return timetable_calls.fetch_add(1) == 0 ? html_response("busy", 503) : json_response(json);
        })};
    harness.warm_map();
    ProgressLog progress;

    const LegSearchResultPtr result = run_search(*harness.service, budapest_r0_to_london(harness), &progress);

    EXPECT_EQ(describe(progress.entries()),
              (Strings{
                  kLoading,
                  kRates,
                  kFrankfurterNotice,
                  fetching(kBudLondon, 0, 1),
                  "Fetching fares / Wizz Air is busy (HTTP 503) — retry 1 of 3 / 0/1 wait=2026-11-02T10:00:05Z" + kStep,
                  fetching("Retrying (1 of 3)", 0, 1),
                  final_stage("Done", 1, 1),
              }));
    EXPECT_EQ(progress.entries()[4].waiting_until_utc, kNow + 5s);
    EXPECT_EQ(result->flights.size(), 11u);
    EXPECT_EQ(harness.delay.delays(), (std::vector<Duration>{5s, 5s}));  // back-off, then the cooled-down gate
}

TEST(FlightSearchService, P_32_Cancel_during_a_back_off)
{
    std::stop_source cancel;
    SearchHarnessOptions options;
    options.on_delay = [&cancel](Duration) { cancel.request_stop(); };
    SearchHarness harness{timetable_responder([](const RecordedRequest&) { return html_response("busy", 503); }),
                          std::move(options)};
    harness.warm_map();  // spacing 0: the warm-up records no delay
    ProgressLog progress;

    const LegSearchResultPtr result =
        run_search(*harness.service, budapest_r0_to_london(harness), &progress, nullptr, cancel.get_token());

    EXPECT_TRUE(result->cancelled);
    EXPECT_EQ(result->requests_completed, 0);
    EXPECT_TRUE(result->flights.empty());
    EXPECT_TRUE(result->warnings.empty());
    EXPECT_EQ(result->abort_reason, std::nullopt);
    EXPECT_EQ(harness.timetable_count(), 1u);
    EXPECT_EQ(harness.delay.delays(), (std::vector<Duration>{5s}));
    EXPECT_EQ(describe(progress.entries()),
              (Strings{
                  kLoading,
                  kRates,
                  kFrankfurterNotice,
                  fetching(kBudLondon, 0, 1),
                  "Fetching fares / Wizz Air is busy (HTTP 503) — retry 1 of 3 / 0/1 wait=2026-11-02T10:00:05Z" + kStep,
                  final_stage("Cancelled", 0, 1),
              }));
}

/// A thread-safe ordered log of what happened, for the R-0x ordering checks.
class Timeline {
public:
    void add(std::string event)
    {
        const std::scoped_lock lock{mutex_};
        events_.push_back(std::move(event));
    }
    [[nodiscard]] Strings events() const
    {
        const std::scoped_lock lock{mutex_};
        return events_;
    }

private:
    mutable std::mutex mutex_;
    Strings events_;
};

TEST(FlightSearchService, R_01_One_report_per_request_before_it_starts_with_StepStartedUtc)
{
    // Spacing S = 1 s, jitter 0; the fake clock advances by every recorded delay.
    static constexpr Duration kSpacing = 1s;  // static: the lambdas below use it without capturing
    const auto clock = std::make_shared<FixedClock>();
    Timeline timeline;
    SearchHarnessOptions options;
    options.clock = clock;
    options.on_delay = [clock](Duration delay) { clock->advance(delay); };
    options.customize = [](wizz::WizzClientOptions& client) { client.request_spacing = kSpacing; };
    const Responder route = route_responder();
    SearchHarness harness{[&timeline, &route, clock](const RecordedRequest& request) {
                              if (is_timetable_request(request)) {
                                  timeline.add(std::format("request {} at {}", departure_station(request),
                                                           wizz::format_iso_utc(clock->utc_now())));
                              }
                              return route(request);
                          },
                          std::move(options)};
    harness.warm_map();  // discovery (first turn, no wait) and the map (waits S)
    const UtcInstant search_start = clock->utc_now();
    ASSERT_EQ(search_start, kNow + kSpacing);

    const wizz::ProgressSink sink = [&timeline, clock](const SearchProgress& progress) {
        if (progress.stage == "Fetching fares") {
            // The report's step start is "now" when it is reported.
            timeline.add(std::format("report {} {} step {} now {}", progress.completed, progress.detail.substr(0, 3),
                                     wizz::format_iso_utc(progress.step_started_utc.value_or(UtcInstant{})),
                                     wizz::format_iso_utc(clock->utc_now())));
        }
    };
    const LegSearchResultPtr result =
        harness.service->search_async(budapest_to_london(harness, 30, 2), sink, {}, {}).get();

    // K = 3 requests: each is preceded by its own report, and the silent gate wait
    // of S separates the report's StepStartedUtc from the request's start.
    EXPECT_EQ(timeline.events(), (Strings{
                                     "report 0 BUD step 2026-11-02T10:00:01Z now 2026-11-02T10:00:01Z",
                                     "request BUD at 2026-11-02T10:00:02Z",
                                     "report 1 BTS step 2026-11-02T10:00:02Z now 2026-11-02T10:00:02Z",
                                     "request BTS at 2026-11-02T10:00:03Z",
                                     "report 2 DEB step 2026-11-02T10:00:03Z now 2026-11-02T10:00:03Z",
                                     "request DEB at 2026-11-02T10:00:04Z",
                                 }));
    // Elapsed comes from the monotonic clock (06 §3): 3 gate waits of S since the start.
    EXPECT_EQ(result->elapsed, 3 * kSpacing);
    EXPECT_EQ(result->flights.size(), 12u);
}

TEST(FlightSearchService, R_02_Every_back_off_is_announced_with_WaitUntilUtc_before_it_sleeps)
{
    Timeline timeline;
    std::atomic<int> timetable_calls{0};
    SearchHarnessOptions options;
    options.on_delay = [&timeline](Duration delay) {
        timeline.add(std::format("delay {}s", std::chrono::duration_cast<std::chrono::seconds>(delay).count()));
    };
    SearchHarness harness{timetable_responder(
                              [&timetable_calls, json = read_fixture("timetable_v2_bud_ltn.json")](const RecordedRequest&) {
                                  return timetable_calls.fetch_add(1) < 3 ? html_response("down", 502) : json_response(json);
                              }),
                          std::move(options)};
    harness.warm_map();
    const wizz::ProgressSink sink = [&timeline](const SearchProgress& progress) {
        timeline.add(progress.detail +
                     (progress.waiting_until_utc ? " until " + wizz::format_iso_utc(*progress.waiting_until_utc) : ""));
    };

    const LegSearchResultPtr result =
        harness.service->search_async(budapest_r0_to_london(harness), sink, {}, {}).get();

    EXPECT_EQ(timeline.events(), (Strings{
                                     "",
                                     "",
                                     "Downloading exchange rates (Frankfurter)",
                                     kBudLondon,
                                     "Wizz Air is busy (HTTP 502) — retry 1 of 3 until 2026-11-02T10:00:05Z",
                                     "delay 5s",
                                     "Retrying (1 of 3)",
                                     "Wizz Air is busy (HTTP 502) — retry 2 of 3 until 2026-11-02T10:00:15Z",
                                     "delay 15s",
                                     "Retrying (2 of 3)",
                                     "Wizz Air is busy (HTTP 502) — retry 3 of 3 until 2026-11-02T10:00:30Z",
                                     "delay 30s",
                                     "Retrying (3 of 3)",
                                     "",
                                 }));
    EXPECT_EQ(result->flights.size(), 11u);
    EXPECT_TRUE(result->warnings.empty());
}

// ===========================================================================
// 03 §5.3 / §5.4 / §5.6 / §5.8 rules
// ===========================================================================

TEST(FlightSearchService, Zero_planned_requests_finish_Done_0_of_0_without_a_fetching_report)
{
    SearchHarness harness{default_responder()};
    harness.warm_map();
    ProgressLog progress;
    const LegSearchRequest past = make_request(harness.place("Budapest"), 0, harness.place("London"), 100,
                                               day(2026, 10, 1), day(2026, 10, 20));

    const LegSearchResultPtr result = run_search(*harness.service, past, &progress);

    EXPECT_EQ(describe(progress.entries()), (Strings{kLoading, kRates, kFrankfurterNotice, final_stage("Done", 0, 0)}));
    EXPECT_EQ(result->warnings, (Strings{"The date range is empty or entirely in the past.",
                                         "Dates before today were skipped (searching from 2026-11-02)."}));
    EXPECT_EQ(result->warnings, result->plan.notes);
    EXPECT_EQ(result->requests_completed, 0);
    EXPECT_TRUE(result->flights.empty());
    EXPECT_EQ(harness.timetable_count(), 0u);
}

TEST(FlightSearchService, Warnings_follow_the_03_5_8_order)
{
    // Plan note (I1), approximate rates (W2), a foreign origin (W4), a failed request
    // (W5), a missing fare currency (W6), then the abort reason (blocked).
    const std::vector<FareRow> bud{
        {.from = "BUD", .to = "LGW", .departure = "2026-11-10T06:00:00", .amount = "10.0", .currency = "ZZZ"},
        {.from = "KTW", .to = "LTN", .departure = "2026-11-11T06:00:00", .amount = "20.0"},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-12T06:00:00", .amount = "30.0"},
    };
    SearchHarnessOptions options;
    options.fx_responder = [](const RecordedRequest&) -> ScriptedResponse {
        throw wizz::http::TransportError("offline");
    };
    SearchHarness harness{timetable_responder([json = timetable_json(bud)](const RecordedRequest& request) {
                              const std::string from = departure_station(request);
                              if (from == "BUD") {
                                  return json_response(json);
                              }
                              if (from == "BTS") {
                                  return json_response(R"({"message":"nope"})", 404);
                              }
                              return string_content_response(429).with_header("x-kpsdk-ct", "abc");
                          }),
                          std::move(options)};
    harness.warm_map();
    const LegSearchRequest request = make_request(harness.place("Budapest"), 200, harness.place("London"), 30,
                                                  day(2026, 10, 30), day(2026, 11, 20));

    const LegSearchResultPtr result = run_search(*harness.service, request);

    EXPECT_EQ(result->warnings,
              (Strings{
                  "Dates before today were skipped (searching from 2026-11-02).",
                  "Exchange rates are approximate (built-in table, 2026-11-02); converted prices may be off by a few "
                  "percent.",
                  "BUD → London (LON) was answered with flights from Katowice (KTW), which is outside your start "
                  "radius; those fares were left out.",
                  "BTS → London (LON) (2 Nov–20 Nov): " + kNope,
                  "No exchange rate for ZZZ; those fares are shown without a converted price.",
                  std::string{kKasadaBlocked},
              }));
    EXPECT_EQ(result->abort_reason, std::string{kKasadaBlocked});
    EXPECT_EQ(result->requests_completed, 2);
    EXPECT_EQ(describe(result->flights), (Strings{"BUD→LTN 2026-11-12 06:00", "BUD→LGW 2026-11-10 06:00"}));
}

TEST(FlightSearchService, Foreign_origin_warning_once_per_queried_and_answering_pair)
{
    // Budapest r0: BTS and DEB are not plan origins. Two chunks answer the same rows.
    const std::vector<FareRow> rows{
        {.from = "BTS", .to = "LTN", .departure = "2026-11-12T07:15:00", .amount = "10.0"},
        {.from = "DEB", .to = "LTN", .departure = "2026-11-13T07:15:00", .amount = "10.0"},
        {.from = "BTS", .to = "LTN", .departure = "2026-11-14T07:15:00", .amount = "10.0"},
        {.from = "BUD", .to = "LTN", .departure = "2026-11-15T06:00:00", .amount = "10.0"},
    };
    SearchHarness harness{
        timetable_responder([json = timetable_json(rows)](const RecordedRequest&) { return json_response(json); })};
    harness.warm_map();
    BatchLog batches;
    const LegSearchRequest request = make_request(harness.place("Budapest"), 0, harness.place("London"), 100,
                                                  day(2026, 11, 10), day(2027, 1, 5));

    const LegSearchResultPtr result = run_search(*harness.service, request, nullptr, &batches);

    EXPECT_EQ(harness.timetable_count(), 2u);  // 11-10..12-21 and 12-22..01-05
    EXPECT_EQ(result->warnings,
              (Strings{"BUD → London (LON) was answered with flights from Bratislava (BTS), which is outside your "
                       "start radius; those fares were left out.",
                       "BUD → London (LON) was answered with flights from Debrecen (DEB), which is outside your start "
                       "radius; those fares were left out."}));
    EXPECT_EQ(describe(result->flights), (Strings{"BUD→LTN 2026-11-15 06:00"}));
    EXPECT_EQ(batches.sizes(), (Sizes{1}));  // the second chunk's row is a tie: no batch at all
}

TEST(FlightSearchService, Success_or_route_not_served_resets_the_failure_counter)
{
    // Anywhere from Budapest r200 plans 8 requests (P-15). Three failures, a reset,
    // three failures, a reset: never 4 in a row, so no abort.
    for (const bool reset_by_success : {false, true}) {
        SCOPED_TRACE(reset_by_success ? "reset by a success" : "reset by InvalidMarket");
        SearchHarness harness{timetable_responder([reset_by_success](const RecordedRequest& request) {
            const std::string route = departure_station(request) + "-" + arrival_station(request);
            if (route == "BUD-KRK") {
                return json_response(R"({"validationCodes":["SomethingElse"]})", 400);
            }
            if (route == "BUD-TIA") {
                return reset_by_success ? json_response(std::string{kEmptyTimetableBody})
                                        : json_response(std::string{kInvalidMarketBody}, 400);
            }
            if (route == "DEB-LTN") {
                return json_response(std::string{kEmptyTimetableBody});
            }
            return json_response(R"({"message":"nope"})", 404);
        })};
        harness.warm_map();
        ProgressLog progress;
        const LegSearchRequest anywhere = make_request(harness.place("Budapest"), 200, std::nullopt, 100,
                                                       day(2026, 11, 10), day(2026, 11, 20));

        const LegSearchResultPtr result = run_search(*harness.service, anywhere, &progress);

        EXPECT_EQ(result->abort_reason, std::nullopt);
        EXPECT_EQ(harness.timetable_count(), 8u);
        EXPECT_EQ(result->requests_completed, 8);
        EXPECT_EQ(result->routes_not_served, reset_by_success ? 0 : 1);
        EXPECT_EQ(describe(progress.entries().back()), final_stage("Done", 8, 8));
        ASSERT_EQ(result->warnings.size(), 7u);
        EXPECT_EQ(result->warnings[1], "BUD → Barcelona El Prat (BCN) (10 Nov–20 Nov): " + kNope);
        EXPECT_EQ(result->warnings[2], "BUD → London (LON) (10 Nov–20 Nov): " + kNope);
        // A validation error with an unhandled code is an ordinary API failure.
        EXPECT_EQ(result->warnings[3], "BUD → Katowice / Kraków (10 Nov–20 Nov): Wizz Air rejected the request: "
                                       "SomethingElse");
        EXPECT_EQ(result->warnings[4], "BTS → Barcelona El Prat (BCN) (10 Nov–20 Nov): " + kNope);
        EXPECT_EQ(result->warnings[5], "BTS → London (LON) (10 Nov–20 Nov): " + kNope);
        EXPECT_EQ(result->warnings[6], "BTS → Tirana (TIA) (10 Nov–20 Nov): " + kNope);
    }
}

TEST(FlightSearchService, Route_map_failure_propagates_through_the_future)
{
    SearchHarness harness{[fallback = default_responder()](const RecordedRequest& request) {
        if (request.url.contains("/asset/map")) {
            return json_response(R"({"message":"gone"})", 404);
        }
        return fallback(request);
    }};
    ProgressLog progress;
    std::future<LegSearchResultPtr> future = harness.service->search_async(
        budapest_r0_to_london(harness), progress.sink(), {}, {});

    const std::string message = expect_exactly<WizzApiError>(thrown_by([&] { static_cast<void>(future.get()); }));
    EXPECT_EQ(message, R"(Unexpected Wizz Air response (HTTP 404): {"message":"gone"})");
    EXPECT_EQ(describe(progress.entries()),
              (Strings{kLoading, "Loading Wizz Air route map / Looking up the current Wizz Air API version / 0/0" + kStep}));
    EXPECT_EQ(harness.fx_transport->request_count(), 0u);
}

TEST(FlightSearchService, A_cancel_before_the_fetch_loop_propagates_after_the_first_report)
{
    // 03 §5.6: "a pre-cancelled token gave TaskCanceledException after the first report".
    SearchHarness harness{default_responder()};
    harness.warm_map();
    std::stop_source cancel;
    cancel.request_stop();

    ProgressLog progress;
    std::future<LegSearchResultPtr> future = harness.service->search_async(budapest_r0_to_london(harness),
                                                                           progress.sink(), {}, cancel.get_token());
    EXPECT_EQ(expect_exactly<OperationCancelled>(thrown_by([&] { static_cast<void>(future.get()); })),
              "The operation was canceled.");
    EXPECT_EQ(describe(progress.entries()), (Strings{kLoading}));

    // The blocking entry point behaves the same, on the calling thread.
    ProgressLog direct;
    static_cast<void>(expect_exactly<OperationCancelled>(thrown_by([&] {
        static_cast<void>(
            harness.service->search(budapest_r0_to_london(harness), direct.sink(), {}, cancel.get_token()));
    })));
    EXPECT_EQ(describe(direct.entries()), (Strings{kLoading}));
    EXPECT_EQ(direct.threads(), (std::vector<std::thread::id>{std::this_thread::get_id()}));

    EXPECT_EQ(harness.fx_transport->request_count(), 0u);
    EXPECT_EQ(harness.timetable_count(), 0u);
}

TEST(FlightSearchService, A_cancel_during_the_rate_stage_propagates)
{
    std::stop_source cancel;
    SearchHarnessOptions options;
    options.fx_responder = [&cancel](const RecordedRequest&) {
        cancel.request_stop();
        return wizz::test::bare_response(500);  // the service moves on to the next source and sees the cancel
    };
    SearchHarness harness{default_responder(), std::move(options)};
    harness.warm_map();
    ProgressLog progress;

    std::future<LegSearchResultPtr> future = harness.service->search_async(budapest_r0_to_london(harness),
                                                                           progress.sink(), {}, cancel.get_token());

    static_cast<void>(expect_exactly<OperationCancelled>(thrown_by([&] { static_cast<void>(future.get()); })));
    const std::vector<SearchProgress> entries = progress.entries();
    ASSERT_GE(entries.size(), 3u);
    EXPECT_EQ(describe(std::span{entries}.first(3)), (Strings{kLoading, kRates, kFrankfurterNotice}));
    for (const SearchProgress& entry : entries) {
        EXPECT_NE(entry.stage, "Fetching fares");
        EXPECT_NE(entry.stage, "Cancelled");
    }
    EXPECT_EQ(harness.timetable_count(), 0u);
}

TEST(FlightSearchService, A_cancel_is_noticed_in_the_loop_even_when_every_timetable_is_cached)
{
    SearchHarness harness{default_responder()};
    harness.warm_map();
    const LegSearchRequest request = make_request(harness.place("Budapest"), 200, harness.place("London"), 30,
                                                  day(2026, 11, 10), day(2026, 11, 20));
    static_cast<void>(run_search(*harness.service, request));  // warm every cache
    const std::size_t sent = harness.wizz_transport->request_count();

    std::stop_source cancel;
    ProgressLog progress;
    const wizz::ProgressSink sink = [&cancel, inner = progress.sink()](const SearchProgress& entry) {
        inner(entry);
        if (entry.stage == "Fetching fares" && entry.completed == 1) {
            cancel.request_stop();  // while the BTS request is being reported
        }
    };
    const LegSearchResultPtr result =
        harness.service->search_async(request, sink, {}, cancel.get_token()).get();

    EXPECT_TRUE(result->cancelled);
    EXPECT_EQ(result->requests_completed, 1);
    EXPECT_EQ(describe(progress.entries().back()), final_stage("Cancelled", 1, 3));
    EXPECT_EQ(result->flights.size(), 11u);  // the cached BUD answer
    EXPECT_EQ(harness.wizz_transport->request_count(), sent);
}

TEST(FlightSearchService, Unexpected_errors_propagate_through_the_future)
{
    {
        // Not a WizzApiError: the loop does not catch it (03 §5.6 last row).
        SearchHarness harness{timetable_responder(
            [](const RecordedRequest&) -> ScriptedResponse { throw std::logic_error("must not be called"); })};
        harness.warm_map();
        std::future<LegSearchResultPtr> future = harness.service->search_async(budapest_r0_to_london(harness), {}, {}, {});
        EXPECT_EQ(expect_exactly<std::logic_error>(thrown_by([&] { static_cast<void>(future.get()); })),
                  "must not be called");
    }
    {
        // A throwing callback is not swallowed either.
        SearchHarness harness{default_responder()};
        harness.warm_map();
        std::future<LegSearchResultPtr> future = harness.service->search_async(
            budapest_r0_to_london(harness), {},
            [](std::vector<FlightResultPtr>) { throw std::runtime_error("sink failed"); }, {});
        EXPECT_EQ(expect_exactly<std::runtime_error>(thrown_by([&] { static_cast<void>(future.get()); })),
                  "sink failed");
    }
}

TEST(FlightSearchService, Partial_batches_hand_over_the_rows_of_the_final_result)
{
    SearchHarness harness{route_responder()};
    harness.warm_map();
    BatchLog batches;
    const LegSearchResultPtr result = run_search(*harness.service, budapest_to_london(harness, 30, 2), nullptr, &batches);

    // The batch rows are the very instances of the final list (identity for the UI pick).
    std::vector<FlightResultPtr> streamed;
    for (const auto& batch : batches.batches()) {
        streamed.insert(streamed.end(), batch.begin(), batch.end());
    }
    ASSERT_EQ(streamed.size(), result->flights.size());
    for (const FlightResultPtr& flight : result->flights) {
        EXPECT_NE(std::ranges::find(streamed, flight), streamed.end()) << describe(*flight);
    }
    // The first batch is in response order (fixture row order).
    EXPECT_EQ(describe(batches.batches()[0])[0], "BUD→LTN 2026-11-10 06:00");
    EXPECT_EQ(describe(batches.batches()[0])[5], "BUD→LGW 2026-11-15 08:30");
}

// ===========================================================================
// IsBetter (03 §5.3) and Reprice (03 §6.3)
// ===========================================================================

[[nodiscard]] FlightResult fare_with(FareStatus status, std::optional<Decimal> total)
{
    FlightResult result;
    result.fare.status = status;
    result.total_converted = total;
    result.price_per_person_converted = total;
    return result;
}

TEST(FlightSearchService, IsBetter_prefers_available_then_a_strictly_lower_total)
{
    const FlightResult available_10 = fare_with(FareStatus::available, Decimal{10});
    const FlightResult available_20 = fare_with(FareStatus::available, Decimal{20});
    const FlightResult available_10_00 = fare_with(FareStatus::available, dec("10.00"));
    const FlightResult available_none = fare_with(FareStatus::available, std::nullopt);
    const FlightResult check = fare_with(FareStatus::check_on_site, std::nullopt);

    EXPECT_TRUE(FlightSearchService::is_better(available_none, check));   // available beats CheckOnSite
    EXPECT_FALSE(FlightSearchService::is_better(check, available_20));
    EXPECT_TRUE(FlightSearchService::is_better(available_10, available_20));
    EXPECT_FALSE(FlightSearchService::is_better(available_20, available_10));
    EXPECT_FALSE(FlightSearchService::is_better(available_10_00, available_10));  // ties keep the existing row
    EXPECT_TRUE(FlightSearchService::is_better(available_20, available_none));    // a missing total is DECIMAL_MAX
    EXPECT_FALSE(FlightSearchService::is_better(available_none, available_20));
    EXPECT_FALSE(FlightSearchService::is_better(available_none, available_none));
    EXPECT_FALSE(FlightSearchService::is_better(check, check));
}

TEST(FlightSearchService, Reprice_delegates_to_pricing_without_requests)
{
    SearchHarness harness{route_responder()};
    harness.warm_map();
    const LegSearchResultPtr eur = run_search(*harness.service, budapest_to_london(harness, 30, 2));
    const std::size_t wizz_sent = harness.wizz_transport->request_count();
    const std::size_t fx_sent = harness.fx_transport->request_count();

    const LegSearchResultPtr huf = FlightSearchService::reprice(*eur, "huf");

    EXPECT_EQ(huf->request.currency, "HUF");
    EXPECT_EQ(describe(*huf->flights[0]), "BTS→LTN 2026-11-12 07:15");
    EXPECT_EQ(huf->flights[0]->price_per_person_converted, Decimal{5491});
    EXPECT_EQ(huf->flights[0]->total_converted, Decimal{10982});
    EXPECT_EQ(describe(*huf->flights[1]), "BUD→LTN 2026-11-14 06:00");
    EXPECT_EQ(huf->flights[1]->total_converted, Decimal{38580});
    EXPECT_EQ(huf->warnings, eur->warnings);
    EXPECT_EQ(huf->rates, eur->rates);
    EXPECT_EQ(describe(huf->flights), describe(wizz::pricing::reprice(*eur, "huf")->flights));
    EXPECT_EQ(harness.wizz_transport->request_count(), wizz_sent);
    EXPECT_EQ(harness.fx_transport->request_count(), fx_sent);
}

// ===========================================================================
// 03 §5.10 LoadAirports
// ===========================================================================

TEST(FlightSearchService, LoadAirports_reports_the_stage_and_forwards_notices_on_the_calling_thread)
{
    SearchHarness harness{default_responder()};
    ProgressLog progress;

    const AirportCatalogPtr catalog = harness.service->load_airports(progress.sink(), {});

    ASSERT_NE(catalog, nullptr);
    EXPECT_EQ(catalog->all().size(), 12u);
    EXPECT_EQ(catalog->real_airports().size(), 11u);
    EXPECT_EQ(describe(progress.entries()),
              (Strings{kLoading, "Loading Wizz Air route map / Looking up the current Wizz Air API version / 0/0" + kStep}));
    for (const std::thread::id id : progress.threads()) {
        EXPECT_EQ(id, std::this_thread::get_id());  // no thread hop is forced
    }
    EXPECT_EQ(harness.wizz_transport->request_count(), 2u);  // homepage, map

    // Memory afterwards: no request; the stage is still reported.
    ProgressLog again;
    EXPECT_EQ(harness.service->load_airports(again.sink(), {}), catalog);
    EXPECT_EQ(describe(again.entries()), (Strings{kLoading}));
    EXPECT_EQ(harness.wizz_transport->request_count(), 2u);
}

TEST(FlightSearchService, LoadAirports_forwards_countdowns_with_a_new_step_start)
{
    const auto clock = std::make_shared<FixedClock>();
    std::atomic<int> map_calls{0};
    SearchHarnessOptions options;
    options.clock = clock;
    options.on_delay = [clock](Duration delay) { clock->advance(delay); };
    SearchHarness harness{[&map_calls, fallback = default_responder()](const RecordedRequest& request) {
                              if (request.url.contains("/asset/map") && map_calls.fetch_add(1) == 0) {
                                  return html_response("busy", 503);
                              }
                              return fallback(request);
                          },
                          std::move(options)};
    ProgressLog progress;

    const AirportCatalogPtr catalog = harness.service->load_airports(progress.sink(), {});

    ASSERT_NE(catalog, nullptr);
    EXPECT_EQ(describe(progress.entries()),
              (Strings{
                  kLoading,
                  "Loading Wizz Air route map / Looking up the current Wizz Air API version / 0/0" + kStep,
                  "Loading Wizz Air route map / Wizz Air is busy (HTTP 503) — retry 1 of 3 / 0/0 "
                  "wait=2026-11-02T10:00:05Z" + kStep,
                  "Loading Wizz Air route map / Retrying (1 of 3) / 0/0 step=2026-11-02T10:00:05Z",
              }));
}

TEST(FlightSearchService, LoadAirports_forwards_force_refresh_and_errors)
{
    {
        SearchHarness harness{default_responder()};
        const AirportCatalogPtr first = harness.service->load_airports({}, {});
        const AirportCatalogPtr refreshed = harness.service->load_airports({}, {}, true);
        EXPECT_NE(refreshed, first);
        EXPECT_EQ(harness.wizz_transport->count_urls_containing("/asset/map"), 2u);
    }
    {
        SearchHarness harness{[fallback = default_responder()](const RecordedRequest& request) {
            if (request.url.contains("/asset/map")) {
                return json_response(R"({"message":"gone"})", 404);
            }
            return fallback(request);
        }};
        EXPECT_EQ(expect_exactly<WizzApiError>(
                      thrown_by([&] { static_cast<void>(harness.service->load_airports({}, {})); })),
                  R"(Unexpected Wizz Air response (HTTP 404): {"message":"gone"})");
    }
    {
        SearchHarness harness{default_responder()};
        std::stop_source cancel;
        cancel.request_stop();
        static_cast<void>(expect_exactly<OperationCancelled>(
            thrown_by([&] { static_cast<void>(harness.service->load_airports({}, cancel.get_token())); })));
    }
}

// ===========================================================================
// Construction and worker ownership
// ===========================================================================

TEST(FlightSearchService, Constructor_rejects_missing_dependencies)
{
    SearchHarness harness{default_responder()};
    const auto clock = std::make_shared<FixedClock>();
    const auto& locale = Locale::en_us();
    EXPECT_THROW(FlightSearchService(nullptr, harness.fx, clock, locale), std::invalid_argument);
    EXPECT_THROW(FlightSearchService(harness.wizz, nullptr, clock, locale), std::invalid_argument);
    EXPECT_THROW(FlightSearchService(harness.wizz, harness.fx, nullptr, locale), std::invalid_argument);
    EXPECT_THROW(FlightSearchService(harness.wizz, harness.fx, clock, nullptr), std::invalid_argument);
}

TEST(FlightSearchService, Destroying_the_service_cancels_and_joins_its_searches)
{
    // A real, cancellable back-off sleep of 5 s that only a stop request can cut short.
    SearchHarnessOptions options;
    options.customize = [](wizz::WizzClientOptions& client) { client.delay = &wizz::cancellable_sleep; };
    SearchHarness harness{timetable_responder([](const RecordedRequest&) { return html_response("busy", 503); }),
                          std::move(options)};
    harness.warm_map();

    // The back-off the search announces (now → waiting_until on the harness's fixed clock)
    // bounds the shutdown: a join that waited the back-off out would take all of it,
    // a cancelled one a fraction — whatever the machine's load.
    std::promise<wizz::Duration> backing_off;
    std::atomic<bool> announced{false};
    const wizz::ProgressSink sink = [&](const SearchProgress& progress) {
        if (progress.waiting_until_utc && !announced.exchange(true)) {
            backing_off.set_value(*progress.waiting_until_utc - harness.clock->utc_now());
        }
    };
    std::stop_source caller;
    std::future<LegSearchResultPtr> future =
        harness.service->search_async(budapest_r0_to_london(harness), sink, {}, caller.get_token());
    std::future<wizz::Duration> backoff = backing_off.get_future();
    ASSERT_EQ(backoff.wait_for(60s), std::future_status::ready);
    const wizz::Duration backoff_length = backoff.get();
    ASSERT_GE(backoff_length, wizz::Duration{std::chrono::seconds{5}}) << "the first retry delay (04 §9)";

    const auto started = std::chrono::steady_clock::now();
    harness.service.reset();  // requests stop on the worker's own source and joins it
    EXPECT_LT(std::chrono::steady_clock::now() - started, backoff_length) << "the shutdown waited the back-off out";

    ASSERT_EQ(future.wait_for(0s), std::future_status::ready);
    const LegSearchResultPtr result = future.get();
    EXPECT_TRUE(result->cancelled);
    EXPECT_FALSE(caller.stop_requested());  // the caller's token is untouched
}

TEST(FlightSearchService, Searches_may_run_concurrently_and_finished_workers_are_reaped)
{
    SearchHarness harness{route_responder()};
    harness.warm_map();
    std::vector<std::future<LegSearchResultPtr>> futures;
    for (int i = 0; i < 6; ++i) {
        futures.push_back(harness.service->search_async(budapest_to_london(harness, 30, 1 + i % 2), {}, {}, {}));
    }
    for (std::future<LegSearchResultPtr>& future : futures) {
        const LegSearchResultPtr result = future.get();
        EXPECT_EQ(result->flights.size(), 12u);
        EXPECT_EQ(result->routes_not_served, 1);
    }
    // Later searches still work after earlier workers finished (they are reaped on the next call).
    EXPECT_EQ(run_search(*harness.service, budapest_to_london(harness, 30, 2))->flights.size(), 12u);
}

// ===========================================================================
// Reused fares (DECISIONS D-66..D-68, D-70)
// ===========================================================================

TEST(FlightSearchServiceReuse, Counters_and_the_age_of_the_oldest_reused_fare)
{
    SearchHarness harness{daily_fares_responder()};
    harness.warm_map();
    const LegSearchRequest request = budapest_to_london(harness, 30, 2);  // three planned requests

    const LegSearchResultPtr first = run_search(*harness.service, request);
    EXPECT_EQ(first->requests_from_cache, 0);
    EXPECT_EQ(first->requests_partly_cached, 0);
    EXPECT_EQ(first->oldest_reused_age, std::nullopt);
    EXPECT_EQ(harness.timetable_count(), 3u);

    harness.clock->advance(10min);
    const LegSearchResultPtr repeat = run_search(*harness.service, request);
    EXPECT_EQ(repeat->requests_from_cache, 3);
    EXPECT_EQ(repeat->requests_partly_cached, 0);
    EXPECT_EQ(repeat->oldest_reused_age, Duration{10min});
    EXPECT_EQ(repeat->requests_completed, 3);  // progress counts every planned request as before
    EXPECT_EQ(repeat->flights.size(), first->flights.size());
    EXPECT_EQ(harness.timetable_count(), 3u);

    harness.clock->advance(2min);
    LegSearchRequest shifted = request;
    shifted.from = day(2026, 11, 11);
    shifted.to = day(2026, 11, 21);
    const LegSearchResultPtr partly = run_search(*harness.service, shifted);
    EXPECT_EQ(partly->requests_from_cache, 0);
    EXPECT_EQ(partly->requests_partly_cached, 3);
    EXPECT_EQ(partly->oldest_reused_age, Duration{12min});
    EXPECT_EQ(harness.timetable_count(), 6u);
    EXPECT_EQ(harness.timetable_bodies().back(), body("DEB", "LTN", "2026-11-21", "2026-11-21", 2));

    // A currency change keeps them (pricing::reprice copies the result).
    const LegSearchResultPtr repriced = FlightSearchService::reprice(*partly, "HUF");
    EXPECT_EQ(repriced->requests_partly_cached, 3);
    EXPECT_EQ(repriced->oldest_reused_age, Duration{12min});
}

TEST(FlightSearchServiceReuse, A_not_served_route_answered_from_its_mark_counts_as_reused)
{
    SearchHarness harness{route_responder()};  // DEB is not served
    harness.warm_map();
    const LegSearchRequest request = budapest_to_london(harness, 30, 2);
    (void)run_search(*harness.service, request);
    ASSERT_EQ(harness.timetable_count(), 3u);

    const LegSearchResultPtr repeat = run_search(*harness.service, request);
    EXPECT_EQ(repeat->requests_from_cache, 3);
    EXPECT_EQ(repeat->routes_not_served, 1);
    EXPECT_EQ(harness.timetable_count(), 3u);
}

TEST(FlightSearchServiceReuse, The_reuse_window_is_taken_once_when_the_search_begins)
{
    SearchHarness harness{daily_fares_responder()};
    harness.warm_map();
    const LegSearchRequest request = budapest_to_london(harness, 30, 2);
    (void)run_search(*harness.service, request);
    ASSERT_EQ(harness.timetable_count(), 3u);

    harness.clock->advance(45min);
    harness.wizz->set_timetable_reuse_window(1h);
    // Back to 30 minutes while the search runs, after its first request: the search keeps
    // the hour it began with (a Settings change applies to the next search).
    const wizz::ProgressSink narrow_midway = [&harness](const SearchProgress& entry) {
        if (entry.stage == "Fetching fares" && entry.completed == 1) {
            harness.wizz->set_timetable_reuse_window(30min);
        }
    };
    const LegSearchResultPtr during = harness.service->search_async(request, narrow_midway, {}, {}).get();
    EXPECT_EQ(harness.wizz->timetable_reuse_window(), Duration{30min});
    EXPECT_EQ(during->requests_from_cache, 3);
    EXPECT_EQ(harness.timetable_count(), 3u);

    const LegSearchResultPtr next = run_search(*harness.service, request);
    EXPECT_EQ(next->requests_from_cache, 0);
    EXPECT_EQ(harness.timetable_count(), 6u);
}

TEST(FlightSearchServiceReuse, Fares_json_is_written_when_a_search_ends_also_a_cancelled_one_but_not_after_a_fully_cached_one)
{
    const ScratchDirectory directory;
    SearchHarness harness{daily_fares_responder(),
                          SearchHarnessOptions{.customize = [&directory](wizz::WizzClientOptions& options) {
                              options.cache = wizz::FileCache{directory.path()};
                          }}};
    harness.warm_map();
    const std::filesystem::path file = directory.file("fares.json");
    const LegSearchRequest request = budapest_to_london(harness, 30, 2);

    std::atomic<bool> written_during_the_search{false};
    const wizz::ProgressSink watch = [&](const SearchProgress& entry) {
        if (entry.stage == "Fetching fares" && entry.completed == 2) {
            written_during_the_search = std::filesystem::exists(file);  // two answers recorded by now
        }
    };
    (void)harness.service->search_async(request, watch, {}, {}).get();
    EXPECT_FALSE(written_during_the_search.load());
    ASSERT_TRUE(std::filesystem::exists(file));
    EXPECT_EQ(wizz::fare_days::parse(directory.read("fares.json")).day_count(), 33u);  // 3 routes × 11 days

    // A fully cached search changes nothing and writes nothing.
    std::filesystem::remove(file);
    const LegSearchResultPtr cached = run_search(*harness.service, request);
    EXPECT_EQ(cached->requests_from_cache, 3);
    EXPECT_FALSE(std::filesystem::exists(file));

    // A search cancelled after its first request still saves what that request brought.
    LegSearchRequest shifted = request;
    shifted.from = day(2026, 11, 11);
    shifted.to = day(2026, 11, 21);
    std::stop_source cancel;
    const wizz::ProgressSink cancel_after_one = [&cancel](const SearchProgress& entry) {
        if (entry.stage == "Fetching fares" && entry.completed == 1) {
            cancel.request_stop();
        }
    };
    const LegSearchResultPtr cancelled =
        harness.service->search_async(shifted, cancel_after_one, {}, cancel.get_token()).get();
    EXPECT_TRUE(cancelled->cancelled);
    EXPECT_EQ(harness.timetable_count(), 4u);
    ASSERT_TRUE(std::filesystem::exists(file));
    EXPECT_EQ(wizz::fare_days::parse(directory.read("fares.json")).day_count(), 34u);
}

} // namespace
