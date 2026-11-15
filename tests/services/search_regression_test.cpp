// Search regressions through FlightSearchService: 05 §3.11 CT-SRCH-01 (CORE-4),
// CT-SRCH-02 (REG-1 / UX-12), CT-SRCH-06 (UI-2), and the 03 §11 worked end-to-end
// example (outbound search, re-pricing, the return-leg search with its synthetic
// GBP fares, MatchLegs and Combine).
//
// 05 §3.11: a Wizz harness with the FIXED clock, a CurrencyService whose fake
// transport always returns fx_frankfurter_v2.json (Clock SYSTEM, cache disabled),
// FlightSearchService(client, fx, FIXED); GetAirports() is called first.

#include "wizz/services/flight_search_service.hpp"

#include "search_harness.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/round_trips.hpp"

#include "support/fake_transport.hpp"
#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace wizz::test::search;
using namespace std::chrono_literals;

using wizz::Date;
using wizz::Decimal;
using wizz::FlightResultPtr;
using wizz::LegSearchRequest;
using wizz::LegSearchResultPtr;
using wizz::SearchProgress;
using wizz::SystemClock;
using wizz::test::default_responder;
using wizz::test::json_response;
using wizz::test::read_fixture;
using wizz::test::RecordedRequest;
using wizz::test::Responder;
using wizz::test::ScriptedResponse;

using Strings = std::vector<std::string>;
using Sizes = std::vector<std::size_t>;

const std::string kStep = " step=2026-11-02T10:00:00Z";

[[nodiscard]] constexpr Date day(int year, unsigned month, unsigned d) noexcept
{
    return Date::from_ymd(year, month, d);
}

/// 05 §3.11 options: the FX service runs on the SYSTEM clock.
[[nodiscard]] SearchHarnessOptions system_fx_clock()
{
    SearchHarnessOptions options;
    options.fx_clock = std::make_shared<SystemClock>();
    return options;
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
        values.emplace_back(dec(lexeme));
    }
    return values;
}

[[nodiscard]] double round_to(double value, int places)
{
    const double scale = std::pow(10.0, places);
    return std::round(value * scale) / scale;
}

/// "BUD→LGW [LON] 2026-11-10..2026-12-21" for every planned request.
[[nodiscard]] Strings planned(const wizz::SearchPlan& plan)
{
    Strings lines;
    for (const wizz::PlannedRequest& request : plan.requests) {
        lines.push_back(std::format("{}→{} [{}] {}..{}", request.from->iata, request.query_to, request.group_key,
                                    request.start_date.iso(), request.end_date.iso()));
    }
    return lines;
}

// ===========================================================================
// CT-SRCH-01 — Route_not_served_is_not_asked_again_for_later_chunks (CORE-4)
// ===========================================================================

TEST(SearchRegression, CT_SRCH_01_Route_not_served_is_not_asked_again_for_later_chunks)
{
    std::atomic<int> deb_calls{0};
    SearchHarness harness{[&deb_calls, fallback = default_responder()](const RecordedRequest& request) {
                              if (!is_timetable_request(request)) {
                                  return fallback(request);
                              }
                              if (request.body && request.body->contains(R"("DEB")")) {
                                  ++deb_calls;
                                  return json_response(std::string{kInvalidMarketBody}, 400);
                              }
                              return json_response(std::string{kEmptyTimetableBody});
                          },
                          system_fx_clock()};
    harness.warm_map();
    ProgressLog progress;
    const LegSearchRequest request = make_request(harness.place("Budapest"), 200, harness.place("London"), 100,
                                                  day(2026, 11, 10), day(2027, 2, 20));

    const LegSearchResultPtr result = run_search(*harness.service, request, &progress);

    // [A]
    EXPECT_EQ(deb_calls.load(), 1);
    EXPECT_EQ(result->routes_not_served, 1);
    // [D] 9 planned requests in the 3-window order; only 7 calls were sent.
    EXPECT_EQ(planned(result->plan), (Strings{
                                         "BUD→LGW [LON] 2026-11-10..2026-12-21",
                                         "BUD→LGW [LON] 2026-12-22..2027-02-01",
                                         "BUD→LGW [LON] 2027-02-02..2027-02-20",
                                         "BTS→LTN [LON] 2026-11-10..2026-12-21",
                                         "BTS→LTN [LON] 2026-12-22..2027-02-01",
                                         "BTS→LTN [LON] 2027-02-02..2027-02-20",
                                         "DEB→LTN [LON] 2026-11-10..2026-12-21",
                                         "DEB→LTN [LON] 2026-12-22..2027-02-01",
                                         "DEB→LTN [LON] 2027-02-02..2027-02-20",
                                     }));
    EXPECT_EQ(harness.timetable_count(), 7u);
    EXPECT_EQ(result->requests_completed, 9);  // neither cancelled nor aborted
    EXPECT_TRUE(result->flights.empty());
    EXPECT_TRUE(result->warnings.empty());

    // The skipped DEB chunks advance Completed silently: no report of their own (03 §5.4).
    const std::vector<SearchProgress> entries = progress.entries();
    ASSERT_EQ(entries.size(), 11u);
    EXPECT_EQ(describe(std::span{entries}.subspan(3)),
              (Strings{
                  "Fetching fares / BUD → London (LON)   10 Nov – 21 Dec / 0/9" + kStep,
                  "Fetching fares / BUD → London (LON)   22 Dec – 1 Feb / 1/9" + kStep,
                  "Fetching fares / BUD → London (LON)   2 Feb – 20 Feb / 2/9" + kStep,
                  "Fetching fares / BTS → London (LON)   10 Nov – 21 Dec / 3/9" + kStep,
                  "Fetching fares / BTS → London (LON)   22 Dec – 1 Feb / 4/9" + kStep,
                  "Fetching fares / BTS → London (LON)   2 Feb – 20 Feb / 5/9" + kStep,
                  "Fetching fares / DEB → London (LON)   10 Nov – 21 Dec / 6/9" + kStep,
                  "Done /  / 9/9" + kStep,
              }));
}

// ===========================================================================
// CT-SRCH-02 — Fares_answered_from_an_airport_outside_the_start_area_are_left_out
// ===========================================================================

TEST(SearchRegression, CT_SRCH_02_Fares_answered_from_an_airport_outside_the_start_area_are_left_out)
{
    std::string sibling = read_fixture("timetable_v2_bud_ltn.json");
    constexpr std::string_view kBud = R"("departureStation":"BUD")";
    constexpr std::string_view kBts = R"("departureStation":"BTS")";
    for (std::size_t at = sibling.find(kBud); at != std::string::npos; at = sibling.find(kBud, at)) {
        sibling.replace(at, kBud.size(), kBts);
    }
    SearchHarness harness{[sibling, fallback = default_responder()](const RecordedRequest& request) {
                              return is_timetable_request(request) ? json_response(sibling) : fallback(request);
                          },
                          system_fx_clock()};
    harness.warm_map();
    BatchLog batches;
    const LegSearchRequest request = make_request(harness.place("Budapest"), 0, harness.place("London"), 100,
                                                  day(2026, 11, 10), day(2026, 11, 20));

    const LegSearchResultPtr result = run_search(*harness.service, request, nullptr, &batches);

    // [A]
    EXPECT_TRUE(result->flights.empty());
    EXPECT_TRUE(std::ranges::any_of(result->warnings, [](const std::string& warning) {
        return warning.contains("outside your start radius");
    }));
    // [D] once per (requested origin → answered origin) pair, although 11 rows were left out.
    EXPECT_EQ(result->warnings,
              (Strings{"BUD → London (LON) was answered with flights from Bratislava (BTS), which is outside your "
                       "start radius; those fares were left out."}));
    EXPECT_TRUE(batches.sizes().empty());  // an empty batch is never reported
    EXPECT_EQ(result->requests_completed, 1);
}

// ===========================================================================
// CT-SRCH-06 — Fully_cached_search_never_completes_synchronously (UI-2)
// ===========================================================================

TEST(SearchRegression, CT_SRCH_06_Fully_cached_search_never_completes_synchronously)
{
    SearchHarness harness{default_responder(), system_fx_clock()};
    harness.warm_map();
    const LegSearchRequest request = make_request(harness.place("Budapest"), 0, harness.place("London"), 100,
                                                  day(2026, 11, 10), day(2026, 11, 20));
    const LegSearchResultPtr warm = run_search(*harness.service, request);  // warms every cache
    ASSERT_EQ(warm->flights.size(), 11u);
    const std::size_t wizz_sent = harness.wizz_transport->request_count();
    const std::size_t fx_sent = harness.fx_transport->request_count();

    // Hold the worker on a latch that is released only after the call returned
    // (05 CT-SRCH-06 [N]). If the search ran inline, the first report would wait
    // for the release in vain and the call would not return in time.
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> call_returned{false};
    std::atomic<bool> first_report_saw_return{false};
    std::atomic<int> reports{0};
    ProgressLog progress;
    const wizz::ProgressSink sink = [&, inner = progress.sink()](const SearchProgress& entry) {
        if (reports.fetch_add(1) == 0) {
            const bool was_released = released.wait_for(30s) == std::future_status::ready;
            first_report_saw_return = was_released && call_returned.load();
        }
        inner(entry);
    };
    BatchLog batches;
    LegSearchRequest gbp = request;
    gbp.currency = "GBP";

    std::future<LegSearchResultPtr> task = harness.service->search_async(gbp, sink, batches.sink(), {});
    call_returned = true;

    // [A] not complete at the moment the call returns.
    EXPECT_EQ(task.wait_for(0s), std::future_status::timeout);
    release.set_value();
    const LegSearchResultPtr result = task.get();

    EXPECT_TRUE(first_report_saw_return.load());
    ASSERT_FALSE(result->flights.empty());
    EXPECT_EQ(result->flights[0]->target_currency, "GBP");

    // [D] no extra HTTP: timetable, FX and catalog all come from memory.
    EXPECT_EQ(harness.wizz_transport->request_count(), wizz_sent);
    EXPECT_EQ(harness.fx_transport->request_count(), fx_sent);
    EXPECT_EQ(describe(*result->flights[0]), "BUD→LTN 2026-11-14 06:00");
    EXPECT_EQ(result->flights[0]->price_per_person_converted, dec("43.99"));  // 19290 / 366.3 × 0.83536

    // Reports 1 and 2 are sent even from memory; no FX notice this time (03 §5.4).
    EXPECT_EQ(describe(progress.entries()), (Strings{
                                                "Loading Wizz Air route map /  / 0/0" + kStep,
                                                "Getting exchange rates /  / 0/0" + kStep,
                                                "Fetching fares / BUD → London (LON)   10 Nov – 20 Nov / 0/1" + kStep,
                                                "Done /  / 1/1" + kStep,
                                            }));
    // Every callback ran on the worker, never on the caller's thread; no duplicated rows.
    for (const std::thread::id id : progress.threads()) {
        EXPECT_NE(id, std::this_thread::get_id());
    }
    EXPECT_EQ(batches.sizes(), (Sizes{11}));
    EXPECT_EQ(result->flights.size(), 11u);
}

// ===========================================================================
// 03 §11 — worked end-to-end example
// ===========================================================================

/// RouteResponder for the outbound leg plus the synthetic GBP fares of the
/// 03 §11 return leg (London r30 → Budapest r200).
[[nodiscard]] Responder worked_example_responder()
{
    const std::vector<FareRow> lgw_bud{
        {.from = "LGW", .to = "BUD", .departure = "2026-11-16T14:00:00", .amount = "29.99", .currency = "GBP"},
    };
    const std::vector<FareRow> ltn_bud{
        {.from = "LTN", .to = "BUD", .departure = "2026-11-17T21:35:00", .amount = "17.99", .currency = "GBP"},
        {.from = "LTN", .to = "BUD", .departure = "2026-11-14T10:05:00", .amount = "24.99", .currency = "GBP"},
    };
    const std::vector<FareRow> ltn_bts{
        {.from = "LTN", .to = "BTS", .departure = "2026-11-15T12:00:00", .amount = "19.99", .currency = "GBP"},
    };
    return [route = route_responder(), lgw_bud = timetable_json(lgw_bud), ltn_bud = timetable_json(ltn_bud),
            ltn_bts = timetable_json(ltn_bts)](const RecordedRequest& request) {
        if (is_timetable_request(request)) {
            const std::string path = departure_station(request) + "-" + arrival_station(request);
            if (path == "LGW-BUD") {
                return json_response(lgw_bud);
            }
            if (path == "LTN-BUD") {
                return json_response(ltn_bud);
            }
            if (path == "LTN-BTS") {
                return json_response(ltn_bts);
            }
            if (path == "LTN-DEB") {
                return json_response(std::string{kEmptyTimetableBody});
            }
        }
        return route(request);
    };
}

TEST(SearchRegression, Worked_example_03_11_end_to_end)
{
    SearchHarness harness{worked_example_responder()};
    harness.warm_map();
    BatchLog batches;

    // Outbound: Budapest r200 → London r30, 10–20 Nov 2026, Adults 2, EUR, Regular.
    const LegSearchRequest outbound_request = make_request(harness.place("Budapest"), 200, harness.place("London"), 30,
                                                           day(2026, 11, 10), day(2026, 11, 20), 2);
    const LegSearchResultPtr outbound = run_search(*harness.service, outbound_request, nullptr, &batches);

    // Plan.
    const wizz::SearchPlan& plan = outbound->plan;
    ASSERT_EQ(plan.origin_airports.size(), 3u);
    EXPECT_EQ(plan.origin_airports[0].airport->iata, "BUD");
    EXPECT_EQ(round_to(plan.origin_airports[0].distance_km, 3), 0.0);
    EXPECT_EQ(plan.origin_airports[1].airport->iata, "BTS");
    EXPECT_EQ(round_to(plan.origin_airports[1].distance_km, 3), 172.544);
    EXPECT_EQ(plan.origin_airports[2].airport->iata, "DEB");
    EXPECT_EQ(round_to(plan.origin_airports[2].distance_km, 3), 184.077);
    ASSERT_EQ(plan.destination_airports.size(), 2u);
    EXPECT_EQ(plan.destination_airports[0].airport->iata, "LGW");
    EXPECT_EQ(round_to(plan.destination_airports[0].distance_km, 3), 40.558);
    EXPECT_EQ(plan.destination_airports[1].airport->iata, "LTN");
    EXPECT_EQ(round_to(plan.destination_airports[1].distance_km, 3), 44.072);
    EXPECT_EQ(planned(plan), (Strings{"BUD→LGW [LON] 2026-11-10..2026-11-20", "BTS→LTN [LON] 2026-11-10..2026-11-20",
                                      "DEB→LTN [LON] 2026-11-10..2026-11-20"}));
    EXPECT_EQ(plan.routes_considered, 3);
    EXPECT_TRUE(plan.notes.empty());
    EXPECT_FALSE(plan.same_area);

    // Execution: batches [11, 1]; 12 flights, RoutesNotServed 1, RequestsCompleted 3, no warnings.
    EXPECT_EQ(batches.sizes(), (Sizes{11, 1}));
    ASSERT_EQ(outbound->flights.size(), 12u);
    EXPECT_EQ(outbound->routes_not_served, 1);
    EXPECT_EQ(outbound->requests_completed, 3);
    EXPECT_TRUE(outbound->warnings.empty());
    EXPECT_FALSE(outbound->cancelled);

    // The 12-row table: equal totals ordered by date (SortByPrice key 3).
    EXPECT_EQ(describe(outbound->flights), (Strings{
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
    EXPECT_EQ(per_person(outbound->flights), decs({"14.99", "52.66", "52.66", "52.66", "52.66", "58.94", "58.94",
                                                   "58.94", "58.94", "69.31", "84.06", "94.70"}));
    EXPECT_EQ(totals(outbound->flights), decs({"29.98", "105.32", "105.32", "105.32", "105.32", "117.88", "117.88",
                                               "117.88", "117.88", "138.62", "168.12", "189.40"}));
    for (const FlightResultPtr& flight : outbound->flights) {
        const bool bts = flight->fare.from == "BTS";
        EXPECT_EQ(round_to(flight->from_distance_km, 1), bts ? 172.5 : 0.0);
        ASSERT_TRUE(flight->to_distance_km.has_value());
        EXPECT_EQ(round_to(*flight->to_distance_km, 1), flight->fare.to == "LGW" ? 40.6 : 44.1);
        EXPECT_FALSE(flight->outside_destination_radius);
    }
    EXPECT_EQ(outbound->flights[0]->booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/null/2/0/0/null");

    // Reprice (no network).
    const std::size_t sent = harness.wizz_transport->request_count();
    const LegSearchResultPtr huf = wizz::FlightSearchService::reprice(*outbound, "HUF");
    EXPECT_EQ(describe(*huf->flights[0]), "BTS→LTN 2026-11-12 07:15");
    EXPECT_EQ(huf->flights[0]->price_per_person_converted, Decimal{5491});  // 14.99 × 366.3 = 5490.837
    EXPECT_EQ(huf->flights[0]->total_converted, Decimal{10982});
    EXPECT_EQ(huf->flights[1]->price_per_person_converted, Decimal{19290});
    EXPECT_EQ(huf->flights[1]->total_converted, Decimal{38580});
    const LegSearchResultPtr gbp = wizz::FlightSearchService::reprice(*outbound, "GBP");
    EXPECT_EQ(describe(*gbp->flights[0]), "BTS→LTN 2026-11-12 07:15");
    EXPECT_EQ(gbp->flights[0]->price_per_person_converted, dec("12.52"));
    EXPECT_EQ(gbp->flights[0]->total_converted, dec("25.04"));
    EXPECT_EQ(gbp->flights[1]->price_per_person_converted, dec("43.99"));
    EXPECT_EQ(gbp->flights[1]->total_converted, dec("87.98"));
    EXPECT_EQ(harness.wizz_transport->request_count(), sent);

    // Return leg: London r30 → Budapest r200, 12–20 Nov, 2 adults, EUR (synthetic GBP fares).
    const LegSearchRequest return_request = make_request(harness.place("London"), 30, harness.place("Budapest"), 200,
                                                         day(2026, 11, 12), day(2026, 11, 20), 2);
    const LegSearchResultPtr inbound = run_search(*harness.service, return_request);
    EXPECT_EQ(planned(inbound->plan), (Strings{"LGW→BUD [BUD] 2026-11-12..2026-11-20",
                                               "LTN→BUD [BUD] 2026-11-12..2026-11-20",
                                               "LTN→BTS [VBR] 2026-11-12..2026-11-20",
                                               "LTN→DEB [DEB] 2026-11-12..2026-11-20"}));
    EXPECT_EQ(describe(inbound->flights), (Strings{"LTN→BUD 2026-11-17 21:35", "LTN→BTS 2026-11-15 12:00",
                                                   "LTN→BUD 2026-11-14 10:05", "LGW→BUD 2026-11-16 14:00"}));
    EXPECT_EQ(per_person(inbound->flights), decs({"21.54", "23.93", "29.92", "35.90"}));
    EXPECT_EQ(totals(inbound->flights), decs({"43.08", "47.86", "59.84", "71.80"}));
    EXPECT_TRUE(inbound->warnings.empty());

    // MatchLegs: every outbound and every return fare matches.
    const wizz::LegMatch match = wizz::round_trips::match_legs(*outbound, *inbound);
    EXPECT_EQ(match.problem, std::nullopt);
    EXPECT_EQ(match.outbound.size(), 12u);
    EXPECT_EQ(match.inbound.size(), 4u);

    // Combine(min 2, max 5) → 19 trips; the first five.
    const std::vector<wizz::RoundTrip> trips = wizz::round_trips::combine(match.outbound, match.inbound, 2, 5);
    ASSERT_EQ(trips.size(), 19u);
    struct Expected {
        std::string_view total;
        std::string_view outbound;
        std::string_view inbound;
        int nights;
        bool same_home;
        bool same_away;
        std::optional<std::string> url;
    };
    const std::string base = "https://www.wizzair.com/en-gb/booking/select-flight";
    const std::vector<Expected> top{
        {"73.06", "BTS→LTN 2026-11-12 07:15", "LTN→BUD 2026-11-17 21:35", 5, false, true, std::nullopt},
        {"77.84", "BTS→LTN 2026-11-12 07:15", "LTN→BTS 2026-11-15 12:00", 3, true, true,
         base + "/BTS/LTN/2026-11-12/2026-11-15/2/0/0/null"},
        {"89.82", "BTS→LTN 2026-11-12 07:15", "LTN→BUD 2026-11-14 10:05", 2, false, true, std::nullopt},
        {"101.78", "BTS→LTN 2026-11-12 07:15", "LGW→BUD 2026-11-16 14:00", 4, false, false, std::nullopt},
        {"148.40", "BUD→LTN 2026-11-14 06:00", "LTN→BUD 2026-11-17 21:35", 3, true, true,
         base + "/BUD/LTN/2026-11-14/2026-11-17/2/0/0/null"},
    };
    for (std::size_t i = 0; i < top.size(); ++i) {
        SCOPED_TRACE(std::format("trip {}", i + 1));
        const wizz::RoundTrip& trip = trips[i];
        EXPECT_EQ(trip.total, dec(top[i].total));
        EXPECT_EQ(describe(*trip.outbound), top[i].outbound);
        EXPECT_EQ(describe(*trip.inbound), top[i].inbound);
        EXPECT_EQ(trip.nights, top[i].nights);
        EXPECT_EQ(trip.same_home_airport, top[i].same_home);
        EXPECT_EQ(trip.same_away_airport, top[i].same_away);
        EXPECT_EQ(trip.combined_booking_url, top[i].url);
    }
}

} // namespace
