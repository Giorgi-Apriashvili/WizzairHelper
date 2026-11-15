// summary.json of --autotest (01 §17.2, §17.3): Describe, DescribeTrips, the
// value formats and the top-level key order. Consumers compare the parsed JSON.

#include "wizz/ui/summary_json.hpp"

#include "wizz/core/json.hpp"
#include "wizz/core/round_trips.hpp"

#include "ui_logic/ui_samples.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace wizz;
using namespace wizz::ui;
using test::ui::en;

[[nodiscard]] std::vector<std::string> keys(const json::Value& object)
{
    std::vector<std::string> names;
    for (const json::Member& member : object.as_object()) {
        names.emplace_back(member.name());
    }
    return names;
}

[[nodiscard]] std::vector<std::string> strings_of(const json::Value& array)
{
    std::vector<std::string> values;
    for (const json::Value& value : array.as_array()) {
        values.emplace_back(value.as_string());
    }
    return values;
}

TEST(SummaryJson, Elapsed_seconds_round_to_one_decimal_ties_to_even)
{
    EXPECT_EQ(elapsed_seconds_value(Duration{2'500'000}), 0.2);     // 0.25 s
    EXPECT_EQ(elapsed_seconds_value(Duration{7'500'000}), 0.8);     // 0.75 s
    EXPECT_EQ(elapsed_seconds_value(Duration{122'500'000}), 12.2);  // 12.25 s
    EXPECT_EQ(elapsed_seconds_value(Duration{123'400'000}), 12.3);
    EXPECT_EQ(elapsed_seconds_value(Duration{0}), 0.0);
}

TEST(SummaryJson, Airport_entries_have_no_space_before_km)
{
    EXPECT_EQ(summary_airport(AreaAirport{test::ui::station("BTS"), 172.5439}, en()), "BTS 173km");
    EXPECT_EQ(summary_airport(AreaAirport{test::ui::station("BUD"), 0}, en()), "BUD 0km");
    EXPECT_EQ(started_at_text(make_local(test::ui::kToday, 10, 0, 7)), "2026-11-02T10:00:07");
}

TEST(SummaryJson, Describe_a_leg)
{
    const LegSearchResult result = test::ui::section11_outbound(Duration{22'500'000});  // 2.25 s
    FlightGridModel grid{Locale::en_us()};
    grid.clear("EUR", false);
    grid.set_results(result.flights, "EUR", "");
    const LegDescription description = describe(&result, grid, 7, en());
    ASSERT_TRUE(std::holds_alternative<LegJson>(description));
    const LegJson& leg = std::get<LegJson>(description);
    EXPECT_EQ(leg.flights, 12u);
    EXPECT_EQ(leg.grid_rows, 12u);
    EXPECT_EQ(leg.requests_planned, 3u);
    EXPECT_EQ(leg.requests_completed, 3);
    EXPECT_FALSE(leg.cancelled);
    EXPECT_FALSE(leg.abort_reason);
    EXPECT_EQ(leg.elapsed_seconds, 2.2);
    EXPECT_EQ(leg.progress_updates, 7);
    EXPECT_EQ(leg.origin_airports, (std::vector<std::string>{"BUD 0km", "BTS 173km", "DEB 184km"}));
    EXPECT_EQ(leg.destination_airports, (std::vector<std::string>{"LGW 41km", "LTN 44km"}));
    EXPECT_TRUE(leg.warnings.empty());
    EXPECT_EQ(leg.fx, "Frankfurter 2026-11-02");
    EXPECT_EQ(leg.view, "AllFares");
    EXPECT_EQ(leg.current_row, "2026-11-12 BTS->LTN 29.98");
    ASSERT_EQ(leg.top_rows.size(), 8u);
    EXPECT_EQ(leg.top_rows[0], "2026-11-12 07:15 BTS  Bratislava -> LTN  London Luton total=29.98 local=14.99 EUR");

    EXPECT_TRUE(std::holds_alternative<NoResult>(describe(nullptr, grid, 0, en())));
}

TEST(SummaryJson, Describe_trips)
{
    const LegSearchResult out = test::ui::section11_outbound();
    const LegSearchResult ret = test::ui::section11_return();
    const LegMatch match = round_trips::match_legs(out, ret);
    RoundTripGridModel grid{Locale::en_us()};
    grid.set_trips(round_trips::combine(match.outbound, match.inbound, 2, 5), "EUR", "19 combinations \u00B7 \u2026");
    const TripsJson trips = describe_trips(grid, en());
    EXPECT_EQ(trips.count, 19u);
    EXPECT_EQ(trips.summary, "19 combinations \u00B7 \u2026");
    EXPECT_EQ(trips.top, (std::vector<std::string>{
                             "73.06 EUR: 2026-11-12 BTS->LTN + 2026-11-17 LTN->BUD (5 nights)",
                             "77.84 EUR: 2026-11-12 BTS->LTN + 2026-11-15 LTN->BTS (3 nights)",
                             "89.82 EUR: 2026-11-12 BTS->LTN + 2026-11-14 LTN->BUD (2 nights)",
                             "101.78 EUR: 2026-11-12 BTS->LTN + 2026-11-16 LGW->BUD (4 nights)",
                             "148.4 EUR: 2026-11-14 BUD->LTN + 2026-11-17 LTN->BUD (3 nights)",
                         }));
}

TEST(SummaryJson, Trip_total_uses_the_0_hash_hash_format)
{
    RoundTrip trip = round_trips::combine(std::vector<FlightResultPtr>{test::samples::builder_a("BUD", "LTN", test::ui::day(11, 10), "20")},
                                          std::vector<FlightResultPtr>{test::samples::builder_a("LTN", "BUD", test::ui::day(11, 13), "5")},
                                          2, 5)
                         .front();
    EXPECT_EQ(describe_trip(trip, "EUR", en()), "25 EUR: 2026-11-10 BUD->LTN + 2026-11-13 LTN->BUD (3 nights)");
    trip.total = test::ui::dec("1234.567");
    EXPECT_EQ(describe_trip(trip, "EUR", en()), "1234.57 EUR: 2026-11-10 BUD->LTN + 2026-11-13 LTN->BUD (3 nights)");
    EXPECT_EQ(describe_trip(trip, "EUR", *Locale::create("de-DE")),
              "1234,57 EUR: 2026-11-10 BUD->LTN + 2026-11-13 LTN->BUD (3 nights)");
}

TEST(SummaryJson, Log_split_on_CRLF_without_empty_lines)
{
    EXPECT_EQ(split_log("[10:00:00] a\r\n\r\n[10:00:01] b\r\n"),
              (std::vector<std::string>{"[10:00:00] a", "[10:00:01] b"}));
    EXPECT_EQ(split_log("one\ntwo\r\nthree"), (std::vector<std::string>{"one\ntwo", "three"}));
    EXPECT_TRUE(split_log("").empty());
    EXPECT_EQ(split_log(" \r\n"), (std::vector<std::string>{" "}));
}

TEST(SummaryJson, Full_document_key_order_and_values)
{
    AutoTestSummary summary;
    summary.started_at = "2026-11-02T10:00:00";
    summary.api_url = "https://be.wizzair.com/29.23.1/Api";
    summary.airports = 11;
    LegJson leg;
    leg.flights = 12;
    leg.grid_rows = 12;
    leg.requests_planned = 3;
    leg.requests_completed = 3;
    leg.elapsed_seconds = 2.2;
    leg.progress_updates = 7;
    leg.origin_airports = {"BUD 0km"};
    leg.fx = "Frankfurter 2026-11-02";
    leg.view = "AllFares";
    leg.current_row = "2026-11-12 BTS->LTN 29.98";
    leg.top_rows = {"2026-11-12 07:15 BTS  Bratislava -> LTN  London Luton total=29.98 local=14.99 EUR"};
    summary.outbound = leg;
    summary.inbound = NoResult{};
    summary.round_trips = TripsJson{0, {}, "Search outbound and return flights to see the cheapest combinations here."};
    LegJson switched = leg;
    switched.progress_updates = 0;
    summary.after_currency_switch = CurrencySwitchJson{"HUF", switched, TripsJson{}};
    summary.exception = "System.Exception: boom";
    summary.progress_screenshots = {"Fetching fares:  BUD \u2192 London (LON)   10 Nov \u2013 20 Nov    (1 of 3)"};
    summary.final_status = "Outbound Done: 12 fares from 3 of 3 requests in 2 s.";
    summary.requests_to_wizz = 5;
    summary.log = {"[10:00:00] WizzairHelper started."};

    const std::string text = to_json(summary);
    EXPECT_NE(text.find("\r\n  \"startedAt\": \"2026-11-02T10:00:00\","), std::string::npos);  // 2-space indent
    EXPECT_NE(text.find("\\u2192"), std::string::npos);  // JsonSerializer escaping of →
    EXPECT_NE(text.find("\\u003E"), std::string::npos);  // and of '>'

    const json::Value root = json::parse(text);
    EXPECT_EQ(keys(root), (std::vector<std::string>{"startedAt", "apiUrl", "airports", "outbound", "return",
                                                    "roundTrips", "afterCurrencySwitch", "exception",
                                                    "progressScreenshots", "finalStatus", "requestsToWizz", "log"}));
    const json::Value& outbound = *root.find("outbound");
    EXPECT_EQ(keys(outbound), (std::vector<std::string>{"flights", "gridRows", "requestsPlanned", "requestsCompleted",
                                                        "cancelled", "abortReason", "elapsedSeconds", "progressUpdates",
                                                        "originAirports", "destinationAirports", "warnings", "fx",
                                                        "view", "currentRow", "topRows"}));
    EXPECT_EQ(outbound.find("flights")->number_lexeme(), "12");
    EXPECT_EQ(outbound.find("elapsedSeconds")->number_lexeme(), "2.2");
    EXPECT_TRUE(outbound.find("abortReason")->is_null());
    EXPECT_FALSE(outbound.find("cancelled")->as_bool());
    EXPECT_EQ(strings_of(*outbound.find("originAirports")), (std::vector<std::string>{"BUD 0km"}));
    EXPECT_EQ(strings_of(*outbound.find("topRows")), leg.top_rows);
    EXPECT_EQ(root.find("return")->get_string("error"), "no result");
    EXPECT_EQ(keys(*root.find("return")), (std::vector<std::string>{"error"}));
    EXPECT_EQ(keys(*root.find("roundTrips")), (std::vector<std::string>{"count", "top", "summary"}));
    const json::Value& after = *root.find("afterCurrencySwitch");
    EXPECT_EQ(keys(after), (std::vector<std::string>{"currency", "outbound", "roundTrips"}));
    EXPECT_EQ(after.find("outbound")->find("progressUpdates")->number_lexeme(), "0");
    EXPECT_EQ(root.get_string("apiUrl"), "https://be.wizzair.com/29.23.1/Api");
    EXPECT_EQ(root.find("airports")->number_lexeme(), "11");
    EXPECT_EQ(strings_of(*root.find("progressScreenshots")), summary.progress_screenshots);
    EXPECT_EQ(root.get_string("finalStatus"), summary.final_status);
    EXPECT_EQ(root.find("requestsToWizz")->number_lexeme(), "5");
    EXPECT_EQ(strings_of(*root.find("log")), summary.log);
}

TEST(SummaryJson, Optional_sections_are_omitted_and_nulls_written)
{
    AutoTestSummary summary;
    summary.started_at = "2026-11-02T10:00:00";
    summary.outbound = NoResult{};
    summary.final_status = "Cancelled. The route map is needed for searching \u2014 press a Search button to try again.";
    const json::Value root = json::parse(to_json(summary));
    EXPECT_EQ(keys(root), (std::vector<std::string>{"startedAt", "apiUrl", "airports", "outbound", "progressScreenshots",
                                                    "finalStatus", "requestsToWizz", "log"}));
    EXPECT_TRUE(root.find("apiUrl")->is_null());
    EXPECT_TRUE(root.find("airports")->is_null());
    EXPECT_TRUE(root.find("progressScreenshots")->as_array().empty());
    EXPECT_EQ(root.get_string("finalStatus"), summary.final_status);
}

} // namespace
