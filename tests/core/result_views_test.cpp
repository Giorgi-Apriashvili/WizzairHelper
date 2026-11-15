// ResultViews (03 §8): 05 CT-RT-03, CT-FSS-04, the 03 §8 example from the
// fixture search, and the SortByPrice key / stability rules.

#include "wizz/core/result_views.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/pricing.hpp"

#include "fare_samples.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using wizz::Date;
using wizz::DayFare;
using wizz::Decimal;
using wizz::FareStatus;
using wizz::FlightResult;
using wizz::FlightResultPtr;
using wizz::LegSearchRequest;
namespace pricing = wizz::pricing;
namespace views = wizz::result_views;
using namespace wizz::test::samples;

/// "BTS→LTN 2026-11-10 29.98" ("-" when there is no total).
[[nodiscard]] std::string describe(const FlightResult& f)
{
    return f.fare.from + "→" + f.fare.to + " " + f.date().iso() + " " +
           (f.total_converted ? f.total_converted->to_string() : std::string{"-"});
}

[[nodiscard]] std::vector<std::string> describe(const std::vector<FlightResultPtr>& flights)
{
    std::vector<std::string> lines;
    for (const FlightResultPtr& flight : flights) {
        lines.push_back(describe(*flight));
    }
    return lines;
}

/// A hand-built result: `id` doubles as the departure code (a stub airport),
/// `to` is looked up in CAT (or stubbed); `total` nullopt = no converted price.
[[nodiscard]] FlightResultPtr flight(std::string id, std::string to, Date date, int hour, int minute,
                                     std::optional<std::string_view> total,
                                     FareStatus status = FareStatus::available)
{
    FlightResult f;
    f.fare.from = std::move(id);
    f.fare.to = std::move(to);
    f.fare.date = date;
    f.fare.cheapest_departure = wizz::make_local(date, hour, minute);
    f.fare.departures = {f.fare.cheapest_departure};
    f.fare.status = status;
    f.fare.price_per_person = wizz::Money{Decimal{}, "EUR"};
    f.from_airport = pricing::unknown_airport(f.fare.from);
    f.to_airport = cat()->find(f.fare.to);
    if (!f.to_airport) {
        f.to_airport = pricing::unknown_airport(f.fare.to);
    }
    if (total) {
        f.price_per_person_converted = dec(*total);
        f.total_converted = dec(*total);
    }
    f.target_currency = "EUR";
    return std::make_shared<const FlightResult>(std::move(f));
}

TEST(ResultViews, CT_RT_03_Views_pick_cheapest_per_day_and_destination)
{
    // Fare builder A rows (d = 2026-11-10).
    const Date d = day(11, 10);
    const std::vector<FlightResultPtr> rows{
        builder_a("BUD", "LTN", d, "30"),
        builder_a("BTS", "LTN", d, "20"),
        builder_a("BUD", "BCN", d.add_days(1), "40"),
        builder_a("BUD", "LGW", d.add_days(1), "35"),
    };
    const std::vector<FlightResultPtr> per_day = views::cheapest_per_day(rows);
    // [A] totals [20, 35]
    ASSERT_EQ(per_day.size(), 2u);
    EXPECT_EQ(per_day[0]->total_converted, Decimal{20});
    EXPECT_EQ(per_day[1]->total_converted, Decimal{35});
    // [D] PerDay is [BTS→LTN 11-10, BUD→LGW 11-11].
    EXPECT_EQ(describe(per_day), (std::vector<std::string>{"BTS→LTN 2026-11-10 20", "BUD→LGW 2026-11-11 35"}));

    const std::vector<FlightResultPtr> per_destination = views::cheapest_per_destination(rows);
    // [A] two rows (the LON group and BCN), row [0] totals 20.
    ASSERT_EQ(per_destination.size(), 2u);
    EXPECT_EQ(per_destination[0]->total_converted, Decimal{20});
    // [D] PerDest is [BTS→LTN 20, BUD→BCN 40]: LTN and LGW share the LON group.
    EXPECT_EQ(describe(per_destination),
              (std::vector<std::string>{"BTS→LTN 2026-11-10 20", "BUD→BCN 2026-11-11 40"}));
    EXPECT_EQ(per_destination[0]->to_airport->group_key(), "LON");
    EXPECT_EQ(per_destination[1]->to_airport->group_key(), "BCN");
}

TEST(ResultViews, Section_8_example_from_the_fixture_search)
{
    const std::vector<FlightResultPtr> flights = section11_flights();
    // Per day: one row per date, ascending: 11-10 BUD 168.12, 11-11 BUD 117.88, 11-12 BTS 29.98, 11-13 117.88, ...
    EXPECT_EQ(describe(views::cheapest_per_day(flights)), (std::vector<std::string>{
                                                              "BUD→LTN 2026-11-10 168.12",
                                                              "BUD→LTN 2026-11-11 117.88",
                                                              "BTS→LTN 2026-11-12 29.98",
                                                              "BUD→LTN 2026-11-13 117.88",
                                                              "BUD→LTN 2026-11-14 105.32",
                                                              "BUD→LGW 2026-11-15 189.40",
                                                              "BUD→LTN 2026-11-16 138.62",
                                                              "BUD→LTN 2026-11-17 117.88",
                                                              "BUD→LTN 2026-11-18 105.32",
                                                              "BUD→LTN 2026-11-19 105.32",
                                                              "BUD→LTN 2026-11-20 105.32",
                                                          }));
    // Per destination: a single row LON BTS→LTN 29.98.
    const std::vector<FlightResultPtr> per_destination = views::cheapest_per_destination(flights);
    ASSERT_EQ(per_destination.size(), 1u);
    EXPECT_EQ(describe(*per_destination[0]), "BTS→LTN 2026-11-12 29.98");
    EXPECT_EQ(per_destination[0]->to_airport->group_key(), "LON");
}

TEST(ResultViews, CT_FSS_04_Check_on_site_rows_have_no_converted_price_and_sort_last)
{
    LegSearchRequest request;
    request.origin = place("Budapest");
    request.from = day(11, 2);
    request.to = day(11, 5);
    std::vector<FlightResultPtr> results;
    for (const DayFare& fare : checkprice_rows()) {
        results.push_back(pricing::build_result(*cat(), request, *rates(), fare));
    }
    const std::vector<FlightResultPtr> sorted = views::sort_by_price(results);
    ASSERT_EQ(sorted.size(), 4u);
    // [A] the last row is CheckOnSite with no total.
    EXPECT_EQ(sorted.back()->fare.status, FareStatus::check_on_site);
    EXPECT_FALSE(sorted.back()->total_converted.has_value());
    EXPECT_FALSE(sorted.back()->price_per_person_converted.has_value());
    // [D] order and converted prices.
    EXPECT_EQ(describe(sorted), (std::vector<std::string>{
                                    "BUD→LTN 2026-11-05 84.06",
                                    "BUD→LGW 2026-11-04 105.08",
                                    "BUD→LTN 2026-11-03 120.91",
                                    "BUD→LTN 2026-11-02 -",
                                }));
}

TEST(ResultViews, Sort_by_price_keys_and_stability)
{
    const std::vector<FlightResultPtr> input{
        flight("A", "LTN", day(11, 12), 6, 0, "50"),
        flight("B", "LTN", day(11, 10), 6, 0, std::nullopt, FareStatus::check_on_site),
        flight("C", "LTN", day(11, 10), 6, 0, std::nullopt), // available but unconverted
        flight("D", "LTN", day(11, 11), 8, 0, "50"),
        flight("E", "LTN", day(11, 11), 6, 0, "50"),
        flight("F", "LTN", day(11, 15), 6, 0, "20"),
        flight("G", "LGW", day(11, 11), 6, 0, "50.00"), // full tie with E (scale ignored): input order
        flight("H", "LTN", day(11, 9), 6, 0, "79228162514264337593543950335"), // = DECIMAL_MAX
        flight("I", "LTN", day(11, 1), 6, 0, "0", FareStatus::check_on_site),
    };
    const std::vector<FlightResultPtr> sorted = views::sort_by_price(input);
    EXPECT_EQ(describe(sorted), (std::vector<std::string>{
                                    "F→LTN 2026-11-15 20",
                                    "E→LTN 2026-11-11 50",
                                    "G→LGW 2026-11-11 50.00",
                                    "D→LTN 2026-11-11 50",
                                    "A→LTN 2026-11-12 50",
                                    "H→LTN 2026-11-09 79228162514264337593543950335",
                                    "C→LTN 2026-11-10 -",
                                    "I→LTN 2026-11-01 0",
                                    "B→LTN 2026-11-10 -",
                                }));
    // A new list; the input keeps its order and the same instances are shared.
    EXPECT_EQ(input.front()->fare.from, "A");
    EXPECT_EQ(sorted.front().get(), input[5].get());
    // Stable: re-sorting a sorted list changes nothing.
    EXPECT_EQ(views::sort_by_price(sorted), sorted);
}

TEST(ResultViews, Cheapest_per_day_keeps_days_without_a_price_and_ascends_by_date)
{
    const std::vector<FlightResultPtr> input{
        flight("P", "LTN", day(11, 12), 6, 0, "30"),
        flight("X", "LTN", day(11, 10), 6, 0, std::nullopt, FareStatus::check_on_site),
        flight("Z", "LTN", day(11, 11), 6, 0, std::nullopt, FareStatus::check_on_site),
        flight("Y", "LTN", day(11, 11), 9, 0, std::nullopt),
        flight("Q", "LGW", day(11, 12), 9, 0, "20"),
    };
    EXPECT_EQ(describe(views::cheapest_per_day(input)), (std::vector<std::string>{
                                                            "X→LTN 2026-11-10 -",
                                                            "Y→LTN 2026-11-11 -",
                                                            "Q→LGW 2026-11-12 20",
                                                        }));
}

TEST(ResultViews, Cheapest_per_destination_groups_by_ordinal_group_key)
{
    const std::vector<FlightResultPtr> input{
        flight("A", "XYZ", day(11, 10), 6, 0, "20"),
        flight("B", "LTN", day(11, 10), 6, 0, "5"),
        flight("C", "xyz", day(11, 11), 6, 0, "10"), // stub group "xyz" differs from "XYZ" (ordinal)
        flight("D", "LGW", day(11, 12), 6, 0, "3"),  // same LON group as LTN
        flight("E", "XYZ", day(11, 12), 6, 0, "25"),
        flight("F", "BCN", day(11, 12), 6, 0, std::nullopt, FareStatus::check_on_site),
    };
    EXPECT_EQ(describe(views::cheapest_per_destination(input)), (std::vector<std::string>{
                                                                    "D→LGW 2026-11-12 3",
                                                                    "C→xyz 2026-11-11 10",
                                                                    "A→XYZ 2026-11-10 20",
                                                                    "F→BCN 2026-11-12 -",
                                                                }));
}

TEST(ResultViews, Empty_input_gives_empty_views)
{
    EXPECT_TRUE(views::sort_by_price({}).empty());
    EXPECT_TRUE(views::cheapest_per_day({}).empty());
    EXPECT_TRUE(views::cheapest_per_destination({}).empty());
}

} // namespace
