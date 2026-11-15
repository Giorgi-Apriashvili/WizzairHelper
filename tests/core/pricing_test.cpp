// Pricing: BuildResult and Reprice (03 §6.1–§6.3, 02 §2.11). 05 CT-SRCH-07,
// P-27, the pricing part of CT-FSS-01 / CT-SRCH-06, fare builder A (05 §1.7),
// the 03 §6.2 examples and the full 03 §11 conversion tables (outbound in EUR,
// re-priced to HUF and GBP, and the synthetic GBP return leg). Locale en-US.

#include "wizz/core/pricing.hpp"

#include "wizz/core/currency_format.hpp"
#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/fx_rates.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/result_views.hpp"

#include "fare_samples.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wizz::AirportPtr;
using wizz::Date;
using wizz::DayFare;
using wizz::Decimal;
using wizz::FareStatus;
using wizz::FlightResult;
using wizz::FlightResultPtr;
using wizz::FxRates;
using wizz::GeoPoint;
using wizz::LegSearchRequest;
using wizz::LegSearchResult;
using wizz::LegSearchResultPtr;
using wizz::Place;
namespace pricing = wizz::pricing;
namespace views = wizz::result_views;
using namespace wizz::test::samples;

/// "BTS→LTN 2026-11-12 07:15 14.99/29.98" ("-" for a missing price).
[[nodiscard]] std::string describe(const FlightResult& f)
{
    const auto text = [](const std::optional<Decimal>& value) { return value ? value->to_string() : std::string{"-"}; };
    return f.fare.from + "→" + f.fare.to + " " + f.date().iso() + " " + wizz::format_hhmm(f.fare.cheapest_departure) +
           " " + text(f.price_per_person_converted) + "/" + text(f.total_converted);
}

[[nodiscard]] std::vector<std::string> describe(const std::vector<FlightResultPtr>& flights)
{
    std::vector<std::string> lines;
    for (const FlightResultPtr& flight : flights) {
        lines.push_back(describe(*flight));
    }
    return lines;
}

/// A request from `origin` to `destination` (nullopt = anywhere) in `currency`.
[[nodiscard]] LegSearchRequest request_for(std::string_view origin, std::optional<std::string_view> destination,
                                           int adults, std::string currency)
{
    LegSearchRequest request;
    request.origin = place(origin);
    if (destination) {
        request.destination = place(*destination);
    }
    request.from = day(11, 10);
    request.to = day(11, 20);
    request.adults = adults;
    request.currency = std::move(currency);
    return request;
}

[[nodiscard]] LegSearchResult leg_result(LegSearchRequest request, std::vector<FlightResultPtr> flights)
{
    LegSearchResult result;
    result.request = std::move(request);
    result.flights = std::move(flights);
    result.rates = rates();
    return result;
}

// ---------------------------------------------------------------------------
// BuildResult (03 §6.1, 02 §2.11)
// ---------------------------------------------------------------------------

TEST(Pricing, BuildResult_fills_every_field_02_2_11)
{
    const LegSearchRequest request = budapest_to_london();
    const pricing::CodeSet destinations = london_destination_iatas();
    const DayFare fare = b1_row();
    const FlightResultPtr result = pricing::build_result(*cat(), request, *rates(), fare, nullptr, &destinations);
    ASSERT_NE(result, nullptr);

    // Fare copied as given.
    EXPECT_EQ(result->fare.from, "BTS");
    EXPECT_EQ(result->fare.to, "LTN");
    EXPECT_EQ(result->date(), day(11, 12));
    EXPECT_EQ(result->fare.cheapest_departure, wizz::make_local(day(11, 12), 7, 15));
    EXPECT_EQ(result->fare.departures, fare.departures);
    EXPECT_EQ(result->fare.price_per_person, (wizz::Money{dec("14.99"), "EUR"}));
    EXPECT_TRUE(result->is_available());
    // Airports are the catalog's own instances.
    EXPECT_EQ(result->from_airport.get(), station("BTS").get());
    EXPECT_EQ(result->to_airport.get(), station("LTN").get());
    // Distances from the place centres (05 §2.3: BTS 172.5439 from BUD, LTN 44.0719 from LON).
    EXPECT_EQ(result->from_distance_km, wizz::distance_km(request.origin.location, station("BTS")->location));
    EXPECT_NEAR(result->from_distance_km, 172.5439, 1e-4);
    ASSERT_TRUE(result->to_distance_km.has_value());
    EXPECT_EQ(*result->to_distance_km, wizz::distance_km(request.destination->location, station("LTN")->location));
    EXPECT_NEAR(*result->to_distance_km, 44.0719, 1e-4);
    EXPECT_FALSE(result->outside_destination_radius);
    EXPECT_EQ(result->adults, 2);
    EXPECT_EQ(result->target_currency, "EUR");
    EXPECT_EQ(result->price_per_person_converted, dec("14.99"));
    EXPECT_EQ(result->total_converted, dec("29.98"));
    EXPECT_EQ(result->booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/null/2/0/0/null");
}

TEST(Pricing, BuildResult_uses_a_stub_for_an_unknown_code)
{
    const LegSearchRequest request = budapest_to_london();
    const pricing::CodeSet destinations = london_destination_iatas();
    const DayFare fare = day_fare("XXX", "YYY", day(11, 12), {9, 0}, {{9, 0}}, "10", "EUR");
    const FlightResultPtr result = pricing::build_result(*cat(), request, *rates(), fare, nullptr, &destinations);

    const wizz::Airport& from = *result->from_airport;
    EXPECT_EQ(from.iata, "XXX");
    EXPECT_EQ(from.name, "XXX");
    EXPECT_EQ(from.country_name, "");
    EXPECT_EQ(from.country_code, "");
    EXPECT_EQ(from.currency_code, "");
    EXPECT_EQ(from.location, (GeoPoint{0, 0}));
    EXPECT_FALSE(from.mac.has_value());
    EXPECT_FALSE(from.is_fake_station);
    EXPECT_FALSE(from.has_city_group);
    EXPECT_TRUE(from.connections.empty());
    EXPECT_EQ(from.display(), "XXX (XXX)");
    EXPECT_EQ(result->to_airport->iata, "YYY");
    // Meaningless but finite distances from (0, 0).
    EXPECT_EQ(result->from_distance_km, wizz::distance_km(request.origin.location, GeoPoint{}));
    EXPECT_TRUE(std::isfinite(result->from_distance_km));
    EXPECT_EQ(*result->to_distance_km, wizz::distance_km(request.destination->location, GeoPoint{}));
    // YYY is not a destination airport of the plan.
    EXPECT_TRUE(result->outside_destination_radius);
    EXPECT_EQ(result->total_converted, Decimal{20});

    // The stub keeps the code exactly as given.
    const AirportPtr stub = pricing::unknown_airport("abc");
    EXPECT_EQ(stub->iata, "abc");
    EXPECT_EQ(stub->name, "abc");
    EXPECT_EQ(stub->group_key(), "abc");
}

TEST(Pricing, BuildResult_anywhere_has_no_destination_distance_and_is_never_outside)
{
    const LegSearchRequest request = request_for("Budapest", std::nullopt, 1, "EUR");
    const pricing::CodeSet destinations{"LGW"};
    const DayFare fare = bud_ltn_rows().at(4); // 11-14 19290 HUF
    const FlightResultPtr result = pricing::build_result(*cat(), request, *rates(), fare, nullptr, &destinations);
    EXPECT_FALSE(result->to_distance_km.has_value());
    EXPECT_FALSE(result->outside_destination_radius);
    EXPECT_EQ(result->from_distance_km, 0.0); // the place sits at BUD
    EXPECT_EQ(result->price_per_person_converted, dec("52.66"));
    EXPECT_EQ(result->total_converted, dec("52.66"));
}

TEST(Pricing, BuildResult_clamps_adults_in_prices_and_links)
{
    const DayFare fare = bud_ltn_rows().at(4); // 19290 HUF on 11-14
    for (const int raw : {0, -2}) {
        const FlightResultPtr result =
            pricing::build_result(*cat(), request_for("Budapest", "London", raw, "EUR"), *rates(), fare);
        EXPECT_EQ(result->adults, 1) << raw;
        EXPECT_EQ(result->price_per_person_converted, dec("52.66"));
        EXPECT_EQ(result->total_converted, dec("52.66"));
        EXPECT_TRUE(result->booking_url.ends_with("/2026-11-14/null/1/0/0/null")) << result->booking_url;
    }
    const FlightResultPtr three =
        pricing::build_result(*cat(), request_for("Budapest", "London", 3, "EUR"), *rates(), fare);
    EXPECT_EQ(three->adults, 3);
    EXPECT_EQ(three->total_converted->to_string(), "157.98");
    EXPECT_TRUE(three->booking_url.ends_with("/null/3/0/0/null"));
}

TEST(Pricing, BuildResult_flags_arrivals_outside_the_destination_area)
{
    // P-18 shape: Budapest r0 → "London Luton" r0; the plan's destinations are {LTN}.
    LegSearchRequest request = request_for("Budapest", "London Luton", 1, "EUR");
    request.origin_radius_km = 0;
    request.destination_radius_km = 0;
    const pricing::CodeSet destinations{"LTN"};
    const DayFare lgw = bud_ltn_rows().at(5); // the 11-15 BUD→LGW row
    const FlightResultPtr outside = pricing::build_result(*cat(), request, *rates(), lgw, nullptr, &destinations);
    EXPECT_TRUE(outside->outside_destination_radius);
    EXPECT_NEAR(*outside->to_distance_km, 81.7358, 1e-4);

    const DayFare ltn = bud_ltn_rows().at(4);
    EXPECT_FALSE(pricing::build_result(*cat(), request, *rates(), ltn, nullptr, &destinations)->outside_destination_radius);
    EXPECT_EQ(*pricing::build_result(*cat(), request, *rates(), ltn, nullptr, &destinations)->to_distance_km, 0.0);
    // Membership is OrdinalIgnoreCase.
    DayFare lower = ltn;
    lower.to = "ltn";
    EXPECT_FALSE(pricing::build_result(*cat(), request, *rates(), lower, nullptr, &destinations)->outside_destination_radius);
    // Without the destination set (unit tests) it is always false.
    EXPECT_FALSE(pricing::build_result(*cat(), request, *rates(), lgw)->outside_destination_radius);
}

TEST(Pricing, Fare_builder_A_consequences_05_1_7)
{
    const FlightResultPtr result = builder_a("BUD", "LTN", day(11, 10), "20");
    EXPECT_EQ(result->total_converted, Decimal{20});
    EXPECT_EQ(result->price_per_person_converted, Decimal{20});
    EXPECT_EQ(result->from_distance_km, 0.0);
    EXPECT_FALSE(result->to_distance_km.has_value());
    EXPECT_FALSE(result->outside_destination_radius);
    EXPECT_EQ(result->adults, 1);
    EXPECT_EQ(result->target_currency, "EUR");
    // EUR to EUR is still rounded to 2 decimals, halves away from zero.
    EXPECT_EQ(builder_a("BUD", "LTN", day(11, 10), "20.005")->total_converted->to_string(), "20.01");
}

TEST(Pricing, Missing_fare_currencies_are_recorded_once_in_first_seen_order)
{
    const LegSearchRequest request = request_for("Budapest", std::nullopt, 1, "EUR");
    pricing::OrderedCodeSet missing;
    const auto build = [&](std::string currency, std::string_view amount, FareStatus status = FareStatus::available) {
        const DayFare fare = day_fare("BUD", "LTN", day(11, 14), {6, 0}, {{6, 0}}, amount, std::move(currency), status);
        return pricing::build_result(*cat(), request, *rates(), fare, &missing);
    };
    EXPECT_FALSE(build("XYZ", "1000")->price_per_person_converted.has_value());
    EXPECT_FALSE(build("xyz", "1000")->price_per_person_converted.has_value()); // same code, ci
    EXPECT_FALSE(build("ABC", "0", FareStatus::check_on_site)->price_per_person_converted.has_value());
    EXPECT_FALSE(build("", "5")->price_per_person_converted.has_value()); // empty code counts too (03 §5.8)
    EXPECT_TRUE(build("HUF", "19290")->price_per_person_converted.has_value());
    EXPECT_FALSE(build("ABC", "7")->price_per_person_converted.has_value());
    EXPECT_EQ(missing.items(), (std::vector<std::string>{"XYZ", "", "ABC"}));

    // An unsupported target currency records nothing (W3 covers it instead).
    pricing::OrderedCodeSet none;
    const LegSearchRequest unsupported = request_for("Budapest", std::nullopt, 1, "QQQ");
    const DayFare xyz = day_fare("BUD", "LTN", day(11, 14), {6, 0}, {{6, 0}}, "1000", "XYZ");
    const FlightResultPtr result = pricing::build_result(*cat(), unsupported, *rates(), xyz, &none);
    EXPECT_FALSE(result->price_per_person_converted.has_value());
    EXPECT_EQ(result->target_currency, "QQQ");
    EXPECT_TRUE(none.empty());
}

TEST(Pricing, Section_6_2_convert_then_round_per_person_then_multiply)
{
    // 19290 HUF → EUR: 52.661752661752661752661752662 → 52.66 → ×2 = 105.32.
    const DayFare huf = day_fare("BUD", "LTN", day(11, 14), {6, 0}, {{6, 0}}, "19290", "HUF");
    const FlightResultPtr eur = pricing::build_result(*cat(), request_for("Budapest", "London", 2, "EUR"), *rates(), huf);
    EXPECT_EQ(eur->price_per_person_converted->to_string(), "52.66");
    EXPECT_EQ(eur->total_converted->to_string(), "105.32");
    // 14.99 EUR → HUF: 5490.837 → 5491 → ×2 = 10982.
    const DayFare euro = b1_row();
    const FlightResultPtr forint = pricing::build_result(*cat(), request_for("Bratislava", "London", 2, "huf"), *rates(), euro);
    EXPECT_EQ(forint->target_currency, "HUF");
    EXPECT_EQ(forint->price_per_person_converted->to_string(), "5491");
    EXPECT_EQ(forint->total_converted->to_string(), "10982");
    // A fare in the target currency is converted exactly and still rounded to its decimals.
    const FlightResultPtr same = pricing::reprice(*eur, *rates(), "HUF");
    EXPECT_EQ(same->price_per_person_converted->to_string(), "19290");
    EXPECT_EQ(same->total_converted->to_string(), "38580");
    const DayFare half = day_fare("BUD", "LTN", day(11, 14), {6, 0}, {{6, 0}}, "19290.5", "HUF");
    const FlightResultPtr rounded =
        pricing::build_result(*cat(), request_for("Budapest", "London", 2, "HUF"), *rates(), half);
    EXPECT_EQ(rounded->price_per_person_converted->to_string(), "19291");
    EXPECT_EQ(rounded->total_converted->to_string(), "38582");
}

TEST(Pricing, P_27_Rounding_mode_is_away_from_zero)
{
    FxRates p27;
    p27.per_eur = {{"XXX", dec("0.5")}, {"HUF", dec("0.5")}};
    const auto price = [&](std::string_view eur, std::string target) {
        const DayFare fare = day_fare("BUD", "LTN", day(11, 14), {6, 0}, {{6, 0}}, eur, "EUR");
        return pricing::build_result(*cat(), request_for("Budapest", std::nullopt, 1, std::move(target)), p27, fare);
    };
    // 2.01 × 0.5 = 1.005 → 1.01 (banker's rounding would give 1.00).
    EXPECT_EQ(price("2.01", "XXX")->price_per_person_converted->to_string(), "1.01");
    // 5 × 0.5 = 2.5 HUF → 3 (banker's: 2); 7 × 0.5 = 3.5 → 4.
    EXPECT_EQ(price("5", "HUF")->price_per_person_converted->to_string(), "3");
    EXPECT_EQ(price("7", "HUF")->price_per_person_converted->to_string(), "4");
    // The same through Reprice.
    const FlightResultPtr eur = price("2.01", "EUR");
    EXPECT_EQ(pricing::reprice(*eur, p27, "xxx")->total_converted->to_string(), "1.01");
}

TEST(Pricing, Check_on_site_fares_get_no_converted_price)
{
    const DayFare fare = checkprice_rows().at(0);
    pricing::OrderedCodeSet missing;
    const FlightResultPtr result =
        pricing::build_result(*cat(), request_for("Budapest", std::nullopt, 1, "EUR"), *rates(), fare, &missing);
    EXPECT_FALSE(result->is_available());
    EXPECT_FALSE(result->price_per_person_converted.has_value());
    EXPECT_FALSE(result->total_converted.has_value());
    EXPECT_EQ(result->target_currency, "EUR");
    EXPECT_EQ(result->booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-02/null/1/0/0/null");
    EXPECT_TRUE(missing.empty());
    const FlightResultPtr huf = pricing::reprice(*result, *rates(), "HUF");
    EXPECT_FALSE(huf->total_converted.has_value());
    EXPECT_EQ(huf->target_currency, "HUF");
}

// ---------------------------------------------------------------------------
// 03 §11 worked example (CT-FSS-01 pricing part)
// ---------------------------------------------------------------------------

TEST(Pricing, Section_11_conversion_table_and_result_fields)
{
    pricing::OrderedCodeSet missing;
    const std::vector<FlightResultPtr> sorted = views::sort_by_price(section11_flights(&missing));
    ASSERT_EQ(sorted.size(), 12u);
    // Rows with equal totals are ordered by date (03 §11; CT-FSS-01 [D]).
    EXPECT_EQ(describe(sorted), (std::vector<std::string>{
                                    "BTS→LTN 2026-11-12 07:15 14.99/29.98",
                                    "BUD→LTN 2026-11-14 06:00 52.66/105.32",
                                    "BUD→LTN 2026-11-18 06:00 52.66/105.32",
                                    "BUD→LTN 2026-11-19 12:50 52.66/105.32",
                                    "BUD→LTN 2026-11-20 06:00 52.66/105.32",
                                    "BUD→LTN 2026-11-11 06:00 58.94/117.88",
                                    "BUD→LTN 2026-11-12 06:00 58.94/117.88",
                                    "BUD→LTN 2026-11-13 12:50 58.94/117.88",
                                    "BUD→LTN 2026-11-17 06:00 58.94/117.88",
                                    "BUD→LTN 2026-11-16 06:00 69.31/138.62",
                                    "BUD→LTN 2026-11-10 06:00 84.06/168.12",
                                    "BUD→LGW 2026-11-15 08:30 94.70/189.40",
                                }));
    // CT-FSS-01 [A]: Flights[0] and the first BUD row; ascending totals.
    const FlightResult& first = *sorted.front();
    EXPECT_EQ(first.from_airport->iata, "BTS");
    EXPECT_EQ(first.price_per_person_converted, dec("14.99"));
    EXPECT_EQ(first.total_converted, dec("29.98"));
    EXPECT_GE(first.from_distance_km, 170.0);
    EXPECT_LE(first.from_distance_km, 175.0);
    EXPECT_NE(first.booking_url.find("/BTS/LTN/2026-11-12/null/2/0/0/null"), std::string::npos);
    EXPECT_EQ(first.booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/null/2/0/0/null");
    const FlightResult& first_bud = *sorted.at(1);
    EXPECT_EQ(first_bud.fare.price_per_person.currency, "HUF");
    EXPECT_EQ(first_bud.price_per_person_converted,
              (dec("19290") / dec("366.3")).round(2, wizz::Rounding::away_from_zero));
    EXPECT_EQ(first_bud.price_per_person_converted, dec("52.66"));
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        EXPECT_LE(*sorted[i - 1]->total_converted, *sorted[i]->total_converted) << i;
    }
    // Other fields: FromDistanceKm BTS 172.5 / BUD 0.0, ToDistanceKm LTN 44.1 / LGW 40.6,
    // never outside the destination radius (the pins), target EUR, 2 adults.
    for (const FlightResultPtr& flight : sorted) {
        const std::string& from = flight->from_airport->iata;
        const std::string& to = flight->to_airport->iata;
        if (from == "BTS") {
            EXPECT_NEAR(flight->from_distance_km, 172.5, 0.05);
        } else {
            EXPECT_EQ(from, "BUD");
            EXPECT_EQ(flight->from_distance_km, 0.0);
        }
        ASSERT_TRUE(flight->to_distance_km.has_value());
        if (to == "LTN") {
            EXPECT_NEAR(*flight->to_distance_km, 44.072, 5e-4);
        } else {
            EXPECT_EQ(to, "LGW");
            EXPECT_NEAR(*flight->to_distance_km, 40.558, 5e-4);
        }
        EXPECT_FALSE(flight->outside_destination_radius);
        EXPECT_EQ(flight->target_currency, "EUR");
        EXPECT_EQ(flight->adults, 2);
        EXPECT_TRUE(flight->is_available());
    }
    EXPECT_TRUE(missing.empty());
}

TEST(Pricing, Section_11_reprice_to_HUF_and_GBP_without_network)
{
    const LegSearchResult eur =
        leg_result(budapest_to_london(), views::sort_by_price(section11_flights()));
    // To HUF: 14.99 × 366.3 = 5490.837 → 5491, ×2 = 10982; BUD rows stay exactly 19290 → 38580.
    const LegSearchResultPtr huf = pricing::reprice(eur, "HUF");
    EXPECT_EQ(huf->request.currency, "HUF");
    EXPECT_EQ(describe(huf->flights), (std::vector<std::string>{
                                          "BTS→LTN 2026-11-12 07:15 5491/10982",
                                          "BUD→LTN 2026-11-14 06:00 19290/38580",
                                          "BUD→LTN 2026-11-18 06:00 19290/38580",
                                          "BUD→LTN 2026-11-19 12:50 19290/38580",
                                          "BUD→LTN 2026-11-20 06:00 19290/38580",
                                          "BUD→LTN 2026-11-11 06:00 21590/43180",
                                          "BUD→LTN 2026-11-12 06:00 21590/43180",
                                          "BUD→LTN 2026-11-13 12:50 21590/43180",
                                          "BUD→LTN 2026-11-17 06:00 21590/43180",
                                          "BUD→LTN 2026-11-16 06:00 25390/50780",
                                          "BUD→LTN 2026-11-10 06:00 30790/61580",
                                          "BUD→LGW 2026-11-15 08:30 34690/69380",
                                      }));
    // To GBP: BTS 14.99 × 0.83536 = 12.5220… → 12.52 → 25.04; BUD 19290 / 366.3 × 0.83536 = 43.9915… → 43.99.
    const LegSearchResultPtr gbp = pricing::reprice(eur, "gbp");
    EXPECT_EQ(gbp->request.currency, "GBP");
    EXPECT_EQ(describe(gbp->flights), (std::vector<std::string>{
                                          "BTS→LTN 2026-11-12 07:15 12.52/25.04",
                                          "BUD→LTN 2026-11-14 06:00 43.99/87.98",
                                          "BUD→LTN 2026-11-18 06:00 43.99/87.98",
                                          "BUD→LTN 2026-11-19 12:50 43.99/87.98",
                                          "BUD→LTN 2026-11-20 06:00 43.99/87.98",
                                          "BUD→LTN 2026-11-11 06:00 49.24/98.48",
                                          "BUD→LTN 2026-11-12 06:00 49.24/98.48",
                                          "BUD→LTN 2026-11-13 12:50 49.24/98.48",
                                          "BUD→LTN 2026-11-17 06:00 49.24/98.48",
                                          "BUD→LTN 2026-11-16 06:00 57.90/115.80",
                                          "BUD→LTN 2026-11-10 06:00 70.22/140.44",
                                          "BUD→LGW 2026-11-15 08:30 79.11/158.22",
                                      }));
    for (const FlightResultPtr& flight : gbp->flights) {
        EXPECT_EQ(flight->target_currency, "GBP");
    }
    // The EUR result is untouched.
    EXPECT_EQ(eur.request.currency, "EUR");
    EXPECT_EQ(eur.flights.front()->total_converted->to_string(), "29.98");
}

TEST(Pricing, Section_11_return_leg_synthetic_GBP_fares)
{
    // London r30 → Budapest r200, 12–20 Nov, 2 adults, EUR; fares ÷ 0.83536.
    LegSearchRequest request = request_for("London", "Budapest", 2, "EUR");
    request.origin_radius_km = 30;
    request.destination_radius_km = 200;
    request.from = day(11, 12);
    const pricing::CodeSet destinations{"BUD", "BTS", "DEB"};
    const std::vector<DayFare> fares{
        day_fare("LGW", "BUD", day(11, 16), {14, 0}, {{14, 0}}, "29.99", "GBP"),
        day_fare("LTN", "BUD", day(11, 14), {10, 5}, {{10, 5}}, "24.99", "GBP"),
        day_fare("LTN", "BTS", day(11, 15), {12, 0}, {{12, 0}}, "19.99", "GBP"),
        day_fare("LTN", "BUD", day(11, 17), {21, 35}, {{21, 35}}, "17.99", "GBP"),
    };
    std::vector<FlightResultPtr> flights;
    for (const DayFare& fare : fares) {
        flights.push_back(pricing::build_result(*cat(), request, *rates(), fare, nullptr, &destinations));
    }
    const std::vector<FlightResultPtr> sorted = views::sort_by_price(flights);
    EXPECT_EQ(describe(sorted), (std::vector<std::string>{
                                    "LTN→BUD 2026-11-17 21:35 21.54/43.08",
                                    "LTN→BTS 2026-11-15 12:00 23.93/47.86",
                                    "LTN→BUD 2026-11-14 10:05 29.92/59.84",
                                    "LGW→BUD 2026-11-16 14:00 35.90/71.80",
                                }));
    for (const FlightResultPtr& flight : sorted) {
        EXPECT_NEAR(flight->from_distance_km, flight->from_airport->iata == "LGW" ? 40.6 : 44.1, 0.05);
        EXPECT_NEAR(*flight->to_distance_km, flight->to_airport->iata == "BTS" ? 172.5 : 0.0, 0.05);
        EXPECT_FALSE(flight->outside_destination_radius);
    }
}

// ---------------------------------------------------------------------------
// Reprice(LegSearchResult) (03 §6.3)
// ---------------------------------------------------------------------------

TEST(Pricing, CT_SRCH_07_Reprice_converts_without_new_requests_and_uses_currency_decimals)
{
    // Input: BuildResult of every timetable_v2_bud_ltn.json row, Budapest → anywhere, 2 adults, EUR.
    LegSearchRequest request = request_for("Budapest", std::nullopt, 2, "EUR");
    std::vector<FlightResultPtr> flights;
    for (const DayFare& fare : bud_ltn_rows()) {
        flights.push_back(pricing::build_result(*cat(), request, *rates(), fare));
    }
    // [D] before re-pricing: 52.66 and 105.32.
    const std::vector<FlightResultPtr> eur_order = views::sort_by_price(flights);
    EXPECT_EQ(eur_order.front()->price_per_person_converted->to_string(), "52.66");
    EXPECT_EQ(eur_order.front()->total_converted->to_string(), "105.32");

    const LegSearchResult result = leg_result(request, flights);
    const LegSearchResultPtr huf = pricing::reprice(result, "HUF");
    // [A]
    EXPECT_EQ(huf->request.currency, "HUF");
    ASSERT_FALSE(huf->flights.empty());
    EXPECT_EQ(huf->flights[0]->price_per_person_converted, Decimal{19290});
    EXPECT_EQ(huf->flights[0]->total_converted, Decimal{38580});
    EXPECT_EQ(wizz::currency_format::decimals("HUF"), 0);
    EXPECT_EQ(wizz::currency_format::number_format("EUR"), "#,##0.00");
    // [D] Flights[0] is BUD→LTN 2026-11-14 06:00; the HUF order is the EUR order of CT-FSS-01 without BTS.
    EXPECT_EQ(describe(*huf->flights[0]), "BUD→LTN 2026-11-14 06:00 19290/38580");
    std::vector<std::string> eur_routes;
    for (const FlightResultPtr& flight : eur_order) {
        eur_routes.push_back(flight->fare.from + flight->fare.to + flight->date().iso());
    }
    std::vector<std::string> huf_routes;
    for (const FlightResultPtr& flight : huf->flights) {
        huf_routes.push_back(flight->fare.from + flight->fare.to + flight->date().iso());
    }
    EXPECT_EQ(huf_routes, eur_routes);
    // [D] en-US money texts.
    const wizz::NumberSymbols& en_us = wizz::Locale::en_us()->number_symbols();
    EXPECT_EQ(wizz::currency_format::format(dec("1234.5"), "EUR", en_us), "1,234.50 EUR");
    EXPECT_EQ(wizz::currency_format::format(Decimal{19290}, "HUF", en_us), "19,290 HUF");
    EXPECT_EQ(wizz::currency_format::format(*huf->flights[0]->total_converted, "HUF", en_us), "38,580 HUF");
    // CT-SRCH-06 [D] (pricing part): the same rows in GBP start with BUD→LTN 11-14 at 43.99 per person.
    const LegSearchResultPtr gbp = pricing::reprice(result, "GBP");
    EXPECT_EQ(gbp->flights[0]->target_currency, "GBP");
    EXPECT_EQ(describe(*gbp->flights[0]), "BUD→LTN 2026-11-14 06:00 43.99/87.98");
}

TEST(Pricing, Reprice_of_a_result_keeps_everything_else)
{
    LegSearchRequest request = budapest_to_london();
    request.label = "Outbound";
    request.price_type = wizz::PriceType::wizz_discount_club;
    LegSearchResult result = leg_result(request, views::sort_by_price(section11_flights()));
    result.plan.notes = {"Some plan note."};
    result.plan.routes_considered = 3;
    result.plan.effective_from = day(11, 10);
    result.plan.effective_to = day(11, 20);
    result.warnings = {"Some plan note.", "No exchange rate for xyz; converted prices are unavailable."};
    result.requests_completed = 3;
    result.routes_not_served = 1;
    result.cancelled = true;
    result.abort_reason = "Stopped after 4 failed requests in a row. Last error: x";
    result.elapsed = std::chrono::seconds{7};

    const LegSearchResultPtr repriced = pricing::reprice(result, "pln");
    EXPECT_EQ(repriced->request.currency, "PLN");
    EXPECT_EQ(repriced->request.label, "Outbound");
    EXPECT_EQ(repriced->request.price_type, wizz::PriceType::wizz_discount_club);
    EXPECT_EQ(repriced->request.adults, 2);
    EXPECT_EQ(repriced->request.origin, request.origin);
    EXPECT_EQ(repriced->request.destination, request.destination);
    EXPECT_EQ(repriced->plan.notes, result.plan.notes);
    EXPECT_EQ(repriced->plan.routes_considered, 3);
    EXPECT_EQ(repriced->plan.effective_from, day(11, 10));
    EXPECT_EQ(repriced->warnings, result.warnings); // not recomputed
    EXPECT_EQ(repriced->rates.get(), result.rates.get());
    EXPECT_EQ(repriced->requests_completed, 3);
    EXPECT_EQ(repriced->routes_not_served, 1);
    EXPECT_TRUE(repriced->cancelled);
    EXPECT_EQ(repriced->abort_reason, result.abort_reason);
    EXPECT_EQ(repriced->elapsed, result.elapsed);
    ASSERT_EQ(repriced->flights.size(), 12u);
    for (const FlightResultPtr& flight : repriced->flights) {
        EXPECT_EQ(flight->target_currency, "PLN");
        EXPECT_EQ(flight->total_converted->scale(), 2);
    }
    // 14.99 EUR × 4.4032 = 66.003968 → 66.00 → 132.00.
    EXPECT_EQ(describe(*repriced->flights[0]), "BTS→LTN 2026-11-12 07:15 66.00/132.00");
    // The input result is unchanged.
    EXPECT_EQ(result.request.currency, "EUR");
    EXPECT_EQ(result.flights.front()->target_currency, "EUR");

    LegSearchResult no_rates = result;
    no_rates.rates = nullptr;
    EXPECT_THROW((void)pricing::reprice(no_rates, "EUR"), std::invalid_argument);
}

TEST(Pricing, Reprice_keeps_the_previous_order_for_full_ties)
{
    const LegSearchRequest request = request_for("Budapest", std::nullopt, 1, "EUR");
    const FlightResultPtr ltn = pricing::build_result(
        *cat(), request, *rates(), day_fare("BUD", "LTN", day(11, 14), {6, 0}, {{6, 0}}, "10", "EUR"));
    const FlightResultPtr lgw = pricing::build_result(
        *cat(), request, *rates(), day_fare("BUD", "LGW", day(11, 14), {6, 0}, {{6, 0}}, "10", "EUR"));
    const LegSearchResultPtr forward = pricing::reprice(leg_result(request, {ltn, lgw}), "HUF");
    EXPECT_EQ(forward->flights[0]->fare.to, "LTN");
    EXPECT_EQ(forward->flights[1]->fare.to, "LGW");
    const LegSearchResultPtr backward = pricing::reprice(leg_result(request, {lgw, ltn}), "HUF");
    EXPECT_EQ(backward->flights[0]->fare.to, "LGW");
    EXPECT_EQ(backward->flights[1]->fare.to, "LTN");
    EXPECT_EQ(backward->flights[0]->total_converted, Decimal{3663});
}

TEST(Pricing, Reprice_of_a_flight_returns_a_new_result_and_leaves_the_input_alone)
{
    const FlightResultPtr eur = pricing::build_result(*cat(), request_for("Budapest", "London", 3, "EUR"), *rates(),
                                                      bud_ltn_rows().at(0)); // 30790 HUF
    const FlightResultPtr gbp = pricing::reprice(*eur, *rates(), "GBP");
    EXPECT_NE(gbp.get(), eur.get());
    EXPECT_EQ(eur->target_currency, "EUR");
    EXPECT_EQ(eur->price_per_person_converted->to_string(), "84.06");
    EXPECT_EQ(eur->total_converted->to_string(), "252.18");
    EXPECT_EQ(gbp->target_currency, "GBP");
    EXPECT_EQ(gbp->price_per_person_converted->to_string(), "70.22");
    EXPECT_EQ(gbp->total_converted->to_string(), "210.66"); // × the result's own 3 adults
    EXPECT_EQ(gbp->adults, 3);
    EXPECT_EQ(gbp->booking_url, eur->booking_url);
    EXPECT_EQ(gbp->from_airport.get(), eur->from_airport.get());
    // An unsupported currency leaves no price.
    const FlightResultPtr none = pricing::reprice(*eur, *rates(), "xyz");
    EXPECT_EQ(none->target_currency, "XYZ");
    EXPECT_FALSE(none->price_per_person_converted.has_value());
    EXPECT_FALSE(none->total_converted.has_value());
}

TEST(Pricing, Ordered_code_set_and_iata_set)
{
    pricing::OrderedCodeSet set;
    EXPECT_TRUE(set.empty());
    EXPECT_TRUE(set.add("XYZ"));
    EXPECT_FALSE(set.add("xyz"));
    EXPECT_TRUE(set.add("abc"));
    EXPECT_FALSE(set.add("ABC"));
    EXPECT_TRUE(set.add(""));
    EXPECT_FALSE(set.add(""));
    EXPECT_EQ(set.size(), 3u);
    EXPECT_TRUE(set.contains("Xyz"));
    EXPECT_FALSE(set.contains("QQQ"));
    std::vector<std::string> iterated(set.begin(), set.end());
    EXPECT_EQ(iterated, (std::vector<std::string>{"XYZ", "abc", ""}));

    const std::vector<wizz::AreaAirport> area{{station("LGW"), 40.558}, {station("LTN"), 44.072}};
    const pricing::CodeSet codes = pricing::iata_set(area);
    EXPECT_EQ(codes.size(), 2u);
    EXPECT_TRUE(codes.contains("lgw"));
    EXPECT_TRUE(codes.contains("LTN"));
    EXPECT_FALSE(codes.contains("LON"));
}

} // namespace
