// RoundTripCombiner: 05 §3.8 CT-RT-01/02, §3.12 CT-RTR-01..07, §7.1 P-28,
// the 03 §10.3 worked numbers, the 03 §11 worked example (MatchLegs +
// Combine(min 2, max 5) = 19 trips), the 01 §11.8 problem texts and the 07 §3 /
// D-12 deterministic ranking. Locale en-US (05 §1.11).
//
// The 05 §1.7 fare and leg builders call BuildResult and SearchPlanner.Plan,
// which live in other modules. The helpers below build FlightResult and
// LegSearchResult values directly and reproduce the builders' documented
// consequences: BuildResult's distances, adult clamp, OneWay link and
// convert → round per person (away from zero) → × adults (03 §6.1/§6.2) over
// the RATES subset used here, and AirportsFor's origin area (03 §4.3.2), which
// gives exactly the [D] origin lists of 05 §1.7.

#include "wizz/core/round_trips.hpp"

#include "wizz/core/catalog.hpp"
#include "wizz/core/currency_format.hpp"
#include "wizz/core/decimal.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/text.hpp"
#include "wizz/core/time.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

namespace rt = wizz::round_trips;

using wizz::Airport;
using wizz::AirportCatalog;
using wizz::AirportCatalogPtr;
using wizz::AirportPtr;
using wizz::AreaAirport;
using wizz::Date;
using wizz::DayFare;
using wizz::Decimal;
using wizz::Duration;
using wizz::FareStatus;
using wizz::FlightResult;
using wizz::FlightResultPtr;
using wizz::FxRates;
using wizz::FxRatesPtr;
using wizz::GeoPoint;
using wizz::LegMatch;
using wizz::LegSearchRequest;
using wizz::LegSearchResult;
using wizz::Locale;
using wizz::Money;
using wizz::Place;
using wizz::PriceType;
using wizz::RoundTrip;
using wizz::make_local;

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr std::string_view kSelectFlight = "https://www.wizzair.com/en-gb/booking/select-flight";

/// d = 2026-11-10 (05 §3.8, §3.12).
constexpr Date kD = Date::from_ymd(2026, 11, 10);

[[nodiscard]] constexpr Date d_plus(int days) noexcept
{
    return kD.add_days(days);
}

[[nodiscard]] Decimal dec(std::string_view text)
{
    const std::optional<Decimal> value = Decimal::parse(text);
    if (!value) {
        throw std::logic_error("bad decimal literal in a test");
    }
    return *value;
}

/// CAT (05 §0.3): the catalog of fixtures/map_subset.json under en-US.
[[nodiscard]] const AirportCatalogPtr& cat()
{
    static const AirportCatalogPtr catalog =
        AirportCatalog::from_map_json(wizz::test::read_fixture("map_subset.json"), Locale::en_us());
    return catalog;
}

[[nodiscard]] AirportPtr station(std::string_view code)
{
    AirportPtr airport = cat()->find(code);
    if (!airport) {
        throw std::logic_error("fixture station missing");
    }
    return airport;
}

[[nodiscard]] Place match(std::string_view text)
{
    std::optional<Place> place = cat()->match_place(text);
    if (!place) {
        throw std::logic_error("MatchPlace failed in a test");
    }
    return *place;
}

/// The RATES rows this file needs (fx_frankfurter_v2.json, 05 §2.5): EUR 1,
/// GBP 0.83536, HUF 366.3. The Frankfurter parser belongs to another module.
[[nodiscard]] const FxRatesPtr& rates()
{
    static const FxRatesPtr table = [] {
        auto fx = std::make_shared<FxRates>();
        fx->per_eur = {{"EUR", dec("1")}, {"GBP", dec("0.83536")}, {"HUF", dec("366.3")}};
        fx->date = Date::from_ymd(2026, 11, 2);
        fx->source = "Frankfurter";
        return FxRatesPtr{std::move(fx)};
    }();
    return table;
}

/// 03 §9 OneWay, as BuildResult sets FlightResult.booking_url.
[[nodiscard]] std::string one_way_link(std::string_view from, std::string_view to, Date date, int adults)
{
    return std::format("{}/{}/{}/{}/null/{}/0/0/null", kSelectFlight, wizz::text::to_upper_invariant(from),
                       wizz::text::to_upper_invariant(to), date.iso(), std::max(1, adults));
}

/// 03 §7.1 Convert through EUR over `rates()` (same currency → unchanged).
[[nodiscard]] std::optional<Decimal> convert(Decimal amount, const std::string& from, const std::string& to)
{
    if (from == to) {
        return amount;
    }
    const auto& per_eur = rates()->per_eur;
    const auto f = per_eur.find(from);
    const auto t = per_eur.find(to);
    if (f == per_eur.end() || t == per_eur.end()) {
        return std::nullopt;
    }
    return amount / f->second * t->second;
}

/// 03 §6.1 BuildResult(CAT, request, RATES, fare) for the cases used here.
[[nodiscard]] FlightResultPtr build_result(const LegSearchRequest& request, DayFare fare)
{
    auto result = std::make_shared<FlightResult>();
    result->from_airport = station(fare.from);
    result->to_airport = station(fare.to);
    result->from_distance_km = wizz::distance_km(request.origin.location, result->from_airport->location);
    if (request.destination) {
        result->to_distance_km = wizz::distance_km(request.destination->location, result->to_airport->location);
    }
    result->adults = std::max(1, request.adults);
    result->target_currency = wizz::text::to_upper_invariant(request.currency);
    result->booking_url = one_way_link(fare.from, fare.to, fare.date, result->adults);
    if (fare.status == FareStatus::available) {
        if (const std::optional<Decimal> p =
                convert(fare.price_per_person.amount, fare.price_per_person.currency, result->target_currency)) {
            const Decimal per_person =
                p->round(wizz::currency_format::decimals(result->target_currency), wizz::Rounding::away_from_zero);
            result->price_per_person_converted = per_person;
            result->total_converted = per_person * result->adults;
        }
    }
    result->fare = std::move(fare);
    return result;
}

/// 05 §1.7 fare builder A (`hour` 6, `adults` 1) and builder B (other hour / adults).
[[nodiscard]] FlightResultPtr fare(std::string_view from, std::string_view to, Date date, std::string_view eur,
                                   int hour = 6, int adults = 1)
{
    LegSearchRequest request;
    request.origin = match(from);
    request.from = date;
    request.to = date;
    request.currency = "EUR";
    request.adults = adults;
    DayFare day{.from = std::string(from),
                .to = std::string(to),
                .date = date,
                .cheapest_departure = make_local(date, hour, 0),
                .departures = {},
                .price_per_person = Money{dec(eur), "EUR"},
                .status = FareStatus::available,
                .has_mac_flight = false};
    return build_result(request, std::move(day));
}

/// 03 §4.3.2 AirportsFor(CAT, place, radius): Near plus the pinned stations
/// (fake pins expand to their members), stable-sorted by distance.
[[nodiscard]] std::vector<AreaAirport> airports_for(const Place& place, double radius_km)
{
    std::vector<AreaAirport> result = cat()->airports_near(place.location, std::max(0.0, radius_km));
    for (const std::string& code : place.pinned_airports) {
        const AirportPtr pinned = cat()->find(code);
        if (!pinned) {
            continue;
        }
        const std::vector<AirportPtr> members =
            pinned->is_fake_station ? cat()->group_members(pinned->iata) : std::vector<AirportPtr>{pinned};
        for (const AirportPtr& a : members) {
            const bool present =
                std::ranges::any_of(result, [&](const AreaAirport& r) { return r.airport->iata == a->iata; });
            if (!present) {
                result.push_back(AreaAirport{a, wizz::distance_km(place.location, a->location)});
            }
        }
    }
    std::ranges::stable_sort(result, {}, &AreaAirport::distance_km);
    return result;
}

/// A leg result as the 05 §1.7 leg builder makes it, for an explicit request.
[[nodiscard]] LegSearchResult make_leg(LegSearchRequest request, std::vector<FlightResultPtr> flights)
{
    LegSearchResult result;
    result.plan.origin_airports = airports_for(request.origin, request.origin_radius_km);
    result.plan.effective_from = request.from;
    result.plan.effective_to = request.to;
    result.request = std::move(request);
    result.flights = std::move(flights);
    result.rates = rates();
    return result;
}

/// 05 §1.7 leg builder `Leg(origin, radius, flights, currency = "EUR", adults = 1)`.
[[nodiscard]] LegSearchResult leg(std::string_view origin, double radius, std::vector<FlightResultPtr> flights,
                                  std::string currency = "EUR", int adults = 1)
{
    LegSearchRequest request;
    request.origin = match(origin);
    request.origin_radius_km = radius;
    request.from = Date::from_ymd(2026, 11, 10);
    request.to = Date::from_ymd(2026, 12, 10);
    request.currency = std::move(currency);
    request.adults = adults;
    return make_leg(std::move(request), std::move(flights));
}

[[nodiscard]] std::vector<std::string> origin_codes(const LegSearchResult& leg)
{
    std::vector<std::string> codes;
    for (const AreaAirport& a : leg.plan.origin_airports) {
        codes.push_back(a.airport->iata);
    }
    return codes;
}

/// "FROM→TO" of a flight.
[[nodiscard]] std::string route(const FlightResultPtr& f)
{
    return f->from_airport->iata + "→" + f->to_airport->iata;
}

[[nodiscard]] std::vector<std::string> routes(const std::vector<FlightResultPtr>& flights)
{
    std::vector<std::string> result;
    for (const FlightResultPtr& f : flights) {
        result.push_back(route(f));
    }
    return result;
}

[[nodiscard]] std::vector<Decimal> totals(const std::vector<RoundTrip>& trips)
{
    std::vector<Decimal> result;
    for (const RoundTrip& t : trips) {
        result.push_back(t.total.value());
    }
    return result;
}

/// The same trips in the same order: identical FlightResult instances (Assert.Same).
[[nodiscard]] bool same_trips(std::span<const RoundTrip> a, std::span<const RoundTrip> b)
{
    return std::ranges::equal(a, b, [](const RoundTrip& x, const RoundTrip& y) {
        return x.outbound == y.outbound && x.inbound == y.inbound && x.total == y.total && x.nights == y.nights;
    });
}

/// 07 §3 key without the generation number: a ≤ b.
[[nodiscard]] bool rank_not_after(const RoundTrip& a, const RoundTrip& b)
{
    const auto key = [](const RoundTrip& t) {
        return std::tuple{t.outbound->date(),
                          t.nights,
                          t.outbound->fare.cheapest_departure,
                          t.inbound->fare.cheapest_departure,
                          t.outbound->from_airport->iata,
                          t.outbound->to_airport->iata,
                          t.inbound->from_airport->iata,
                          t.inbound->to_airport->iata};
    };
    if (*a.total != *b.total) {
        return *a.total < *b.total;
    }
    return key(a) <= key(b);
}

[[nodiscard]] bool sorted_by_rank(const std::vector<RoundTrip>& trips)
{
    for (std::size_t i = 1; i < trips.size(); ++i) {
        if (!rank_not_after(trips[i - 1], trips[i])) {
            return false;
        }
    }
    return true;
}

/// A hand-built station (CT-RTR-04 and the edge cases).
[[nodiscard]] AirportPtr hand_airport(std::string iata, double latitude, double longitude,
                                      std::optional<std::string> mac = std::nullopt, bool has_city_group = false)
{
    auto airport = std::make_shared<Airport>();
    airport->iata = iata;
    airport->name = std::move(iata);
    airport->location = GeoPoint{latitude, longitude};
    airport->mac = std::move(mac);
    airport->has_city_group = has_city_group;
    return airport;
}

/// A FlightResult built directly (CT-RTR-04): price 10 EUR, converted 10, total 10, Available.
[[nodiscard]] FlightResultPtr direct_flight(AirportPtr from, AirportPtr to, Date date, int hour, int minute = 0,
                                            int second = 0)
{
    auto f = std::make_shared<FlightResult>();
    f->fare.from = from->iata;
    f->fare.to = to->iata;
    f->fare.date = date;
    f->fare.cheapest_departure = make_local(date, hour, minute, second);
    f->fare.price_per_person = Money{Decimal{10}, "EUR"};
    f->from_airport = std::move(from);
    f->to_airport = std::move(to);
    f->target_currency = "EUR";
    f->price_per_person_converted = Decimal{10};
    f->total_converted = Decimal{10};
    f->booking_url = one_way_link(f->fare.from, f->fare.to, date, 1);
    return f;
}

// ---------------------------------------------------------------------------
// The test builders themselves (05 §1.7 consequences and [D] origin lists)
// ---------------------------------------------------------------------------

TEST(RoundTrips, Builders_05_1_7_fare_builder_consequences)
{
    const FlightResultPtr f = fare("BUD", "LTN", kD, "20");
    EXPECT_EQ(f->total_converted, Decimal{20});
    EXPECT_EQ(f->price_per_person_converted, Decimal{20});
    EXPECT_EQ(f->from_distance_km, 0.0);
    EXPECT_FALSE(f->to_distance_km.has_value());
    EXPECT_FALSE(f->outside_destination_radius);
    EXPECT_EQ(f->adults, 1);
    EXPECT_EQ(f->fare.cheapest_departure, make_local(kD, 6, 0));
    EXPECT_EQ(f->booking_url, "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/null/1/0/0/null");
}

TEST(RoundTrips, Builders_05_1_7_leg_plan_origin_airports)
{
    EXPECT_EQ(origin_codes(leg("Budapest", 0, {})), (std::vector<std::string>{"BUD"}));
    EXPECT_EQ(origin_codes(leg("LTN", 0, {})), (std::vector<std::string>{"LTN"}));
    EXPECT_EQ(origin_codes(leg("Katowice", 0, {})), (std::vector<std::string>{"KTW"}));
    EXPECT_EQ(origin_codes(leg("Kraków", 0, {})), (std::vector<std::string>{"KRK"}));
    const LegSearchResult london = leg("London", 0, {});
    ASSERT_EQ(origin_codes(london), (std::vector<std::string>{"LGW", "LTN"}));
    EXPECT_NEAR(london.plan.origin_airports[0].distance_km, 40.56, 0.005);
    EXPECT_NEAR(london.plan.origin_airports[1].distance_km, 44.07, 0.005);
}

// ---------------------------------------------------------------------------
// 05 §3.8 Round trips
// ---------------------------------------------------------------------------

TEST(RoundTrips, CT_RT_01_Combines_within_night_window_cheapest_first)
{
    const std::vector<FlightResultPtr> outs{fare("BUD", "LTN", kD, "20"), fare("BTS", "LTN", d_plus(1), "10")};
    const std::vector<FlightResultPtr> rets{fare("LTN", "BUD", d_plus(3), "30"), fare("LGW", "BUD", d_plus(5), "15"),
                                            fare("LTN", "BTS", d_plus(20), "1")};

    const std::vector<RoundTrip> trips = rt::combine(outs, rets, 2, 5);

    // [A]
    ASSERT_EQ(trips.size(), 4u);
    EXPECT_EQ(trips[0].total, Decimal{25});
    EXPECT_EQ(trips[0].nights, 4);
    EXPECT_FALSE(trips[0].same_home_airport);
    EXPECT_FALSE(trips[0].same_away_airport);
    EXPECT_FALSE(trips[0].combined_booking_url.has_value());
    EXPECT_TRUE(std::ranges::is_sorted(totals(trips)));

    // [D] all four trips in order.
    struct Row {
        int total;
        int nights;
        const FlightResultPtr& out;
        const FlightResultPtr& ret;
        bool same_home;
        bool same_away;
    };
    const Row expected[] = {
        {25, 4, outs[1], rets[1], false, false},  // BTS→LTN 11-11 + LGW→BUD 11-15
        {35, 5, outs[0], rets[1], true, false},   // BUD→LTN 11-10 + LGW→BUD 11-15
        {40, 2, outs[1], rets[0], false, true},   // BTS→LTN 11-11 + LTN→BUD 11-13
        {50, 3, outs[0], rets[0], true, true},    // BUD→LTN 11-10 + LTN→BUD 11-13
    };
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(trips[i].total, Decimal{expected[i].total});
        EXPECT_EQ(trips[i].nights, expected[i].nights);
        EXPECT_EQ(trips[i].outbound, expected[i].out);
        EXPECT_EQ(trips[i].inbound, expected[i].ret);
        EXPECT_EQ(trips[i].same_home_airport, expected[i].same_home);
        EXPECT_EQ(trips[i].same_away_airport, expected[i].same_away);
        EXPECT_EQ(trips[i].combined_booking_url.has_value(), expected[i].same_home && expected[i].same_away);
    }
    EXPECT_EQ(route(trips[0].outbound), "BTS→LTN");
    EXPECT_EQ(trips[0].outbound->date(), d_plus(1));
    EXPECT_EQ(route(trips[0].inbound), "LGW→BUD");
    EXPECT_EQ(trips[0].inbound->date(), d_plus(5));
    EXPECT_EQ(trips[3].combined_booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-13/1/0/0/null");

    // [A] both flags: a single trip, total 50, with the combined link.
    const std::vector<RoundTrip> both = rt::combine(outs, rets, 2, 5, rt::kDefaultLimit, true, true);
    ASSERT_EQ(both.size(), 1u);
    EXPECT_EQ(both[0].total, Decimal{50});
    ASSERT_TRUE(both[0].combined_booking_url.has_value());
    EXPECT_NE(both[0].combined_booking_url->find("/BUD/LTN/2026-11-10/2026-11-13/1/0/0/null"), std::string::npos);
    EXPECT_EQ(*both[0].combined_booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-13/1/0/0/null");

    // Each flag on its own filters only its own condition.
    EXPECT_EQ(totals(rt::combine(outs, rets, 2, 5, rt::kDefaultLimit, true, false)),
              (std::vector<Decimal>{Decimal{35}, Decimal{50}}));
    EXPECT_EQ(totals(rt::combine(outs, rets, 2, 5, rt::kDefaultLimit, false, true)),
              (std::vector<Decimal>{Decimal{40}, Decimal{50}}));
}

namespace ct_rt_02 {

struct Legs {
    std::vector<FlightResultPtr> outs;
    std::vector<FlightResultPtr> rets;
};

/// 30 out BUD→LTN d+i costing 10+i; 30 ret LTN→BUD d+i+3 costing 50−i; i = 0..29.
[[nodiscard]] const Legs& legs()
{
    static const Legs value = [] {
        Legs l;
        for (int i = 0; i < 30; ++i) {
            l.outs.push_back(fare("BUD", "LTN", d_plus(i), std::to_string(10 + i)));
            l.rets.push_back(fare("LTN", "BUD", d_plus(i + 3), std::to_string(50 - i)));
        }
        return l;
    }();
    return value;
}

} // namespace ct_rt_02

TEST(RoundTrips, CT_RT_02_Limit_keeps_the_cheapest)
{
    const auto& [outs, rets] = ct_rt_02::legs();
    const std::vector<RoundTrip> uncapped = rt::combine(outs, rets, 1, 10, 10000);
    const std::vector<RoundTrip> top5 = rt::combine(outs, rets, 1, 10, 5);

    // [A]
    ASSERT_EQ(top5.size(), 5u);
    ASSERT_GE(uncapped.size(), 5u);
    const std::vector<Decimal> all_totals = totals(uncapped);
    EXPECT_EQ(totals(top5), (std::vector<Decimal>(all_totals.begin(), all_totals.begin() + 5)));

    // [D] 269 trips; the 23 cheapest have total 53 and 10 nights.
    ASSERT_EQ(uncapped.size(), 269u);
    for (std::size_t i = 0; i < 23; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(uncapped[i].total, Decimal{53});
        EXPECT_EQ(uncapped[i].nights, 10);
    }
    EXPECT_GT(*uncapped[23].total, Decimal{53});
    EXPECT_TRUE(std::ranges::is_sorted(all_totals));
    EXPECT_TRUE(sorted_by_rank(uncapped));

    // [D] the limit-5 trips: outbound 11-10..11-14, each returning 10 nights later.
    for (std::size_t i = 0; i < 5; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(top5[i].outbound->date(), d_plus(static_cast<int>(i)));
        EXPECT_EQ(top5[i].nights, 10);
        EXPECT_EQ(top5[i].inbound->date(), d_plus(static_cast<int>(i) + 10));
        EXPECT_EQ(top5[i].total, Decimal{53});
    }
}

TEST(RoundTrips, CT_RT_02_Every_limit_gives_a_prefix_of_the_uncapped_result)
{
    // 07 §3 / D-12: the best N must be the prefix of an uncapped result.
    const auto& [outs, rets] = ct_rt_02::legs();
    const std::vector<RoundTrip> uncapped = rt::combine(outs, rets, 1, 10, 10000);
    ASSERT_EQ(uncapped.size(), 269u);
    for (const int limit : {1, 2, 3, 5, 22, 23, 24, 100, 268, 269, 270, 500, 10000}) {
        SCOPED_TRACE(limit);
        const std::vector<RoundTrip> capped = rt::combine(outs, rets, 1, 10, limit);
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(limit), uncapped.size());
        ASSERT_EQ(capped.size(), n);
        EXPECT_TRUE(same_trips(capped, std::span{uncapped}.first(n)));
    }
    EXPECT_TRUE(same_trips(rt::combine(outs, rets, 1, 10), uncapped));  // default limit 500

    // The input order only feeds the last tie breaker: reversed legs rank the same.
    const std::vector<FlightResultPtr> outs_reversed(outs.rbegin(), outs.rend());
    const std::vector<FlightResultPtr> rets_reversed(rets.rbegin(), rets.rend());
    EXPECT_TRUE(same_trips(rt::combine(outs_reversed, rets_reversed, 1, 10, 10000), uncapped));
    EXPECT_TRUE(same_trips(rt::combine(outs_reversed, rets_reversed, 1, 10, 7), std::span{uncapped}.first(7)));
}

// ---------------------------------------------------------------------------
// 05 §3.12 Round-trip regressions
// ---------------------------------------------------------------------------

TEST(RoundTrips, CT_RTR_01_Anywhere_outbound_is_only_paired_with_returns_from_where_it_landed)
{
    const LegSearchResult out = leg("Budapest", 0,
                                    {fare("BUD", "NAP", kD, "10"), fare("BUD", "BCN", kD, "20"),
                                     fare("BUD", "LGW", kD, "40")});
    const LegSearchResult ret = leg("LTN", 0, {fare("LTN", "BUD", d_plus(3), "25")});

    const LegMatch match = rt::match_legs(out, ret);

    EXPECT_FALSE(match.problem.has_value());
    EXPECT_EQ(routes(match.outbound), (std::vector<std::string>{"BUD→LGW"}));  // LGW: same London city as LTN
    EXPECT_EQ(match.outbound[0], out.flights[2]);
    EXPECT_EQ(routes(match.inbound), (std::vector<std::string>{"LTN→BUD"}));
    const std::vector<RoundTrip> trips = rt::combine(match.outbound, match.inbound, 1, 10);
    ASSERT_EQ(trips.size(), 1u);
    EXPECT_EQ(trips[0].total, Decimal{65});
    EXPECT_EQ(trips[0].nights, 3);
    EXPECT_TRUE(trips[0].same_home_airport);
    EXPECT_FALSE(trips[0].same_away_airport);  // landed at LGW, returns from LTN
    EXPECT_FALSE(trips[0].combined_booking_url.has_value());
}

TEST(RoundTrips, CT_RTR_02_Regional_airport_groups_are_not_treated_as_the_same_city)
{
    const LegSearchResult out = leg("Budapest", 0, {fare("BUD", "KRK", kD, "20")});
    const LegSearchResult ret = leg("Katowice", 0, {fare("KTW", "BUD", d_plus(3), "25")});
    ASSERT_FALSE(station("KRK")->has_city_group);  // SPQ has no pseudo station

    const LegMatch match = rt::match_legs(out, ret);

    ASSERT_TRUE(match.problem.has_value());
    EXPECT_EQ(*match.problem, "The return was searched from KTW, but none of the outbound fares land there. "
                              "Search the return from your destination.");
    EXPECT_TRUE(match.outbound.empty());
    EXPECT_TRUE(match.inbound.empty());
}

TEST(RoundTrips, CT_RTR_03_Returns_must_land_at_a_home_area_airport_exactly)
{
    const LegSearchResult out = leg("Kraków", 0, {fare("KRK", "LTN", kD, "20")});
    const LegSearchResult ret =
        leg("London", 0, {fare("LTN", "KTW", d_plus(3), "10"), fare("LTN", "KRK", d_plus(4), "30")});

    const LegMatch match = rt::match_legs(out, ret);

    EXPECT_FALSE(match.problem.has_value());
    EXPECT_EQ(routes(match.inbound), (std::vector<std::string>{"LTN→KRK"}));  // KTW: sibling outside 0 km
    EXPECT_EQ(match.inbound[0], ret.flights[1]);
    EXPECT_EQ(routes(match.outbound), (std::vector<std::string>{"KRK→LTN"}));
}

TEST(RoundTrips, CT_RTR_04_Same_day_eastbound_trip_accounts_for_the_time_zone)
{
    const AirportPtr ltn = hand_airport("LTN", 51.87, -0.37);
    const AirportPtr evn = hand_airport("EVN", 40.15, 44.40);

    // [A]
    EXPECT_FALSE(rt::same_day_feasible(*direct_flight(ltn, evn, kD, 10), *direct_flight(evn, ltn, kD, 18, 30)));
    EXPECT_TRUE(rt::same_day_feasible(*direct_flight(ltn, evn, kD, 6), *direct_flight(evn, ltn, kD, 23)));

    // [D] 3627.2645 km → 5.33635 h; zone round(2.98467) = 3 (half to even); required 11.33635 h.
    const rt::SameDayRequirement r = rt::same_day_requirement(ltn->location, evn->location);
    EXPECT_NEAR(r.distance_km, 3627.2645, 5e-5);
    EXPECT_NEAR(r.flight_hours, 5.33635, 5e-6);
    EXPECT_NEAR((evn->location.longitude - ltn->location.longitude) / 15.0, 2.98467, 5e-6);
    EXPECT_EQ(r.zone_offset_hours, 3.0);
    EXPECT_NEAR(r.required_hours, 11.33635, 5e-6);
    EXPECT_EQ(r.required_hours, r.flight_hours + 3.0 + 3.0);

    // The combiner applies the same rule to 0-night pairs.
    EXPECT_TRUE(rt::combine(std::vector{direct_flight(ltn, evn, kD, 10)}, std::vector{direct_flight(evn, ltn, kD, 18, 30)},
                            0, 3)
                    .empty());
    EXPECT_EQ(rt::combine(std::vector{direct_flight(ltn, evn, kD, 6)}, std::vector{direct_flight(evn, ltn, kD, 23)}, 0, 3)
                  .size(),
              1u);
}

TEST(RoundTrips, CT_RTR_05_Legs_from_unrelated_searches_are_not_combined)
{
    const LegSearchResult out = leg("Budapest", 0, {fare("BUD", "BCN", kD, "20")});
    const LegSearchResult ret = leg("London", 0, {fare("LTN", "BUD", d_plus(3), "25")});

    const LegMatch match = rt::match_legs(out, ret);

    ASSERT_TRUE(match.problem.has_value());
    EXPECT_NE(match.problem->find("none of the outbound fares land there"), std::string::npos);
    EXPECT_EQ(*match.problem, "The return was searched from LGW, LTN, but none of the outbound fares land there. "
                              "Search the return from your destination.");
    EXPECT_TRUE(match.outbound.empty());
    EXPECT_TRUE(match.inbound.empty());
}

TEST(RoundTrips, CT_RTR_06_Different_currency_or_passenger_count_is_refused)
{
    const LegSearchResult out = leg("Budapest", 0, {fare("BUD", "LTN", kD, "20")});

    const LegMatch currency = rt::match_legs(out, leg("London", 0, {}, "GBP"));
    ASSERT_TRUE(currency.problem.has_value());
    EXPECT_NE(currency.problem->find("same currency"), std::string::npos);
    EXPECT_EQ(*currency.problem,
              "Outbound prices are in EUR but return prices in GBP. Run both with the same currency.");
    EXPECT_TRUE(currency.outbound.empty());
    EXPECT_TRUE(currency.inbound.empty());

    const LegMatch adults = rt::match_legs(out, leg("London", 0, {}, "EUR", 2));
    ASSERT_TRUE(adults.problem.has_value());
    EXPECT_NE(adults.problem->find("same number"), std::string::npos);
    EXPECT_EQ(*adults.problem,
              "Outbound was searched for 1 adult(s) but the return for 2. Search both legs with the same number.");

    // [D] the fare-type text (untested in 05).
    LegSearchResult club = leg("London", 0, {});
    club.request.price_type = PriceType::wizz_discount_club;
    const LegMatch fare_type = rt::match_legs(out, club);
    ASSERT_TRUE(fare_type.problem.has_value());
    EXPECT_EQ(*fare_type.problem, "Outbound and return were searched with different fare types (regular vs "
                                  "Discount Club). Search both the same way.");
}

TEST(RoundTrips, CT_RTR_07_Same_day_trip_requires_the_return_to_leave_after_landing)
{
    const FlightResultPtr morning = fare("BUD", "LTN", kD, "20", 6);
    const FlightResultPtr evening = fare("BUD", "LTN", kD, "20", 18);
    const FlightResultPtr late = fare("LTN", "BUD", kD, "25", 20);
    const FlightResultPtr early = fare("LTN", "BUD", kD, "25", 7);

    // [A]
    const std::vector<RoundTrip> ok = rt::combine(std::vector{morning}, std::vector{late}, 0, 3);
    ASSERT_EQ(ok.size(), 1u);
    EXPECT_EQ(ok[0].nights, 0);
    EXPECT_EQ(ok[0].total, Decimal{45});
    EXPECT_TRUE(rt::combine(std::vector{evening}, std::vector{early}, 0, 3).empty());
    EXPECT_TRUE(rt::combine(std::vector{evening}, std::vector{late}, 0, 3).empty());

    // [D] 1490.8137 km → 2.48775 h; zone round(−1.30826) = −1; required 4.48775 h.
    const rt::SameDayRequirement r =
        rt::same_day_requirement(station("BUD")->location, station("LTN")->location);
    EXPECT_NEAR(r.distance_km, 1490.8137, 5e-5);
    EXPECT_NEAR(r.flight_hours, 2.48775, 5e-6);
    EXPECT_NEAR((station("LTN")->location.longitude - station("BUD")->location.longitude) / 15.0, -1.30826, 5e-6);
    EXPECT_EQ(r.zone_offset_hours, -1.0);
    EXPECT_NEAR(r.required_hours, 4.48775, 5e-6);
    // Gaps: morning/late 14 h, evening/early −11 h, evening/late 2 h.
    EXPECT_EQ(late->fare.cheapest_departure - morning->fare.cheapest_departure, Duration{14h});
    EXPECT_EQ(early->fare.cheapest_departure - evening->fare.cheapest_departure, Duration{-11h});
    EXPECT_EQ(late->fare.cheapest_departure - evening->fare.cheapest_departure, Duration{2h});
    EXPECT_TRUE(rt::same_day_feasible(*morning, *late));
    EXPECT_FALSE(rt::same_day_feasible(*evening, *early));
    EXPECT_FALSE(rt::same_day_feasible(*evening, *late));
}

// ---------------------------------------------------------------------------
// 05 §7.1 P-28
// ---------------------------------------------------------------------------

TEST(RoundTrips, P_28_Combine_edge_cases)
{
    const std::vector<FlightResultPtr> o{fare("BUD", "LTN", kD, "20")};
    const std::vector<FlightResultPtr> r{fare("LTN", "BUD", d_plus(2), "30")};

    EXPECT_EQ(rt::combine(o, r, -1, 2).size(), 1u);  // minimum clamped to 0
    EXPECT_TRUE(rt::combine(o, r, 3, 2).empty());
    EXPECT_TRUE(rt::combine(o, r, 2, 2, 0).empty());
    // Around the edges.
    EXPECT_EQ(rt::combine(o, r, 2, 2).size(), 1u);
    EXPECT_TRUE(rt::combine(o, r, 2, 2, -1).empty());
    EXPECT_TRUE(rt::combine(o, r, -5, -1).empty());  // max < clamped min
    EXPECT_TRUE(rt::combine(o, r, 0, 1).empty());
    EXPECT_EQ(rt::combine(o, r, 0, std::numeric_limits<int>::max()).size(), 1u);
    EXPECT_TRUE(rt::combine(o, {}, 0, 10).empty());
    EXPECT_TRUE(rt::combine({}, r, 0, 10).empty());
}

// ---------------------------------------------------------------------------
// 03 §10.3 worked numbers
// ---------------------------------------------------------------------------

TEST(RoundTrips, SameDayFeasible_03_10_3_BUD_LTN_worked_numbers)
{
    const AirportPtr bud = station("BUD");
    const AirportPtr ltn = station("LTN");
    const rt::SameDayRequirement r = rt::same_day_requirement(bud->location, ltn->location);
    EXPECT_NEAR(r.distance_km, 1490.814, 5e-4);
    EXPECT_NEAR(r.flight_hours, 2.4878, 5e-5);
    EXPECT_NEAR((ltn->location.longitude - bud->location.longitude) / 15.0, -1.3083, 5e-5);
    EXPECT_NEAR(r.required_hours, 4.4878, 5e-5);
    // 4 h 29 m 16 s once whole seconds are reached.
    EXPECT_GT(r.required, Duration{4h + 29min + 15s});
    EXPECT_LE(r.required, Duration{4h + 29min + 16s});

    const auto feasible = [&](int out_h, int out_m, int ret_h, int ret_m, int ret_s = 0) {
        return rt::same_day_feasible(*direct_flight(bud, ltn, kD, out_h, out_m),
                                     *direct_flight(ltn, bud, kD, ret_h, ret_m, ret_s));
    };
    EXPECT_TRUE(feasible(6, 0, 20, 0));
    EXPECT_FALSE(feasible(18, 0, 20, 0));
    EXPECT_FALSE(feasible(6, 0, 9, 10));
    EXPECT_TRUE(feasible(6, 0, 10, 29, 16));
    EXPECT_FALSE(feasible(6, 0, 10, 29, 15));
}

TEST(RoundTrips, SameDayFeasible_03_10_3_LTN_BUD_needs_the_eastbound_hour)
{
    const AirportPtr bud = station("BUD");
    const AirportPtr ltn = station("LTN");
    const rt::SameDayRequirement r = rt::same_day_requirement(ltn->location, bud->location);
    EXPECT_EQ(r.zone_offset_hours, 1.0);
    EXPECT_NEAR(r.required_hours, 6.4878, 5e-5);
    // Out 06:00 needs a return at ≥ 12:29:16 local.
    EXPECT_TRUE(rt::same_day_feasible(*direct_flight(ltn, bud, kD, 6, 0), *direct_flight(bud, ltn, kD, 12, 29, 16)));
    EXPECT_FALSE(rt::same_day_feasible(*direct_flight(ltn, bud, kD, 6, 0), *direct_flight(bud, ltn, kD, 12, 29, 15)));
}

TEST(RoundTrips, SameDayFeasible_03_10_3_LTN_EVN_worked_numbers)
{
    const AirportPtr ltn = hand_airport("LTN", 51.87, -0.37);
    const AirportPtr evn = hand_airport("EVN", 40.15, 44.40);
    const rt::SameDayRequirement r = rt::same_day_requirement(ltn->location, evn->location);
    EXPECT_NEAR(r.distance_km, 3627.26, 5e-3);
    EXPECT_NEAR(r.flight_hours, 5.336, 5e-4);
    EXPECT_NEAR((evn->location.longitude - ltn->location.longitude) / 15.0, 2.985, 5e-4);
    EXPECT_EQ(r.zone_offset_hours, 3.0);
    EXPECT_NEAR(r.required_hours, 11.336, 5e-4);
}

TEST(RoundTrips, SameDayFeasible_westbound_never_needs_less_than_two_hours_and_two_hours_is_enough)
{
    // Close to the pole a quarter turn west is a short hop: 0.7 h flight − 6 h zone + 3 < 2.
    const AirportPtr a = hand_airport("AAA", 89.0, 0.0);
    const AirportPtr b = hand_airport("BBB", 89.0, -90.0);
    const rt::SameDayRequirement r = rt::same_day_requirement(a->location, b->location);
    EXPECT_EQ(r.zone_offset_hours, -6.0);
    EXPECT_LT(r.flight_hours + r.zone_offset_hours + 3.0, 2.0);
    EXPECT_EQ(r.required_hours, 2.0);
    EXPECT_EQ(r.required, Duration{2h});
    EXPECT_TRUE(rt::same_day_feasible(*direct_flight(a, b, kD, 6), *direct_flight(b, a, kD, 8)));  // exactly 2 h
    EXPECT_FALSE(rt::same_day_feasible(*direct_flight(a, b, kD, 6), *direct_flight(b, a, kD, 7, 59, 59)));
}

TEST(RoundTrips, SameDayFeasible_zone_offset_rounds_half_to_even)
{
    // Δlon/15 = 2.5 exactly: Math.Round gives 2 (std::round would give 3); 1.5 → 2; −2.5 → −2.
    const auto zone = [](double from_lon, double to_lon) {
        return rt::same_day_requirement(GeoPoint{0.0, from_lon}, GeoPoint{0.0, to_lon}).zone_offset_hours;
    };
    EXPECT_EQ(zone(0.0, 37.5), 2.0);
    EXPECT_EQ(zone(0.0, 22.5), 2.0);
    EXPECT_EQ(zone(37.5, 0.0), -2.0);
    EXPECT_EQ(zone(0.0, 7.5), 0.0);
    EXPECT_EQ(zone(0.0, 52.5), 4.0);
}

TEST(RoundTrips, SameDayFeasible_uses_only_the_outbound_airports)
{
    // The return leaves from another airport; the rule still measures BUD→LTN.
    const FlightResultPtr out = fare("BUD", "LTN", kD, "20", 6);
    const FlightResultPtr ret_far = fare("LGW", "BUD", kD, "25", 11);  // 5 h after: ≥ 4.48775 h
    EXPECT_TRUE(rt::same_day_feasible(*out, *ret_far));
    EXPECT_THROW((void)rt::same_day_feasible(FlightResult{}, *ret_far), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// 03 §10.2 MatchLegs: the remaining rules and texts (01 §11.8)
// ---------------------------------------------------------------------------

TEST(RoundTrips, MatchLegs_reports_returns_that_do_not_land_at_home)
{
    const LegSearchResult out = leg("Budapest", 0, {fare("BUD", "LTN", kD, "20")});
    const LegSearchResult ret = leg("London", 0, {fare("LTN", "KTW", d_plus(3), "10")});
    const LegMatch match = rt::match_legs(out, ret);
    ASSERT_TRUE(match.problem.has_value());
    EXPECT_EQ(*match.problem, "None of the return fares lands at an airport near where the outbound starts. "
                              "Search the return back to your home area.");
    EXPECT_TRUE(match.outbound.empty());
    EXPECT_TRUE(match.inbound.empty());
}

TEST(RoundTrips, MatchLegs_checks_currency_then_adults_then_fare_type)
{
    const LegSearchResult out = leg("Budapest", 0, {fare("BUD", "LTN", kD, "20")});

    LegSearchResult all_differ = leg("London", 0, {}, "GBP", 3);
    all_differ.request.price_type = PriceType::wizz_discount_club;
    EXPECT_EQ(rt::match_legs(out, all_differ).problem,
              "Outbound prices are in EUR but return prices in GBP. Run both with the same currency.");

    LegSearchResult adults_and_type = leg("London", 0, {}, "EUR", 3);
    adults_and_type.request.price_type = PriceType::wizz_discount_club;
    EXPECT_EQ(rt::match_legs(out, adults_and_type).problem,
              "Outbound was searched for 1 adult(s) but the return for 3. Search both legs with the same number.");

    // Currency compares ci but prints the raw request values; adults compare raw.
    EXPECT_FALSE(rt::match_legs(out, leg("London", 0, {}, "eur")).problem.has_value());
    EXPECT_EQ(rt::match_legs(leg("Budapest", 0, {}, "eur"), leg("London", 0, {}, "gbp")).problem,
              "Outbound prices are in eur but return prices in gbp. Run both with the same currency.");
    EXPECT_EQ(rt::match_legs(leg("Budapest", 0, {}, "EUR", 0), leg("London", 0, {}, "EUR", 1)).problem,
              "Outbound was searched for 0 adult(s) but the return for 1. Search both legs with the same number.");
}

TEST(RoundTrips, MatchLegs_without_flights_on_a_side_is_no_problem)
{
    // 03 §10.2: a leg without flights raises no problem; the other leg is still filtered.
    const LegMatch neither = rt::match_legs(leg("Budapest", 0, {}), leg("London", 0, {}));
    EXPECT_FALSE(neither.problem.has_value());
    EXPECT_TRUE(neither.outbound.empty());
    EXPECT_TRUE(neither.inbound.empty());

    const LegSearchResult out = leg("Budapest", 0, {fare("BUD", "LTN", kD, "20"), fare("BUD", "BCN", kD, "10")});
    const LegMatch no_return = rt::match_legs(out, leg("London", 0, {}));
    EXPECT_FALSE(no_return.problem.has_value());
    EXPECT_EQ(routes(no_return.outbound), (std::vector<std::string>{"BUD→LTN"}));
    EXPECT_TRUE(no_return.inbound.empty());
    EXPECT_TRUE(rt::combine(no_return.outbound, no_return.inbound, 2, 10).empty());

    const LegMatch no_outbound =
        rt::match_legs(leg("Budapest", 0, {}), leg("London", 0, {fare("LTN", "BUD", d_plus(3), "25")}));
    EXPECT_FALSE(no_outbound.problem.has_value());
    EXPECT_TRUE(no_outbound.outbound.empty());
    EXPECT_EQ(routes(no_outbound.inbound), (std::vector<std::string>{"LTN→BUD"}));

    // The pseudocode is literal: fares on one side that land nowhere useful are still
    // reported when the other side is empty (03 §10.2 checks 4 and 5 look at one leg each).
    EXPECT_EQ(rt::match_legs(leg("Budapest", 0, {fare("BUD", "BCN", kD, "10")}), leg("London", 0, {})).problem,
              "The return was searched from LGW, LTN, but none of the outbound fares land there. "
              "Search the return from your destination.");
    EXPECT_EQ(rt::match_legs(leg("Budapest", 0, {}), leg("London", 0, {fare("LTN", "KTW", d_plus(3), "10")})).problem,
              "None of the return fares lands at an airport near where the outbound starts. "
              "Search the return back to your home area.");
}

TEST(RoundTrips, MatchLegs_same_area_return_plan_names_no_airport)
{
    const LegSearchResult out = leg("Budapest", 0, {fare("BUD", "LTN", kD, "20")});
    LegSearchResult ret = leg("London", 0, {fare("LTN", "BUD", d_plus(3), "25")});
    ret.plan.origin_airports.clear();  // a SameArea plan has no origin airports
    ret.plan.same_area = true;
    EXPECT_EQ(rt::match_legs(out, ret).problem,
              "The return was searched from , but none of the outbound fares land there. "
              "Search the return from your destination.");
}

TEST(RoundTrips, MatchLegs_keeps_the_leg_order)
{
    const LegSearchResult out = leg("Budapest", 200,
                                    {fare("BTS", "LTN", d_plus(2), "15"), fare("BUD", "NAP", kD, "16"),
                                     fare("BUD", "LGW", d_plus(1), "17"), fare("DEB", "LTN", kD, "18")});
    const LegSearchResult ret = leg("London", 30,
                                    {fare("LGW", "BUD", d_plus(5), "11"), fare("LTN", "KTW", d_plus(4), "12"),
                                     fare("LTN", "DEB", d_plus(6), "13"), fare("LTN", "BTS", d_plus(3), "14")});
    const LegMatch match = rt::match_legs(out, ret);
    EXPECT_FALSE(match.problem.has_value());
    EXPECT_EQ(routes(match.outbound), (std::vector<std::string>{"BTS→LTN", "BUD→LGW", "DEB→LTN"}));
    EXPECT_EQ(routes(match.inbound), (std::vector<std::string>{"LGW→BUD", "LTN→DEB", "LTN→BTS"}));
}

TEST(RoundTrips, AwayMatches_exact_code_or_real_city_sibling_only)
{
    const LegSearchResult london = leg("LTN", 0, {});
    EXPECT_TRUE(rt::away_matches(*station("LTN"), london.plan.origin_airports));
    EXPECT_TRUE(rt::away_matches(*station("LGW"), london.plan.origin_airports));
    EXPECT_FALSE(rt::away_matches(*station("BUD"), london.plan.origin_airports));
    const LegSearchResult katowice = leg("Katowice", 0, {});
    EXPECT_TRUE(rt::away_matches(*station("KTW"), katowice.plan.origin_airports));
    EXPECT_FALSE(rt::away_matches(*station("KRK"), katowice.plan.origin_airports));  // SPQ is regional

    // Codes compare ci; the city-group flag of the arrival decides; null entries are ignored.
    const AirportPtr lower_ltn = hand_airport("ltn", 51.87, -0.37, "lon", true);
    const std::vector<AreaAirport> area{{nullptr, 0.0}, {station("LTN"), 0.0}};
    EXPECT_TRUE(rt::away_matches(*lower_ltn, area));
    const AirportPtr sibling_without_flag = hand_airport("XXX", 51.0, 0.0, "LON", false);
    EXPECT_FALSE(rt::away_matches(*sibling_without_flag, area));
    const AirportPtr sibling_with_flag = hand_airport("YYY", 51.0, 0.0, "lon", true);
    EXPECT_TRUE(rt::away_matches(*sibling_with_flag, area));
    EXPECT_FALSE(rt::away_matches(*sibling_with_flag, std::vector<AreaAirport>{}));
}

// ---------------------------------------------------------------------------
// 03 §10.1 RoundTrip
// ---------------------------------------------------------------------------

TEST(RoundTrips, RoundTrip_properties)
{
    const FlightResultPtr out = fare("BUD", "LTN", kD, "20.5");
    const FlightResultPtr back = fare("LTN", "BUD", d_plus(7), "0.75");
    const RoundTrip trip = rt::make_round_trip(out, back);
    EXPECT_EQ(trip.outbound, out);
    EXPECT_EQ(trip.inbound, back);
    EXPECT_EQ(trip.nights, 7);
    EXPECT_EQ(trip.total, dec("21.25"));
    EXPECT_TRUE(trip.same_home_airport);
    EXPECT_TRUE(trip.same_away_airport);
    EXPECT_EQ(trip.combined_booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-17/1/0/0/null");

    // Lifted add: no total when either leg has none.
    auto no_price = std::make_shared<FlightResult>(*back);
    no_price->total_converted.reset();
    EXPECT_FALSE(rt::make_round_trip(out, no_price).total.has_value());
    EXPECT_FALSE(rt::make_round_trip(no_price, out).total.has_value());

    // Codes compare ci; the link upper-cases them and clamps adults to ≥ 1.
    const AirportPtr bud = hand_airport("bud", 47.43, 19.26);
    const AirportPtr ltn = hand_airport("ltn", 51.87, -0.37);
    auto lower_out = std::make_shared<FlightResult>(*direct_flight(bud, ltn, kD, 6));
    lower_out->adults = 0;
    const RoundTrip mixed = rt::make_round_trip(lower_out, direct_flight(station("LTN"), station("BUD"), d_plus(2), 9));
    EXPECT_TRUE(mixed.same_home_airport);
    EXPECT_TRUE(mixed.same_away_airport);
    EXPECT_EQ(mixed.total, Decimal{20});
    EXPECT_EQ(mixed.combined_booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-10/2026-11-12/1/0/0/null");

    // Two-adult outbound: the link carries the outbound's adults.
    const RoundTrip two = rt::make_round_trip(fare("BTS", "LTN", d_plus(2), "14.99", 7, 2),
                                              fare("LTN", "BTS", d_plus(5), "23.93", 12, 2));
    EXPECT_EQ(two.total, dec("77.84"));
    EXPECT_EQ(two.combined_booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/2026-11-15/2/0/0/null");

    // Only one side shared: no combined link (01 §9: two one-way bookings).
    const RoundTrip open_jaw = rt::make_round_trip(out, fare("LGW", "BUD", d_plus(3), "1"));
    EXPECT_TRUE(open_jaw.same_home_airport);
    EXPECT_FALSE(open_jaw.same_away_airport);
    EXPECT_FALSE(open_jaw.combined_booking_url.has_value());

    EXPECT_THROW((void)rt::make_round_trip(nullptr, back), std::invalid_argument);
    EXPECT_THROW((void)rt::make_round_trip(out, nullptr), std::invalid_argument);
    EXPECT_THROW((void)rt::make_round_trip(std::make_shared<FlightResult>(), back), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// 03 §10.4 Combine: eligibility and the full 07 §3 key (D-12)
// ---------------------------------------------------------------------------

TEST(RoundTrips, Combine_skips_unavailable_and_unconverted_fares)
{
    const FlightResultPtr out = fare("BUD", "LTN", kD, "20");
    auto check_on_site = std::make_shared<FlightResult>(*fare("BUD", "LTN", d_plus(1), "1"));
    check_on_site->fare.status = FareStatus::check_on_site;
    auto unconverted = std::make_shared<FlightResult>(*fare("BUD", "LTN", d_plus(1), "2"));
    unconverted->total_converted.reset();
    unconverted->price_per_person_converted.reset();
    const FlightResultPtr back = fare("LTN", "BUD", d_plus(4), "30");
    auto back_on_site = std::make_shared<FlightResult>(*fare("LTN", "BUD", d_plus(4), "3"));
    back_on_site->fare.status = FareStatus::check_on_site;

    const std::vector<RoundTrip> trips = rt::combine(std::vector<FlightResultPtr>{check_on_site, nullptr, unconverted, out},
                                                     std::vector<FlightResultPtr>{back_on_site, back, nullptr}, 1, 10);
    ASSERT_EQ(trips.size(), 1u);
    EXPECT_EQ(trips[0].outbound, out);
    EXPECT_EQ(trips[0].inbound, back);
    EXPECT_EQ(trips[0].total, Decimal{50});
}

TEST(RoundTrips, Combine_checks_same_day_feasibility_only_for_zero_nights)
{
    const FlightResultPtr evening = fare("BUD", "LTN", kD, "20", 18);
    const FlightResultPtr early_next_day = fare("LTN", "BUD", d_plus(1), "25", 7);
    const std::vector<RoundTrip> trips = rt::combine(std::vector{evening}, std::vector{early_next_day}, 0, 3);
    ASSERT_EQ(trips.size(), 1u);
    EXPECT_EQ(trips[0].nights, 1);
}

TEST(RoundTrips, Combine_ranks_by_the_full_key)
{
    // Same total, date and nights: the outbound departure decides.
    const FlightResultPtr out_late = fare("BUD", "LTN", kD, "20", 17);
    const FlightResultPtr out_early = fare("BUD", "LTN", kD, "20", 6);
    const FlightResultPtr back = fare("LTN", "BUD", d_plus(3), "30");
    std::vector<RoundTrip> trips = rt::combine(std::vector{out_late, out_early}, std::vector{back}, 1, 5);
    ASSERT_EQ(trips.size(), 2u);
    EXPECT_EQ(trips[0].outbound, out_early);
    EXPECT_EQ(trips[1].outbound, out_late);

    // ... then the return departure.
    const FlightResultPtr back_late = fare("LTN", "BUD", d_plus(3), "30", 20);
    const FlightResultPtr back_early = fare("LTN", "BUD", d_plus(3), "30", 7);
    trips = rt::combine(std::vector{out_early}, std::vector{back_late, back_early}, 1, 5);
    ASSERT_EQ(trips.size(), 2u);
    EXPECT_EQ(trips[0].inbound, back_early);
    EXPECT_EQ(trips[1].inbound, back_late);

    // ... then the outbound from / to codes, then the return from / to codes.
    const FlightResultPtr from_bud = fare("BUD", "LTN", kD, "20");
    const FlightResultPtr from_bts = fare("BTS", "LTN", kD, "20");
    const FlightResultPtr to_lgw = fare("BUD", "LGW", kD, "20");
    trips = rt::combine(std::vector{from_bud, to_lgw, from_bts}, std::vector{back}, 1, 5);
    ASSERT_EQ(trips.size(), 3u);
    EXPECT_EQ(trips[0].outbound, from_bts);
    EXPECT_EQ(trips[1].outbound, to_lgw);
    EXPECT_EQ(trips[2].outbound, from_bud);
    const FlightResultPtr ret_ltn_bud = fare("LTN", "BUD", d_plus(3), "30");
    const FlightResultPtr ret_ltn_bts = fare("LTN", "BTS", d_plus(3), "30");
    const FlightResultPtr ret_lgw_bud = fare("LGW", "BUD", d_plus(3), "30");
    trips = rt::combine(std::vector{from_bud}, std::vector{ret_ltn_bud, ret_ltn_bts, ret_lgw_bud}, 1, 5);
    ASSERT_EQ(trips.size(), 3u);
    EXPECT_EQ(trips[0].inbound, ret_lgw_bud);
    EXPECT_EQ(trips[1].inbound, ret_ltn_bts);
    EXPECT_EQ(trips[2].inbound, ret_ltn_bud);

    // Total before date, date before nights: C (49) first, then A (earlier date,
    // 3 and 5 nights) before B (later date, 2 and 4 nights).
    const FlightResultPtr b_out = fare("BUD", "LTN", d_plus(1), "20");
    const FlightResultPtr a_out = fare("BUD", "LTN", kD, "20");
    const FlightResultPtr c_out = fare("BUD", "LTN", d_plus(2), "19");
    const FlightResultPtr r3 = fare("LTN", "BUD", d_plus(3), "30");
    const FlightResultPtr r5 = fare("LTN", "BUD", d_plus(5), "30");
    trips = rt::combine(std::vector{b_out, a_out, c_out}, std::vector{r3, r5}, 2, 5);
    ASSERT_EQ(trips.size(), 5u);
    EXPECT_TRUE(sorted_by_rank(trips));
    EXPECT_EQ(trips[0].outbound, c_out);
    EXPECT_EQ(trips[0].total, Decimal{49});
    EXPECT_EQ(trips[1].outbound, a_out);
    EXPECT_EQ(trips[1].nights, 3);
    EXPECT_EQ(trips[2].outbound, a_out);
    EXPECT_EQ(trips[2].nights, 5);
    EXPECT_EQ(trips[3].outbound, b_out);
    EXPECT_EQ(trips[3].nights, 2);
    EXPECT_EQ(trips[4].outbound, b_out);
    EXPECT_EQ(trips[4].nights, 4);
}

TEST(RoundTrips, Combine_eviction_at_the_limit_uses_the_full_key)
{
    // D-12: .NET kept the first-come trip at an equal total; 07 §3 keeps the trip
    // whose key ranks first, so BTS (generated second) wins over BUD.
    const FlightResultPtr from_bud = fare("BUD", "LTN", kD, "20");
    const FlightResultPtr from_bts = fare("BTS", "LTN", kD, "20");
    const FlightResultPtr back = fare("LTN", "BUD", d_plus(3), "30");
    const std::vector<RoundTrip> best = rt::combine(std::vector{from_bud, from_bts}, std::vector{back}, 1, 5, 1);
    ASSERT_EQ(best.size(), 1u);
    EXPECT_EQ(best[0].outbound, from_bts);
}

TEST(RoundTrips, Combine_identical_keys_keep_generation_order)
{
    // Two distinct but identical fares: only the generation number separates them.
    const FlightResultPtr first = fare("BUD", "LTN", kD, "20");
    const FlightResultPtr second = std::make_shared<FlightResult>(*first);
    const FlightResultPtr back = fare("LTN", "BUD", d_plus(3), "30");
    const FlightResultPtr back_twin = std::make_shared<FlightResult>(*back);

    const std::vector<RoundTrip> all = rt::combine(std::vector{first, second}, std::vector{back, back_twin}, 1, 5);
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[0].outbound, first);
    EXPECT_EQ(all[0].inbound, back);
    EXPECT_EQ(all[1].outbound, first);
    EXPECT_EQ(all[1].inbound, back_twin);
    EXPECT_EQ(all[2].outbound, second);
    EXPECT_EQ(all[2].inbound, back);
    EXPECT_EQ(all[3].outbound, second);
    EXPECT_EQ(all[3].inbound, back_twin);
    for (int limit = 1; limit <= 4; ++limit) {
        SCOPED_TRACE(limit);
        EXPECT_TRUE(same_trips(rt::combine(std::vector{first, second}, std::vector{back, back_twin}, 1, 5, limit),
                               std::span{all}.first(static_cast<std::size_t>(limit))));
    }
}

TEST(RoundTrips, Combine_tries_each_days_returns_cheapest_first)
{
    // Same day, same key apart from the total: the cheaper return ranks first,
    // and at equal totals the input order of that day is kept (stable sort).
    const FlightResultPtr out = fare("BUD", "LTN", kD, "20");
    const FlightResultPtr dear = fare("LTN", "BUD", d_plus(3), "40");
    const FlightResultPtr cheap = fare("LTN", "BUD", d_plus(3), "30");
    const FlightResultPtr cheap_twin = std::make_shared<FlightResult>(*cheap);
    const std::vector<RoundTrip> trips = rt::combine(std::vector{out}, std::vector{dear, cheap, cheap_twin}, 1, 5);
    ASSERT_EQ(trips.size(), 3u);
    EXPECT_EQ(trips[0].inbound, cheap);
    EXPECT_EQ(trips[1].inbound, cheap_twin);
    EXPECT_EQ(trips[2].inbound, dear);
}

// ---------------------------------------------------------------------------
// 03 §11 worked end-to-end example: MatchLegs + Combine(min 2, max 5)
// ---------------------------------------------------------------------------

namespace worked_example {

[[nodiscard]] DayFare day_fare(std::string from, std::string to, Date date, int hour, int minute,
                               std::string_view amount, std::string currency)
{
    return DayFare{.from = std::move(from),
                   .to = std::move(to),
                   .date = date,
                   .cheapest_departure = make_local(date, hour, minute),
                   .departures = {make_local(date, hour, minute)},
                   .price_per_person = Money{dec(amount), std::move(currency)},
                   .status = FareStatus::available,
                   .has_mac_flight = false};
}

[[nodiscard]] Date nov(unsigned day) noexcept
{
    return Date::from_ymd(2026, 11, day);
}

/// Budapest r200 → London r30, 10–20 Nov 2026, 2 adults, EUR: the 12 result rows (03 §11 table, SortByPrice order).
[[nodiscard]] LegSearchResult outbound_leg()
{
    LegSearchRequest request;
    request.origin = match("Budapest");
    request.origin_radius_km = 200;
    request.destination = match("London");
    request.destination_radius_km = 30;
    request.from = nov(10);
    request.to = nov(20);
    request.adults = 2;
    request.currency = "EUR";
    const DayFare rows[] = {
        day_fare("BTS", "LTN", nov(12), 7, 15, "14.99", "EUR"),   day_fare("BUD", "LTN", nov(14), 6, 0, "19290.0", "HUF"),
        day_fare("BUD", "LTN", nov(18), 6, 0, "19290.0", "HUF"),  day_fare("BUD", "LTN", nov(19), 12, 50, "19290.0", "HUF"),
        day_fare("BUD", "LTN", nov(20), 6, 0, "19290.0", "HUF"),  day_fare("BUD", "LTN", nov(11), 6, 0, "21590.0", "HUF"),
        day_fare("BUD", "LTN", nov(12), 6, 0, "21590.0", "HUF"),  day_fare("BUD", "LTN", nov(13), 12, 50, "21590.0", "HUF"),
        day_fare("BUD", "LTN", nov(17), 6, 0, "21590.0", "HUF"),  day_fare("BUD", "LTN", nov(16), 6, 0, "25390.0", "HUF"),
        day_fare("BUD", "LTN", nov(10), 6, 0, "30790.0", "HUF"),  day_fare("BUD", "LGW", nov(15), 8, 30, "34690.0", "HUF"),
    };
    std::vector<FlightResultPtr> flights;
    for (const DayFare& row : rows) {
        flights.push_back(build_result(request, row));
    }
    return make_leg(std::move(request), std::move(flights));
}

/// London r30 → Budapest r200, 12–20 Nov, 2 adults, EUR: the synthetic GBP fares of 03 §11.
[[nodiscard]] LegSearchResult return_leg()
{
    LegSearchRequest request;
    request.origin = match("London");
    request.origin_radius_km = 30;
    request.destination = match("Budapest");
    request.destination_radius_km = 200;
    request.from = nov(12);
    request.to = nov(20);
    request.adults = 2;
    request.currency = "EUR";
    const DayFare rows[] = {
        day_fare("LTN", "BUD", nov(17), 21, 35, "17.99", "GBP"),
        day_fare("LTN", "BTS", nov(15), 12, 0, "19.99", "GBP"),
        day_fare("LTN", "BUD", nov(14), 10, 5, "24.99", "GBP"),
        day_fare("LGW", "BUD", nov(16), 14, 0, "29.99", "GBP"),
    };
    std::vector<FlightResultPtr> flights;
    for (const DayFare& row : rows) {
        flights.push_back(build_result(request, row));
    }
    return make_leg(std::move(request), std::move(flights));
}

} // namespace worked_example

TEST(RoundTrips, Worked_example_03_11_legs_reproduce_the_tables)
{
    const LegSearchResult out = worked_example::outbound_leg();
    const LegSearchResult ret = worked_example::return_leg();

    std::vector<Decimal> out_totals;
    for (const FlightResultPtr& f : out.flights) {
        out_totals.push_back(*f->total_converted);
    }
    EXPECT_EQ(out_totals, (std::vector<Decimal>{dec("29.98"), dec("105.32"), dec("105.32"), dec("105.32"),
                                                dec("105.32"), dec("117.88"), dec("117.88"), dec("117.88"),
                                                dec("117.88"), dec("138.62"), dec("168.12"), dec("189.40")}));
    EXPECT_EQ(out.flights[1]->price_per_person_converted, dec("52.66"));
    EXPECT_EQ(out.flights[0]->booking_url,
              "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/null/2/0/0/null");

    std::vector<Decimal> ret_per_person;
    std::vector<Decimal> ret_totals;
    for (const FlightResultPtr& f : ret.flights) {
        ret_per_person.push_back(*f->price_per_person_converted);
        ret_totals.push_back(*f->total_converted);
    }
    EXPECT_EQ(ret_per_person, (std::vector<Decimal>{dec("21.54"), dec("23.93"), dec("29.92"), dec("35.90")}));
    EXPECT_EQ(ret_totals, (std::vector<Decimal>{dec("43.08"), dec("47.86"), dec("59.84"), dec("71.80")}));

    // Plans: origins BUD 0, BTS 172.5, DEB 184.1 and LGW 40.6, LTN 44.1.
    EXPECT_EQ(origin_codes(out), (std::vector<std::string>{"BUD", "BTS", "DEB"}));
    EXPECT_NEAR(out.plan.origin_airports[1].distance_km, 172.5, 0.05);
    EXPECT_NEAR(out.plan.origin_airports[2].distance_km, 184.1, 0.05);
    EXPECT_EQ(origin_codes(ret), (std::vector<std::string>{"LGW", "LTN"}));
}

TEST(RoundTrips, Worked_example_03_11_MatchLegs_keeps_every_fare)
{
    const LegSearchResult out = worked_example::outbound_leg();
    const LegSearchResult ret = worked_example::return_leg();
    const LegMatch match = rt::match_legs(out, ret);
    EXPECT_FALSE(match.problem.has_value());
    EXPECT_EQ(match.outbound, out.flights);  // away = [LGW, LTN]: all 12 match by IATA
    EXPECT_EQ(match.inbound, ret.flights);   // home = {BUD, BTS, DEB}: all 4 match
}

TEST(RoundTrips, Worked_example_03_11_Combine_min_2_max_5)
{
    const LegSearchResult out = worked_example::outbound_leg();
    const LegSearchResult ret = worked_example::return_leg();
    const LegMatch match = rt::match_legs(out, ret);

    const std::vector<RoundTrip> trips = rt::combine(match.outbound, match.inbound, 2, 5);

    ASSERT_EQ(trips.size(), 19u);
    EXPECT_TRUE(sorted_by_rank(trips));

    struct Row {
        std::string_view total;
        std::string_view out_route;
        Date out_date;
        std::string_view ret_route;
        Date ret_date;
        int nights;
        bool same_home;
        bool same_away;
        std::optional<std::string_view> url;
    };
    using worked_example::nov;
    const Row expected[] = {
        {"73.06", "BTS→LTN", nov(12), "LTN→BUD", nov(17), 5, false, true, std::nullopt},
        {"77.84", "BTS→LTN", nov(12), "LTN→BTS", nov(15), 3, true, true,
         "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/2026-11-15/2/0/0/null"},
        {"89.82", "BTS→LTN", nov(12), "LTN→BUD", nov(14), 2, false, true, std::nullopt},
        {"101.78", "BTS→LTN", nov(12), "LGW→BUD", nov(16), 4, false, false, std::nullopt},
        {"148.40", "BUD→LTN", nov(14), "LTN→BUD", nov(17), 3, true, true,
         "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-14/2026-11-17/2/0/0/null"},
        // Then two trips at 160.96 (BUD→LTN 117.88 + LTN→BUD 43.08), earlier outbound first.
        {"160.96", "BUD→LTN", nov(12), "LTN→BUD", nov(17), 5, true, true,
         "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-12/2026-11-17/2/0/0/null"},
        {"160.96", "BUD→LTN", nov(13), "LTN→BUD", nov(17), 4, true, true,
         "https://www.wizzair.com/en-gb/booking/select-flight/BUD/LTN/2026-11-13/2026-11-17/2/0/0/null"},
    };
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        SCOPED_TRACE(i);
        const RoundTrip& t = trips[i];
        EXPECT_EQ(t.total, dec(expected[i].total));
        EXPECT_EQ(route(t.outbound), expected[i].out_route);
        EXPECT_EQ(t.outbound->date(), expected[i].out_date);
        EXPECT_EQ(route(t.inbound), expected[i].ret_route);
        EXPECT_EQ(t.inbound->date(), expected[i].ret_date);
        EXPECT_EQ(t.nights, expected[i].nights);
        EXPECT_EQ(t.same_home_airport, expected[i].same_home);
        EXPECT_EQ(t.same_away_airport, expected[i].same_away);
        EXPECT_EQ(t.combined_booking_url, expected[i].url);
    }
    // The capped UI call (limit 500) and any smaller limit agree with the full list.
    EXPECT_TRUE(same_trips(rt::combine(match.outbound, match.inbound, 2, 5, 3), std::span{trips}.first(3)));
}

} // namespace
