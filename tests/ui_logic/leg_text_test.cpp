// Leg texts and decisions (01 §11.8-§11.10, §12.2; 05 §7.2 U-01, U-04, U-11;
// CT-FSS-01/02/03 status lines; 03 §11 Round trips summary). Locale en-US.

#include "wizz/ui/leg_text.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/format.hpp"
#include "wizz/core/round_trips.hpp"

#include "ui_logic/ui_samples.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace wizz;
using namespace wizz::ui;
using test::ui::en;
using test::ui::section11_outbound;

[[nodiscard]] Duration seconds(double value)
{
    return Duration{static_cast<Duration::rep>(value * 10'000'000.0)};
}

[[nodiscard]] AreaAirport area(std::string_view code, double km)
{
    return AreaAirport{test::ui::station(code), km};
}

// ---------------------------------------------------------------------------
// U-01 request budget, estimate texts (01 §11.9)
// ---------------------------------------------------------------------------

TEST(LegText, U_01_Budget_confirms_61_to_500_refuses_above_500)
{
    EXPECT_EQ(request_budget(1, false), RequestBudget::proceed);
    EXPECT_EQ(request_budget(60, false), RequestBudget::proceed);
    EXPECT_EQ(request_budget(61, false), RequestBudget::confirm);
    EXPECT_EQ(request_budget(499, false), RequestBudget::confirm);
    EXPECT_EQ(request_budget(500, false), RequestBudget::confirm);  // exactly 500 is allowed
    EXPECT_EQ(request_budget(501, false), RequestBudget::refuse);
    EXPECT_EQ(request_budget(5000, false), RequestBudget::refuse);
    // autotest skips the question, the cap still applies
    EXPECT_EQ(request_budget(61, true), RequestBudget::proceed);
    EXPECT_EQ(request_budget(500, true), RequestBudget::proceed);
    EXPECT_EQ(request_budget(501, true), RequestBudget::refuse);
}

TEST(LegText, U_01_Cap_text_uses_ceil_of_the_estimate_in_minutes)
{
    EXPECT_EQ(request_cap_text(501),
              "This search would need 501 requests to Wizz Air (about 11 min). That is more than the 500 this app "
              "allows, to stay polite to the site. Use a smaller radius, fewer dates, or a specific destination.");
    // 512 × 1.3 = 665.6 s = 11.09 min → 12; 600 × 1.3 = 780 s = exactly 13 min → 13.
    EXPECT_NE(request_cap_text(512).find("(about 12 min)"), std::string::npos);
    EXPECT_NE(request_cap_text(600).find("(about 13 min)"), std::string::npos);
    EXPECT_NE(request_cap_text(1000).find("would need 1000 requests"), std::string::npos);
    EXPECT_EQ(estimate_minutes_ceiling(request_estimate(600)), 13);
    EXPECT_EQ(estimate_minutes_ceiling(request_estimate(601)), 14);
}

TEST(LegText, Estimate_is_TimeSpan_FromSeconds_of_N_times_1_3)
{
    EXPECT_EQ(request_estimate(61), Duration{793'000'000});      // 79.3 s
    EXPECT_EQ(request_estimate(512), Duration{6'656'000'000});   // 665.6 s
    EXPECT_EQ(format_duration(request_estimate(61)), "1 min 19 s");
    EXPECT_EQ(format_duration(request_estimate(512)), "11 min 5 s");
}

TEST(LegText, Big_search_question_text)
{
    EXPECT_EQ(big_search_text(61),
              "This search needs 61 requests to Wizz Air (about 1 min 19 s, longer if Wizz Air slows us down).\n\n"
              "Tip: a smaller radius or a specific destination makes it much faster.\n\nContinue?");
    EXPECT_NE(big_search_text(512).find("(about 11 min 5 s, longer"), std::string::npos);
}

TEST(LegText, Fetch_begin_text)
{
    EXPECT_EQ(fetch_begin_text("Outbound", 3), "Outbound: starting (3 requests, about 4 s)\u2026");
    // FormatDuration below a minute is ties-to-even (D-17): 6.5 s → 6 s, 19.5 s → 20 s.
    EXPECT_EQ(fetch_begin_text("Return", 5), "Return: starting (5 requests, about 6 s)\u2026");
    EXPECT_EQ(fetch_begin_text("Return", 15), "Return: starting (15 requests, about 20 s)\u2026");
    EXPECT_EQ(fetch_begin_text("Outbound", 61), "Outbound: starting (61 requests, about 1 min 19 s)\u2026");
}

TEST(LegText, Zero_request_plan_uses_the_first_note_or_nothing_to_search)
{
    SearchPlan plan;
    EXPECT_EQ(nothing_to_search_text(plan), "Nothing to search.");
    plan.notes = {"Dates before today were skipped (searching from 2026-11-02).", "second"};
    EXPECT_EQ(nothing_to_search_text(plan), "Dates before today were skipped (searching from 2026-11-02).");
}

TEST(LegText, Searching_text_counts_routes_and_origin_airports)
{
    const LegSearchResult result = section11_outbound();
    EXPECT_EQ(searching_text(result.plan), "Searching 3 route(s) from 3 airport(s)\u2026");
}

// ---------------------------------------------------------------------------
// Summary, StatusLine (01 §11.10; U-04; CT-FSS-01..03)
// ---------------------------------------------------------------------------

TEST(LegText, CT_FSS_01_Summary_and_status_line)
{
    const LegSearchResult result = section11_outbound(2s);
    EXPECT_EQ(summary(result, en()), "12 fares \u00B7 cheapest 29.98 EUR: BTS \u2192 LTN on Thu 12 Nov");
    EXPECT_EQ(status_line(result), "Outbound Done: 12 fares from 3 of 3 requests in 2 s.");
    EXPECT_TRUE(status_success(result));
    EXPECT_TRUE(extra_warnings(result).empty());
}

TEST(LegText, U_04_Status_line_durations)
{
    LegSearchResult result = section11_outbound();
    result.elapsed = seconds(59.4);
    EXPECT_EQ(status_line(result), "Outbound Done: 12 fares from 3 of 3 requests in 59 s.");
    result.elapsed = seconds(0.2);
    EXPECT_EQ(status_line(result), "Outbound Done: 12 fares from 3 of 3 requests in 1 s.");
    result.elapsed = seconds(75);
    EXPECT_EQ(status_line(result), "Outbound Done: 12 fares from 3 of 3 requests in 1 min 15 s.");
    result.elapsed = seconds(12);
    EXPECT_EQ(status_line(result), "Outbound Done: 12 fares from 3 of 3 requests in 12 s.");
}

TEST(LegText, CT_FSS_02_Stopped_early_status_keeps_partial_fares_green)
{
    LegSearchResult result = section11_outbound(0s);
    result.flights.resize(11);  // the BUD fares survive
    result.requests_completed = 1;
    result.abort_reason = "Wizz Air's bot protection blocked the request (HTTP 429 challenge). Wait a few minutes and try again.";
    result.warnings = {*result.abort_reason};
    EXPECT_EQ(status_line(result),
              "Outbound Stopped early: 11 fares from 1 of 3 requests in 1 s. Wizz Air's bot protection blocked the "
              "request (HTTP 429 challenge). Wait a few minutes and try again.");
    EXPECT_TRUE(status_success(result));  // any fares → green
    EXPECT_TRUE(summary(result, en()).ends_with(" \u00B7 stopped early"));

    result.flights.clear();
    EXPECT_FALSE(status_success(result));
    EXPECT_EQ(summary(result, en()), "No fares: " + *result.abort_reason);
}

TEST(LegText, CT_FSS_03_Cancelled_status_and_summary)
{
    LegSearchResult result = section11_outbound(9s);
    result.flights.resize(11);
    result.requests_completed = 1;
    result.cancelled = true;
    EXPECT_EQ(status_line(result), "Outbound Cancelled: 11 fares from 1 of 3 requests in 9 s.");
    EXPECT_TRUE(status_success(result));
    EXPECT_TRUE(summary(result, en()).ends_with(" \u00B7 cancelled early"));
    result.abort_reason = "x";
    EXPECT_TRUE(summary(result, en()).ends_with(" \u00B7 cancelled early \u00B7 stopped early"));

    result.flights.clear();
    result.abort_reason.reset();
    EXPECT_FALSE(status_success(result));
    EXPECT_EQ(summary(result, en()), "Cancelled before any fares arrived.");
}

TEST(LegText, Summary_without_fares_explains_why)
{
    LegSearchResult result = section11_outbound();
    result.flights.clear();
    result.routes_not_served = 3;  // >= RoutesConsidered (3)
    EXPECT_EQ(summary(result, en()),
              "Wizz Air says it does not sell these routes right now (its route map may be out of date).");
    result.routes_not_served = 2;
    EXPECT_EQ(summary(result, en()), "No flights on sale for these dates.");
    result.plan.notes = {"Dates before today were skipped (searching from 2026-11-02).", "A real note."};
    EXPECT_EQ(summary(result, en()), "A real note.");
    result.plan.notes = {"Dates before today were skipped (searching from 2026-11-02)."};
    EXPECT_EQ(summary(result, en()), "No flights on sale for these dates.");
    result.routes_not_served = 0;
    result.plan.routes_considered = 0;  // 0 >= 0 but none not served
    EXPECT_EQ(summary(result, en()), "No flights on sale for these dates.");
}

TEST(LegText, Summary_price_falls_back_to_the_fare_price_without_a_converted_total)
{
    LegSearchResult result = section11_outbound();
    FlightResult first = *result.flights.front();
    first.total_converted.reset();
    first.price_per_person_converted.reset();
    result.flights.front() = std::make_shared<const FlightResult>(first);
    EXPECT_EQ(summary(result, en()), "12 fares \u00B7 cheapest 14.99 EUR: BTS \u2192 LTN on Thu 12 Nov");
    EXPECT_EQ(price_text(*result.flights[1], en()), "105.32 EUR");
}

TEST(LegText, Status_line_counts_extra_warnings_and_plan_notes)
{
    LegSearchResult result = section11_outbound(4s);
    result.plan.notes = {"note A"};
    result.warnings = {"note A", "Exchange rates are approximate (x)", "another"};
    EXPECT_EQ(extra_warnings(result), (std::vector<std::string>{"Exchange rates are approximate (x)", "another"}));
    EXPECT_EQ(status_line(result), "Outbound Done: 12 fares from 3 of 3 requests in 4 s. 3 note(s) in the Log tab.");
    result.abort_reason = "Stopped after 4 failed requests in a row. Last error: boom";
    EXPECT_EQ(status_line(result), "Outbound Stopped early: 12 fares from 3 of 3 requests in 4 s. Stopped after 4 "
                                   "failed requests in a row. Last error: boom");
}

// ---------------------------------------------------------------------------
// ShowResolved (01 §11.10, U-11, LIVE2-6)
// ---------------------------------------------------------------------------

TEST(LegText, CT_FSS_01_Resolved_line)
{
    const LegSearchResult result = section11_outbound();
    EXPECT_EQ(show_resolved(result.request, result.plan, en()),
              "Outbound: from Budapest (BUD), Hungary \u2192 airports within 200 km: BUD 0 km, BTS 173 km, DEB 184 "
              "km.   To London, United Kingdom \u2014 Wizz Air airports: LGW, LTN \u2192 airports within 30 km: LGW 41 "
              "km, LTN 44 km.");
}

TEST(LegText, U_11_Same_area_plan_shows_not_searched_on_both_sides)
{
    LegSearchRequest request;
    request.origin = test::ui::place("Budapest");
    request.origin_radius_km = 200;
    request.destination = test::ui::place("Budapest");
    request.destination_radius_km = 100;
    request.from = test::ui::day(11, 10);
    request.to = test::ui::day(11, 20);
    request.label = "Outbound";
    const SearchPlan plan = planner::plan(*test::ui::cat(), request, test::ui::kToday);
    ASSERT_TRUE(plan.same_area);
    EXPECT_EQ(show_resolved(request, plan, en()),
              "Outbound: from Budapest (BUD), Hungary \u2192 airports within 200 km: not searched (same area).   To "
              "Budapest (BUD), Hungary \u2192 airports within 100 km: not searched (same area).");

    const std::vector<std::string> lines = log_plan_lines(request, plan, en());
    ASSERT_EQ(lines.size(), 2 + plan.notes.size());
    EXPECT_EQ(lines[0], "Outbound: Budapest (BUD), Hungary (+200 km) \u2192 Budapest (BUD), Hungary (+100 km), "
                        "2026-11-10 \u2026 2026-11-20, 1 adult(s), Regular, EUR");
    EXPECT_EQ(lines[1], "  not searched: start and destination are the same area");
    ASSERT_FALSE(plan.notes.empty());
    EXPECT_EQ(lines[2], "  note: " + plan.notes[0]);
    EXPECT_TRUE(plan.notes[0].starts_with("Start and destination are the same area (0 km apart)."));
}

TEST(LegText, Resolved_line_none_in_range_and_anywhere)
{
    LegSearchRequest request;
    request.origin = Place{"Graz", "Graz, Styria, Austria", GeoPoint{47.07, 15.44}, PlaceSource::geocoder, {}};
    request.origin_radius_km = 37.5;
    request.label = "Outbound";
    SearchPlan plan;
    EXPECT_EQ(show_resolved(request, plan, en()),
              "Outbound: from Graz, Styria, Austria \u2192 airports within 38 km: none in range.   To: anywhere.");
}

TEST(LegText, Resolved_line_example_of_01_11_10)
{
    LegSearchRequest request;
    request.origin = test::ui::place("London");
    request.origin.description += " (from \u201CLondn\u201D)";
    request.origin_radius_km = 100;
    request.destination = test::ui::place("Budapest");
    request.destination_radius_km = 250;
    request.label = "Return";
    SearchPlan plan;
    plan.origin_airports = {area("LGW", 40.5581), area("LTN", 44.0719)};
    plan.destination_airports = {area("BUD", 0), area("BTS", 172.5439), area("DEB", 184.0772)};
    EXPECT_EQ(show_resolved(request, plan, en()),
              "Return: from London, United Kingdom \u2014 Wizz Air airports: LGW, LTN (from \u201CLondn\u201D) \u2192 "
              "airports within 100 km: LGW 41 km, LTN 44 km.   To Budapest (BUD), Hungary \u2192 airports within 250 "
              "km: BUD 0 km, BTS 173 km, DEB 184 km.");
}

TEST(LegText, Distance_labels_round_15_significant_digits_then_half_away)
{
    LegSearchRequest request;
    request.origin = test::ui::place("Budapest");
    request.origin_radius_km = 0.5;  // label "1" (half away), not banker's "0"
    request.label = "Outbound";
    SearchPlan plan;
    plan.origin_airports = {area("BTS", 172.4999999999999), area("DEB", 2.5)};
    EXPECT_EQ(show_resolved(request, plan, en()),
              "Outbound: from Budapest (BUD), Hungary \u2192 airports within 1 km: BTS 173 km, DEB 3 km.   To: "
              "anywhere.");
}

// ---------------------------------------------------------------------------
// Log lines (01 §12)
// ---------------------------------------------------------------------------

TEST(LegText, Log_entry_has_a_time_stamp_and_CRLF)
{
    EXPECT_EQ(log_entry(make_local(test::ui::kToday, 10, 0, 5), "Route map: 11 airports (from the local cache).", en()),
              "[10:00:05] Route map: 11 airports (from the local cache).\r\n");
}

TEST(LegText, CT_FSS_01_LogPlan_lines)
{
    const LegSearchResult result = section11_outbound();
    EXPECT_EQ(log_plan_lines(result.request, result.plan, en()),
              (std::vector<std::string>{
                  "Outbound: Budapest (BUD), Hungary (+200 km) \u2192 London, United Kingdom \u2014 Wizz Air airports: "
                  "LGW, LTN (+30 km), 2026-11-10 \u2026 2026-11-20, 2 adult(s), Regular, EUR",
                  "  origin airports: BUD (0 km), BTS (173 km), DEB (184 km)",
                  "  destination airports: LGW (41 km), LTN (44 km)",
                  "  3 route(s), 3 request(s)",
              }));
}

TEST(LegText, LogPlan_anywhere_discount_club_and_notes)
{
    LegSearchRequest request;
    request.origin = test::ui::place("Budapest");
    request.origin_radius_km = 0;
    request.from = Date::from_ymd(2026, 10, 30);  // partly in the past → note A1
    request.to = test::ui::day(11, 20);
    request.adults = 3;
    request.price_type = PriceType::wizz_discount_club;
    request.currency = "HUF";
    request.label = "Outbound";
    const SearchPlan plan = planner::plan(*test::ui::cat(), request, test::ui::kToday);
    const std::vector<std::string> lines = log_plan_lines(request, plan, en());
    ASSERT_GE(lines.size(), 3u);
    EXPECT_EQ(lines[0], "Outbound: Budapest (BUD), Hungary (+0 km) \u2192 anywhere, 2026-11-02 \u2026 2026-11-20, 3 "
                        "adult(s), WizzDiscountClub, HUF");
    EXPECT_EQ(lines[1], "  origin airports: BUD (0 km)");
    EXPECT_TRUE(lines[2].starts_with("  ") && lines[2].ends_with(" request(s)"));  // no destination line
    ASSERT_FALSE(plan.notes.empty());
    EXPECT_EQ(lines.back(), "  note: " + plan.notes.back());
    EXPECT_EQ(lines[3], "  note: Dates before today were skipped (searching from 2026-11-02).");
}

TEST(LegText, CT_FSS_01_LogResult_lines)
{
    LegSearchResult result = section11_outbound(2s);
    EXPECT_EQ(log_result_lines(result, en()),
              (std::vector<std::string>{
                  "Outbound: 12 fares, 3/3 requests, 1 route(s) not served, 2 s, FX Frankfurter 2026-11-02",
                  "    Thu 12 Nov 07:15  BTS\u2192LTN  14.99 EUR  = 29.98 EUR",
                  "    Sat 14 Nov 06:00  BUD\u2192LTN  19,290 HUF  = 105.32 EUR",
                  "    Wed 18 Nov 06:00  BUD\u2192LTN  19,290 HUF  = 105.32 EUR",
                  "    Thu 19 Nov 12:50  BUD\u2192LTN  19,290 HUF  = 105.32 EUR",
                  "    Fri 20 Nov 06:00  BUD\u2192LTN  19,290 HUF  = 105.32 EUR",
              }));

    auto approximate = std::make_shared<FxRates>(*result.rates);
    approximate->is_approximate = true;
    approximate->source = "built-in table";
    result.rates = approximate;
    result.plan.notes = {"a plan note"};
    result.warnings = {"a plan note", "Exchange rates are approximate (built-in table, 2026-11-02); converted prices "
                                      "may be off by a few percent."};
    result.flights.resize(1);
    EXPECT_EQ(log_result_lines(result, en()),
              (std::vector<std::string>{
                  "Outbound: 1 fares, 3/3 requests, 1 route(s) not served, 2 s, FX built-in table 2026-11-02 "
                  "(approximate)",
                  "  ! Exchange rates are approximate (built-in table, 2026-11-02); converted prices may be off by a "
                  "few percent.",
                  "    Thu 12 Nov 07:15  BTS\u2192LTN  14.99 EUR  = 29.98 EUR",
              }));
}

TEST(LegText, UX_18_Notices_are_logged_once_when_they_matter)
{
    NoticeLog log{"Outbound"};
    SearchProgress progress;
    progress.stage = "Fetching fares";
    progress.detail = "BUD \u2192 London (LON)   10 Nov \u2013 20 Nov";
    EXPECT_FALSE(log.line_for(progress));  // plain route detail

    progress.detail = "Wizz Air is busy (HTTP 503) \u2014 retry 1 of 3";
    progress.waiting_until_utc = FixedClock::kDefaultUtcNow + 5s;
    EXPECT_EQ(log.line_for(progress), "  Outbound: Wizz Air is busy (HTTP 503) \u2014 retry 1 of 3");
    EXPECT_FALSE(log.line_for(progress));  // once per distinct text per run

    progress.waiting_until_utc.reset();
    progress.detail = "Wizz Air updated its website \u2014 finding the new API version";
    EXPECT_EQ(log.line_for(progress), "  Outbound: Wizz Air updated its website \u2014 finding the new API version");
    progress.detail = "Exchange-rate services unreachable \u2014 using cached rates";
    EXPECT_EQ(log.line_for(progress), "  Outbound: Exchange-rate services unreachable \u2014 using cached rates");
    progress.detail = "Retrying (1 of 3)";
    EXPECT_FALSE(log.line_for(progress));
    progress.detail = "Using the CACHED copy";  // ordinal, case-sensitive
    EXPECT_FALSE(log.line_for(progress));
}

// ---------------------------------------------------------------------------
// Round trips summary (01 §11.8; 03 §11)
// ---------------------------------------------------------------------------

TEST(LegText, Worked_example_03_11_round_trip_summary)
{
    const LegSearchResult out = section11_outbound();
    const LegSearchResult ret = test::ui::section11_return();
    const LegMatch match = round_trips::match_legs(out, ret);
    ASSERT_FALSE(match.problem);
    const std::vector<RoundTrip> trips = round_trips::combine(match.outbound, match.inbound, 2, 5);
    ASSERT_EQ(trips.size(), 19u);
    EXPECT_EQ(round_trip_summary(trips, 2, 5, "EUR", en()),
              "19 combinations \u00B7 cheapest 73.06 EUR: BTS\u2192LTN Thu 12 Nov, back LTN\u2192BUD Tue 17 Nov (5 "
              "nights)");
    EXPECT_EQ(round_trip_summary({}, 2, 10, "EUR", en()),
              "No combination with a stay of 2\u201310 nights. Change the nights or the dates.");
}

TEST(LegText, Round_trip_summary_always_says_nights_and_formats_the_currency)
{
    const LegSearchResult out = section11_outbound();
    const LegSearchResult ret = test::ui::section11_return();
    const LegMatch match = round_trips::match_legs(out, ret);
    std::vector<RoundTrip> trips = round_trips::combine(match.outbound, match.inbound, 2, 2);
    ASSERT_FALSE(trips.empty());
    EXPECT_TRUE(round_trip_summary(trips, 2, 2, "EUR", en()).ends_with("(2 nights)"));
    RoundTrip one = trips.front();
    one.nights = 1;
    one.total = test::ui::dec("12345.5");
    EXPECT_EQ(round_trip_summary(std::span<const RoundTrip>{&one, 1}, 1, 1, "HUF", en()),
              "1 combinations \u00B7 cheapest 12,346 HUF: BTS\u2192LTN Thu 12 Nov, back LTN\u2192BUD Sat 14 Nov (1 "
              "nights)");
}

} // namespace
