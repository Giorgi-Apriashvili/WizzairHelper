// SearchPlanner on the fixture map and on hand-built catalogs: 05 §3.6 CT-PLAN-01..06,
// §3.10 CT-PLANR-01..07, CT-LATENT-02 (planning part), 05 §7.1 P-18..P-22 and P-30
// (plan parts), every row of the 03 §4.5 fixture table, the 03 §11 plan
// walk-through, the 03 §4.3.x examples and rules, the 03 §4.4 notes catalogue and
// 03 §5.9 GroupLabel. TODAY = 2026-11-02, locale en-US (05 §0.3, §1.11).

#include "wizz/core/planner.hpp"

#include "wizz/core/catalog.hpp"
#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/format.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/locale.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using wizz::Airport;
using wizz::AirportCatalog;
using wizz::AirportCatalogPtr;
using wizz::AirportPtr;
using wizz::AreaAirport;
using wizz::Date;
using wizz::GeoPoint;
using wizz::LegSearchRequest;
using wizz::LocalDateTime;
using wizz::Locale;
using wizz::Place;
using wizz::PlannedRequest;
using wizz::SearchPlan;
using wizz::planner::NextDeparture;
using Strings = std::vector<std::string>;

namespace planner = wizz::planner;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr Date kToday = Date::from_ymd(2026, 11, 2);

[[nodiscard]] constexpr Date day(int year, unsigned month, unsigned d)
{
    return Date::from_ymd(year, month, d);
}

/// CAT (05 §0.3): the catalog of fixtures/map_subset.json under en-US.
[[nodiscard]] const AirportCatalogPtr& cat()
{
    static const AirportCatalogPtr catalog =
        AirportCatalog::from_map_json(wizz::test::read_fixture("map_subset.json"), Locale::en_us());
    return catalog;
}

[[nodiscard]] AirportPtr station(const AirportCatalog& catalog, std::string_view code)
{
    AirportPtr airport = catalog.find(code);
    if (!airport) {
        throw std::logic_error("station missing");
    }
    return airport;
}

[[nodiscard]] AirportPtr station(std::string_view code)
{
    return station(*cat(), code);
}

/// `CAT.MatchPlace(text)`; the place must exist.
[[nodiscard]] Place match(const AirportCatalog& catalog, std::string_view text)
{
    std::optional<Place> place = catalog.match_place(text);
    if (!place) {
        throw std::logic_error("place missing");
    }
    return *place;
}

[[nodiscard]] Place match(std::string_view text)
{
    return match(*cat(), text);
}

/// A hand-made (geocoded-like) Place with no pins.
[[nodiscard]] Place hand_place(std::string name, GeoPoint location)
{
    Place place;
    place.description = name;
    place.name = std::move(name);
    place.location = location;
    place.source = wizz::PlaceSource::geocoder;
    return place;
}

/// 05 §3.6 `Request(from, fromKm, to, toKm, start, end)`.
[[nodiscard]] LegSearchRequest make_request(Place origin, double origin_km, std::optional<Place> destination,
                                            double destination_km, Date from, Date to)
{
    LegSearchRequest request;
    request.origin = std::move(origin);
    request.origin_radius_km = origin_km;
    request.destination = std::move(destination);
    request.destination_radius_km = destination_km;
    request.from = from;
    request.to = to;
    return request;
}

[[nodiscard]] SearchPlan plan_cat(const LegSearchRequest& request)
{
    return planner::plan(*cat(), request, kToday);
}

/// `REQ(o→q grp g s..e)` rendered as "BUD→LGW [LON] 2026-11-10..2026-11-30".
[[nodiscard]] std::string describe(const PlannedRequest& r)
{
    return r.from->iata + "→" + r.query_to + " [" + r.group_key + "] " + r.start_date.iso() + ".." + r.end_date.iso();
}

[[nodiscard]] Strings describe(const SearchPlan& plan)
{
    Strings result;
    for (const PlannedRequest& r : plan.requests) {
        result.push_back(describe(r));
    }
    return result;
}

[[nodiscard]] Strings codes(const std::vector<AreaAirport>& airports)
{
    Strings result;
    for (const AreaAirport& entry : airports) {
        result.push_back(entry.airport->iata);
    }
    return result;
}

[[nodiscard]] Strings query_tos(const SearchPlan& plan)
{
    Strings result;
    for (const PlannedRequest& r : plan.requests) {
        result.push_back(r.query_to);
    }
    return result;
}

[[nodiscard]] Strings group_keys(const SearchPlan& plan)
{
    Strings result;
    for (const PlannedRequest& r : plan.requests) {
        result.push_back(r.group_key);
    }
    return result;
}

/// Distances rendered with `{x:0}` (07 §2).
[[nodiscard]] Strings km_labels(const std::vector<AreaAirport>& airports, int decimals = 0)
{
    Strings result;
    for (const AreaAirport& entry : airports) {
        result.push_back(entry.airport->iata + " " +
                         wizz::format_label(entry.distance_km, decimals, Locale::en_us()->number_symbols()));
    }
    return result;
}

[[nodiscard]] double round_places(double value, int places)
{
    const double scale = std::pow(10.0, places);
    return std::nearbyint(value * scale) / scale;
}

[[nodiscard]] bool any_contains(const Strings& texts, std::string_view needle)
{
    return std::ranges::any_of(texts, [needle](const std::string& t) { return t.find(needle) != std::string::npos; });
}

[[nodiscard]] LocalDateTime at(Date date, int hour = 0, int minute = 0)
{
    return wizz::make_local(date, hour, minute);
}

/// A hand-built station (Name = IATA unless given; CountryName X, CountryCode XX, EUR).
struct Station {
    std::string iata;
    GeoPoint location;
    std::optional<std::string> mac;
    std::vector<std::pair<std::string, std::optional<LocalDateTime>>> connections;
    std::string name;
    bool fake = false;
};

[[nodiscard]] AirportCatalog hand_catalog(const std::vector<Station>& stations)
{
    std::vector<Airport> airports;
    for (const Station& s : stations) {
        Airport airport;
        airport.iata = s.iata;
        airport.name = s.name.empty() ? s.iata : s.name;
        airport.country_name = "X";
        airport.country_code = "XX";
        airport.currency_code = "EUR";
        airport.location = s.location;
        airport.mac = s.mac;
        airport.is_fake_station = s.fake;
        for (const auto& [code, next] : s.connections) {
            airport.connections.push_back(wizz::RouteConnection{code, next});
        }
        airports.push_back(std::move(airport));
    }
    return AirportCatalog{std::move(airports), Locale::en_us()};
}

/// Connections without a next-departure date.
[[nodiscard]] std::vector<std::pair<std::string, std::optional<LocalDateTime>>> undated(std::initializer_list<const char*> codes)
{
    std::vector<std::pair<std::string, std::optional<LocalDateTime>>> result;
    for (const char* code : codes) {
        result.emplace_back(code, std::nullopt);
    }
    return result;
}

[[nodiscard]] std::string replace_once(std::string text, std::string_view from, std::string_view to)
{
    const std::size_t at_pos = text.find(from);
    if (at_pos == std::string::npos) {
        throw std::logic_error("pattern missing");
    }
    return text.replace(at_pos, from.size(), to);
}

/// The JSON object starting at `begin` (balanced braces; the fixture has no braces in strings).
[[nodiscard]] std::string json_object_at(const std::string& text, std::size_t begin)
{
    int depth = 0;
    for (std::size_t i = begin; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}' && --depth == 0) {
            return text.substr(begin, i - begin + 1);
        }
    }
    throw std::logic_error("unbalanced object");
}

// ---------------------------------------------------------------------------
// 05 §3.6 Search planner
// ---------------------------------------------------------------------------

TEST(Planner, CT_PLAN_01_Nearby_origins_to_a_multi_airport_city_query_each_origin_once_per_group)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 200, match("London"), 30, day(2026, 11, 10), day(2026, 11, 30)));

    // [A]
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"BUD", "BTS", "DEB"}));
    Strings destinations = codes(plan.destination_airports);
    std::ranges::sort(destinations);
    EXPECT_EQ(destinations, (Strings{"LGW", "LTN"}));
    ASSERT_EQ(plan.requests.size(), 3u);
    for (const PlannedRequest& r : plan.requests) {
        EXPECT_EQ(r.group_key, "LON");
        EXPECT_NE(r.query_to, "LON");
    }
    Strings from;
    for (const PlannedRequest& r : plan.requests) {
        from.push_back(r.from->iata);
    }
    EXPECT_EQ(from, (Strings{"BUD", "BTS", "DEB"}));

    // [D]
    EXPECT_EQ(describe(plan), (Strings{"BUD→LGW [LON] 2026-11-10..2026-11-30", "BTS→LTN [LON] 2026-11-10..2026-11-30",
                                       "DEB→LTN [LON] 2026-11-10..2026-11-30"}));
    EXPECT_EQ(codes(plan.destination_airports), (Strings{"LGW", "LTN"}));
    EXPECT_EQ(round_places(plan.destination_airports[0].distance_km, 4), 40.5581);
    EXPECT_EQ(round_places(plan.destination_airports[1].distance_km, 4), 44.0719);
    EXPECT_GT(plan.destination_airports[0].distance_km, 30.0);  // pinned, not inside 30 km
    EXPECT_EQ(plan.routes_considered, 3);
    EXPECT_TRUE(plan.notes.empty());
    // 03 §4.5 row 1 / §11: origin distances and the request's own objects are the catalog's.
    EXPECT_EQ(plan.origin_airports[0].distance_km, 0.0);
    EXPECT_EQ(round_places(plan.origin_airports[1].distance_km, 4), 172.5439);
    EXPECT_EQ(round_places(plan.origin_airports[2].distance_km, 4), 184.0772);
    EXPECT_EQ(plan.requests[0].from, station("BUD"));
    EXPECT_EQ(plan.routes_skipped_not_yet_flying, 0);
    EXPECT_EQ(plan.effective_from, day(2026, 11, 10));
    EXPECT_EQ(plan.effective_to, day(2026, 11, 30));
    EXPECT_FALSE(plan.same_area);
}

TEST(Planner, CT_PLAN_02_Long_ranges_are_chunked_per_route)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 0, match("London"), 0, day(2026, 11, 10), day(2027, 1, 31)));

    // [A]
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"BUD"}));
    ASSERT_EQ(plan.requests.size(), 2u);
    EXPECT_EQ(plan.requests[0].start_date, day(2026, 11, 10));
    EXPECT_EQ(plan.requests[1].end_date, day(2027, 1, 31));
    // [D]
    EXPECT_EQ(describe(plan), (Strings{"BUD→LGW [LON] 2026-11-10..2026-12-21", "BUD→LGW [LON] 2026-12-22..2027-01-31"}));
    EXPECT_EQ(codes(plan.destination_airports), (Strings{"LGW", "LTN"}));
    EXPECT_EQ(plan.origin_airports[0].distance_km, 0.0);
}

TEST(Planner, CT_PLAN_03_Routes_whose_next_flight_is_after_the_window_are_skipped)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 0, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 30)));

    // [A]
    const Strings groups = group_keys(plan);
    EXPECT_EQ(std::ranges::count(groups, "AHO"), 0);
    EXPECT_EQ(std::ranges::count(groups, "NAP"), 0);
    EXPECT_GT(std::ranges::count(groups, "LON"), 0);
    EXPECT_GT(std::ranges::count(groups, "BCN"), 0);
    EXPECT_EQ(plan.routes_skipped_not_yet_flying, 2);
    // [D]
    EXPECT_EQ(describe(plan), (Strings{"BUD→BCN [BCN] 2026-11-10..2026-11-30", "BUD→LGW [LON] 2026-11-10..2026-11-30",
                                       "BUD→KRK [SPQ] 2026-11-10..2026-11-30", "BUD→TIA [TIA] 2026-11-10..2026-11-30"}));
    EXPECT_EQ(plan.routes_considered, 6);
    EXPECT_EQ(plan.notes, (Strings{"2 route(s) skipped because their next flight is after your dates: BUD → Alghero "
                                   "(Sardinia): next flight 07 Jun 2027; BUD → Naples: next flight 01 Dec 2026."}));
}

TEST(Planner, CT_PLAN_04_Anywhere_lists_each_destination_group_once_per_origin)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 0, std::nullopt, 100, day(2026, 12, 1), day(2026, 12, 20)));

    // [A]
    Strings groups = group_keys(plan);
    std::ranges::sort(groups);
    EXPECT_EQ(std::ranges::adjacent_find(groups), groups.end());
    EXPECT_EQ(groups, (Strings{"BCN", "LON", "NAP", "SPQ", "TIA"}));
    // [D]
    EXPECT_EQ(describe(plan), (Strings{"BUD→BCN [BCN] 2026-12-01..2026-12-20", "BUD→LGW [LON] 2026-12-01..2026-12-20",
                                       "BUD→NAP [NAP] 2026-12-01..2026-12-20", "BUD→KRK [SPQ] 2026-12-01..2026-12-20",
                                       "BUD→TIA [TIA] 2026-12-01..2026-12-20"}));
    EXPECT_EQ(plan.routes_considered, 6);
    EXPECT_EQ(plan.routes_skipped_not_yet_flying, 1);
    EXPECT_EQ(plan.notes, (Strings{"1 route(s) skipped because their next flight is after your dates: BUD → Alghero "
                                   "(Sardinia): next flight 07 Jun 2027."}));
}

TEST(Planner, CT_PLAN_05_Past_range_and_missing_airports_produce_notes_not_requests)
{
    // Part 1 (also 03 §4.5 row 8).
    const SearchPlan past =
        plan_cat(make_request(match("Budapest"), 0, match("London"), 0, day(2026, 10, 1), day(2026, 10, 20)));
    EXPECT_TRUE(past.requests.empty());
    EXPECT_TRUE(any_contains(past.notes, "past"));
    EXPECT_EQ(past.notes, (Strings{"The date range is empty or entirely in the past.",
                                   "Dates before today were skipped (searching from 2026-11-02)."}));
    EXPECT_EQ(past.effective_from, day(2026, 11, 2));
    EXPECT_EQ(past.effective_to, day(2026, 10, 20));
    EXPECT_EQ(codes(past.origin_airports), (Strings{"BUD"}));
    EXPECT_EQ(codes(past.destination_airports), (Strings{"LGW", "LTN"}));
    EXPECT_EQ(past.routes_considered, 0);

    // Part 2.
    Place reykjavik = hand_place("Reykjavik", {64.1, -21.9});
    const SearchPlan none =
        plan_cat(make_request(reykjavik, 200, match("London"), 100, day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_TRUE(none.requests.empty());
    EXPECT_TRUE(any_contains(none.notes, "No Wizz Air airport"));
    EXPECT_EQ(none.notes, (Strings{"No Wizz Air airport within 200 km of Reykjavik. The nearest is London Luton (LTN), "
                                   "1842 km away."}));
    EXPECT_EQ(codes(none.destination_airports), (Strings{"LGW", "LTN"}));
    EXPECT_TRUE(none.origin_airports.empty());
    // 03 digest: Reykjavik–LTN 1841.80 km, above the 1500 km hint limit.
    EXPECT_EQ(round_places(wizz::distance_km(reykjavik.location, station("LTN")->location), 2), 1841.80);
}

TEST(Planner, CT_PLAN_06_Radius_zero_with_geocoded_place_finds_nothing_but_pinned_place_still_works)
{
    Place pest = hand_place("Pest", station("BUD")->location);
    const std::vector<AreaAirport> found = planner::airports_for(*cat(), pest, 1);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].airport, station("BUD"));
    EXPECT_EQ(found[0].distance_km, 0.0);

    pest.location = {0, 0};
    EXPECT_TRUE(planner::airports_for(*cat(), pest, 50).empty());
    // A geocoded place exactly on BUD with radius 0 still finds BUD; off it, nothing.
    EXPECT_EQ(codes(planner::airports_for(*cat(), hand_place("Pest", station("BUD")->location), 0)), (Strings{"BUD"}));
    EXPECT_TRUE(planner::airports_for(*cat(), hand_place("Pest", {47.5, 19.04}), 0).empty());
    // The pinned place still works at radius 0 (and at a negative radius, searched as 0).
    EXPECT_EQ(codes(planner::airports_for(*cat(), match("Budapest"), 0)), (Strings{"BUD"}));
    Place pinned = hand_place("Pest", {47.5, 19.04});
    pinned.pinned_airports = {"BUD"};
    EXPECT_EQ(codes(planner::airports_for(*cat(), pinned, -5)), (Strings{"BUD"}));
}

// ---------------------------------------------------------------------------
// 05 §3.10 Planner regressions
// ---------------------------------------------------------------------------

TEST(Planner, CT_PLANR_01_Group_sibling_outside_the_destination_radius_is_not_planned)
{
    const Place katowice = hand_place("Katowice", station("KTW")->location);
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 0, katowice, 30, day(2026, 11, 10), day(2026, 11, 20)));

    // [A]
    EXPECT_TRUE(plan.requests.empty());
    ASSERT_FALSE(plan.notes.empty());
    EXPECT_TRUE(plan.notes[0].starts_with(
        "Wizz Air does not fly from BUD to within 30 km of Katowice. Nearest served: Kraków (KRK)"));
    EXPECT_NE(plan.notes[0].find("at least 70 km"), std::string::npos);
    // [D]
    EXPECT_EQ(plan.notes[0], "Wizz Air does not fly from BUD to within 30 km of Katowice. Nearest served: Kraków (KRK), "
                             "67 km away — set the destination radius to at least 70 km.");
    EXPECT_EQ(km_labels(plan.destination_airports), (Strings{"KTW 0"}));
    EXPECT_EQ(plan.notes.size(), 1u);
    EXPECT_EQ(plan.routes_considered, 0);
}

TEST(Planner, CT_PLANR_02_Same_start_and_destination_is_refused_with_a_clear_note)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 500, match("BUD"), 100, day(2026, 11, 10), day(2026, 11, 20)));

    // [A]
    EXPECT_TRUE(plan.requests.empty());
    ASSERT_FALSE(plan.notes.empty());
    EXPECT_TRUE(plan.notes[0].starts_with("Start and destination are the same area"));
    // [D]
    EXPECT_EQ(plan.notes, (Strings{"Start and destination are the same area (0 km apart). Choose a destination farther "
                                   "away, or leave “To” empty to search anywhere."}));
    EXPECT_TRUE(plan.origin_airports.empty());
    EXPECT_TRUE(plan.destination_airports.empty());
    EXPECT_TRUE(plan.same_area);
}

TEST(Planner, CT_PLANR_03_No_airport_note_names_the_nearest_one)
{
    const Place graz = hand_place("Graz", {47.07, 15.44});
    const SearchPlan plan = plan_cat(make_request(graz, 100, match("London"), 100, day(2026, 11, 10), day(2026, 11, 20)));

    // [A]
    ASSERT_FALSE(plan.notes.empty());
    EXPECT_NE(plan.notes[0].find("The nearest is Bratislava (BTS)"), std::string::npos);
    EXPECT_NE(plan.notes[0].find("at least 190 km"), std::string::npos);
    // [D]
    EXPECT_EQ(plan.notes[0], "No Wizz Air airport within 100 km of Graz. The nearest is Bratislava (BTS), 181 km away. "
                             "Set the start radius to at least 190 km.");
    EXPECT_EQ(round_places(wizz::distance_km(graz.location, station("BTS")->location), 3), 180.556);
    EXPECT_EQ(plan.notes.size(), 1u);
    EXPECT_TRUE(plan.requests.empty());
}

/// The CT-PLANR-04 catalog (05 §3.10).
[[nodiscard]] AirportCatalog mirror_catalog()
{
    return hand_catalog({
        {.iata = "KUN", .location = {54.96, 24.08}, .mac = "LTE", .connections = undated({"AGP", "LTN"})},
        {.iata = "VNO", .location = {54.63, 25.29}, .mac = "LTE", .connections = undated({"AGP"})},
        {.iata = "AGP", .location = {36.67, -4.49}, .connections = undated({"VNO"})},
        {.iata = "LTN", .location = {51.87, -0.37}, .connections = undated({"KUN"})},
    });
}

TEST(Planner, CT_PLANR_04_Mirror_only_routes_are_not_queried_but_genuine_ones_are)
{
    const AirportCatalog catalog = mirror_catalog();
    const Place kaunas = hand_place("Kaunas", station(catalog, "KUN")->location);

    // Part 1: Kaunas r30 → anywhere.
    const SearchPlan anywhere =
        planner::plan(catalog, make_request(kaunas, 30, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)), kToday);
    EXPECT_EQ(query_tos(anywhere), (Strings{"LTN"}));
    EXPECT_TRUE(any_contains(anywhere.notes, "is flown from"));
    EXPECT_EQ(anywhere.notes, (Strings{"1 route(s) skipped because Wizz Air flies them from a sibling airport (searched "
                                       "separately if it is in range): KUN → AGP is flown from VNO (VNO), 86 km away."}));
    EXPECT_EQ(describe(anywhere), (Strings{"KUN→LTN [LTN] 2026-11-10..2026-11-20"}));
    EXPECT_EQ(anywhere.routes_considered, 1);
    // 03 digest: Kaunas–VNO 85.81 km.
    EXPECT_EQ(round_places(wizz::distance_km(kaunas.location, station(catalog, "VNO")->location), 2), 85.81);

    // Part 2: → Malaga r30.
    const Place malaga = hand_place("Malaga", station(catalog, "AGP")->location);
    const SearchPlan named =
        planner::plan(catalog, make_request(kaunas, 30, malaga, 30, day(2026, 11, 10), day(2026, 11, 20)), kToday);
    EXPECT_TRUE(named.requests.empty());
    ASSERT_FALSE(named.notes.empty());
    EXPECT_NE(named.notes[0].find("sibling airport outside your 30 km start radius"), std::string::npos);
    EXPECT_EQ(named.notes[0], "Wizz Air flies this from a nearby sibling airport outside your 30 km start radius: "
                              "KUN → AGP is flown from VNO (VNO), 86 km away. Increase the start radius.");
    // B7 clears the mirror list, so no I4 follows.
    EXPECT_EQ(named.notes.size(), 1u);
}

TEST(Planner, CT_PLANR_05_All_origins_inside_the_destination_area_gives_a_specific_note)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Kraków"), 10, match("Katowice"), 150, day(2026, 11, 10), day(2026, 11, 20)));

    // [A]
    ASSERT_FALSE(plan.notes.empty());
    EXPECT_TRUE(plan.notes[0].starts_with("All airports near Kraków (KRK) are also inside the destination area"));
    // [D]
    EXPECT_EQ(plan.notes[0], "All airports near Kraków (KRK) are also inside the destination area. Make the destination "
                             "radius smaller or choose a destination farther away.");
    EXPECT_EQ(codes(plan.destination_airports), (Strings{"KTW", "KRK"}));
    EXPECT_EQ(plan.destination_airports[0].distance_km, 0.0);
    EXPECT_EQ(round_places(plan.destination_airports[1].distance_km, 4), 67.2118);
    EXPECT_TRUE(plan.origin_airports.empty());
    // The NoAirportNote is suppressed (REG-5): B2 is the only note.
    EXPECT_EQ(plan.notes.size(), 1u);
    EXPECT_TRUE(plan.requests.empty());
}

TEST(Planner, CT_PLANR_06_Same_area_plan_is_flagged)
{
    LegSearchRequest request;  // default radii 200 / 100
    request.origin = match("Budapest");
    request.destination = match("BUD");
    request.from = day(2026, 11, 10);
    request.to = day(2026, 11, 20);
    const SearchPlan plan = plan_cat(request);

    EXPECT_TRUE(plan.same_area);
    EXPECT_EQ(plan.notes, (Strings{"Start and destination are the same area (0 km apart). Choose a destination farther "
                                   "away, or leave “To” empty to search anywhere."}));
}

TEST(Planner, CT_PLANR_07_Suspended_routes_with_1900_next_departure_are_skipped)
{
    const AirportCatalog catalog = hand_catalog({
        {.iata = "AAA",
         .location = {47, 19},
         .connections = {{"BBB", at(day(1900, 1, 1))}, {"CCC", at(day(2026, 11, 3))}}},
        {.iata = "BBB", .location = {48, 2}, .connections = undated({"AAA"})},
        {.iata = "CCC", .location = {52, 0}, .connections = undated({"AAA"})},
    });
    const Place a = hand_place("A", station(catalog, "AAA")->location);
    const SearchPlan plan =
        planner::plan(catalog, make_request(a, 10, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)), kToday);

    // [A]
    EXPECT_EQ(query_tos(plan), (Strings{"CCC"}));
    EXPECT_TRUE(any_contains(plan.notes, "no upcoming flights"));
    // [D]
    EXPECT_EQ(plan.notes, (Strings{"1 route(s) skipped: Wizz Air shows no upcoming flights on them."}));
    EXPECT_EQ(plan.routes_considered, 2);
    EXPECT_EQ(plan.routes_skipped_not_yet_flying, 1);
}

// ---------------------------------------------------------------------------
// CT-LATENT-02 (planning part): a duplicate BTS plans as one station.
// ---------------------------------------------------------------------------

TEST(Planner, CT_LATENT_02_Duplicate_station_plans_without_duplicate_origins_or_requests)
{
    const std::string map = wizz::test::read_fixture("map_subset.json");
    const std::size_t bts_at = map.find(R"({"iata":"BTS","mac":"VBR")");
    ASSERT_NE(bts_at, std::string::npos);
    const std::string duplicate =
        replace_once(json_object_at(map, bts_at), R"("shortName":"Bratislava")", R"js("shortName":"Bratislava (duplicate)")js");
    const std::string patched = replace_once(map, R"(],"javascript":null})", "," + duplicate + R"(],"javascript":null})");
    const AirportCatalogPtr catalog = AirportCatalog::from_map_json(patched, Locale::en_us());
    ASSERT_EQ(catalog->all().size(), 12u);

    const auto unique_keys = [](const SearchPlan& plan) {
        std::set<std::tuple<std::string, std::string, Date>> keys;
        for (const PlannedRequest& r : plan.requests) {
            keys.emplace(r.from->iata, r.group_key, r.start_date);
        }
        return keys.size() == plan.requests.size();
    };
    const auto unique_origins = [](const SearchPlan& plan) {
        Strings origins = codes(plan.origin_airports);
        std::ranges::sort(origins);
        return std::ranges::adjacent_find(origins) == origins.end();
    };

    // Budapest r200 → London r30: the CT-PLAN-01 plan, BTS once.
    const SearchPlan to_london = planner::plan(
        *catalog, make_request(match(*catalog, "Budapest"), 200, match(*catalog, "London"), 30, day(2026, 11, 10),
                               day(2026, 11, 30)),
        kToday);
    EXPECT_EQ(codes(to_london.origin_airports), (Strings{"BUD", "BTS", "DEB"}));
    EXPECT_TRUE(unique_origins(to_london));
    EXPECT_TRUE(unique_keys(to_london));
    EXPECT_EQ(describe(to_london), (Strings{"BUD→LGW [LON] 2026-11-10..2026-11-30", "BTS→LTN [LON] 2026-11-10..2026-11-30",
                                            "DEB→LTN [LON] 2026-11-10..2026-11-30"}));
    EXPECT_EQ(to_london.requests[1].from, catalog->find("BTS"));

    // Budapest r200 → anywhere (03 §4.5 row 3): every request once.
    const SearchPlan anywhere = planner::plan(
        *catalog, make_request(match(*catalog, "Budapest"), 200, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 30)),
        kToday);
    EXPECT_TRUE(unique_origins(anywhere));
    EXPECT_TRUE(unique_keys(anywhere));
    EXPECT_EQ(anywhere.requests.size(), 8u);

    // London r0 → anywhere: one LTN→BTS [VBR] request, named after the last value.
    const SearchPlan from_london = planner::plan(
        *catalog, make_request(match(*catalog, "London"), 0, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)),
        kToday);
    EXPECT_TRUE(unique_keys(from_london));
    EXPECT_EQ(std::ranges::count(group_keys(from_london), "VBR"), 1);
    EXPECT_EQ(planner::group_label(*catalog, "VBR"), "Bratislava (duplicate) (BTS)");
}

// ---------------------------------------------------------------------------
// 03 §4.5 fixture table (today 2026-11-02)
// ---------------------------------------------------------------------------

TEST(Planner, Example_4_5_row_2_Budapest_r200_to_London_r100_three_chunks_with_I1)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 200, match("London"), 100, day(2026, 10, 1), day(2027, 1, 31)));
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"BUD", "BTS", "DEB"}));
    EXPECT_EQ(codes(plan.destination_airports), (Strings{"LGW", "LTN"}));
    EXPECT_EQ(describe(plan), (Strings{
                                  "BUD→LGW [LON] 2026-11-02..2026-12-13",
                                  "BUD→LGW [LON] 2026-12-14..2027-01-24",
                                  "BUD→LGW [LON] 2027-01-25..2027-01-31",
                                  "BTS→LTN [LON] 2026-11-02..2026-12-13",
                                  "BTS→LTN [LON] 2026-12-14..2027-01-24",
                                  "BTS→LTN [LON] 2027-01-25..2027-01-31",
                                  "DEB→LTN [LON] 2026-11-02..2026-12-13",
                                  "DEB→LTN [LON] 2026-12-14..2027-01-24",
                                  "DEB→LTN [LON] 2027-01-25..2027-01-31",
                              }));
    EXPECT_EQ(plan.notes, (Strings{"Dates before today were skipped (searching from 2026-11-02)."}));
    EXPECT_EQ(plan.effective_from, kToday);
    EXPECT_EQ(plan.effective_to, day(2027, 1, 31));
}

TEST(Planner, Example_4_5_row_3_Budapest_r200_to_anywhere)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 200, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 30)));
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"BUD", "BTS", "DEB"}));
    EXPECT_TRUE(plan.destination_airports.empty());
    EXPECT_EQ(describe(plan), (Strings{
                                  "BUD→BCN [BCN] 2026-11-10..2026-11-30",
                                  "BUD→LGW [LON] 2026-11-10..2026-11-30",
                                  "BUD→KRK [SPQ] 2026-11-10..2026-11-30",
                                  "BUD→TIA [TIA] 2026-11-10..2026-11-30",
                                  "BTS→BCN [BCN] 2026-11-10..2026-11-30",
                                  "BTS→LTN [LON] 2026-11-10..2026-11-30",
                                  "BTS→TIA [TIA] 2026-11-10..2026-11-30",
                                  "DEB→LTN [LON] 2026-11-10..2026-11-30",
                              }));
    EXPECT_EQ(plan.routes_considered, 11);
    EXPECT_EQ(plan.routes_skipped_not_yet_flying, 3);
    EXPECT_EQ(plan.notes,
              (Strings{"3 route(s) skipped because their next flight is after your dates: BUD → Alghero (Sardinia): next "
                       "flight 07 Jun 2027; BUD → Naples: next flight 01 Dec 2026; BTS → Naples: next flight 03 Dec 2026."}));
}

TEST(Planner, Example_4_5_row_4_London_r0_to_anywhere)
{
    const SearchPlan plan =
        plan_cat(make_request(match("London"), 0, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"LGW", "LTN"}));
    EXPECT_EQ(round_places(plan.origin_airports[0].distance_km, 1), 40.6);
    EXPECT_EQ(round_places(plan.origin_airports[1].distance_km, 1), 44.1);
    EXPECT_EQ(describe(plan), (Strings{
                                  "LGW→BUD [BUD] 2026-11-10..2026-11-20",
                                  "LGW→KRK [SPQ] 2026-11-10..2026-11-20",
                                  "LGW→TIA [TIA] 2026-11-10..2026-11-20",
                                  "LTN→BCN [BCN] 2026-11-10..2026-11-20",
                                  "LTN→BUD [BUD] 2026-11-10..2026-11-20",
                                  "LTN→DEB [DEB] 2026-11-10..2026-11-20",
                                  "LTN→KRK [SPQ] 2026-11-10..2026-11-20",
                                  "LTN→TIA [TIA] 2026-11-10..2026-11-20",
                                  "LTN→BTS [VBR] 2026-11-10..2026-11-20",
                              }));
    EXPECT_TRUE(plan.notes.empty());
}

TEST(Planner, Example_4_5_row_5_Budapest_r0_to_Katowice_r100)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 0, match("Katowice"), 100, day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"BUD"}));
    EXPECT_EQ(km_labels(plan.destination_airports, 1), (Strings{"KTW 0.0", "KRK 67.2"}));
    EXPECT_EQ(describe(plan), (Strings{"BUD→KRK [SPQ] 2026-11-10..2026-11-20"}));
    EXPECT_TRUE(plan.notes.empty());
}

TEST(Planner, Example_4_5_row_6_Krakow_r100_to_Katowice_r50_no_direct_route_and_overlap)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Kraków"), 100, match("Katowice"), 50, day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"KRK"}));
    EXPECT_EQ(codes(plan.destination_airports), (Strings{"KTW"}));
    EXPECT_TRUE(plan.requests.empty());
    EXPECT_EQ(plan.notes, (Strings{"Wizz Air has no direct route between these areas. From KRK Wizz Air flies to: "
                                   "Barcelona El Prat, Budapest, London, Tirana.",
                                   "KTW lie(s) in both the start and the destination area and is used as a destination "
                                   "only."}));
}

TEST(Planner, Example_4_5_row_7_Budapest_r0_to_Bratislava_r0_no_direct_route)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 0, match("Bratislava"), 0, day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"BUD"}));
    EXPECT_EQ(codes(plan.destination_airports), (Strings{"BTS"}));
    EXPECT_TRUE(plan.requests.empty());
    EXPECT_EQ(plan.notes, (Strings{"Wizz Air has no direct route between these areas. From BUD Wizz Air flies to: "
                                   "Alghero (Sardinia), Barcelona El Prat, Kraków, London, Naples, Tirana."}));
}

// ---------------------------------------------------------------------------
// 03 §11 worked example, plan part (both legs)
// ---------------------------------------------------------------------------

TEST(Planner, Walkthrough_11_outbound_and_return_leg_plans)
{
    const SearchPlan outbound =
        plan_cat(make_request(match("Budapest"), 200, match("London"), 30, day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(km_labels(outbound.origin_airports, 3), (Strings{"BUD 0.000", "BTS 172.544", "DEB 184.077"}));
    EXPECT_EQ(km_labels(outbound.destination_airports, 3), (Strings{"LGW 40.558", "LTN 44.072"}));
    EXPECT_EQ(describe(outbound), (Strings{"BUD→LGW [LON] 2026-11-10..2026-11-20", "BTS→LTN [LON] 2026-11-10..2026-11-20",
                                           "DEB→LTN [LON] 2026-11-10..2026-11-20"}));
    EXPECT_EQ(outbound.routes_considered, 3);
    EXPECT_TRUE(outbound.notes.empty());
    // Step 3: the place centres are 1466 km apart.
    EXPECT_EQ(wizz::format_label(wizz::distance_km(match("Budapest").location, match("London").location), 0,
                                 Locale::en_us()->number_symbols()),
              "1466");
    // Step 4: BUD's connections in map order, all genuine.
    Strings bud_connections;
    for (const wizz::RouteConnection& connection : station("BUD")->connections) {
        bud_connections.push_back(connection.iata);
        EXPECT_TRUE(planner::lists_back(*cat(), *station(connection.iata), *station("BUD"))) << connection.iata;
    }
    EXPECT_EQ(bud_connections, (Strings{"AHO", "BCN", "KRK", "LGW", "LON", "LTN", "NAP", "TIA"}));

    LegSearchRequest back = make_request(match("London"), 30, match("Budapest"), 200, day(2026, 11, 12), day(2026, 11, 20));
    back.adults = 2;
    const SearchPlan inbound = plan_cat(back);
    EXPECT_EQ(km_labels(inbound.origin_airports, 1), (Strings{"LGW 40.6", "LTN 44.1"}));
    EXPECT_EQ(km_labels(inbound.destination_airports, 1), (Strings{"BUD 0.0", "BTS 172.5", "DEB 184.1"}));
    EXPECT_EQ(describe(inbound), (Strings{"LGW→BUD [BUD] 2026-11-12..2026-11-20", "LTN→BUD [BUD] 2026-11-12..2026-11-20",
                                          "LTN→BTS [VBR] 2026-11-12..2026-11-20", "LTN→DEB [DEB] 2026-11-12..2026-11-20"}));
    EXPECT_EQ(planner::group_label(*cat(), "VBR"), "Bratislava (BTS)");
    EXPECT_TRUE(inbound.notes.empty());
}

// ---------------------------------------------------------------------------
// 05 §7.1 P-18 .. P-22, P-30 (plan parts)
// ---------------------------------------------------------------------------

TEST(Planner, P_18_Outside_the_destination_radius_plan_and_note)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 0, match("London Luton"), 0, day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(describe(plan), (Strings{"BUD→LTN [LON] 2026-11-10..2026-11-20"}));
    EXPECT_EQ(plan.notes, (Strings{"Served but outside your 0 km destination radius: London Gatwick (LGW) 82 km."}));
    EXPECT_EQ(codes(plan.destination_airports), (Strings{"LTN"}));
    EXPECT_EQ(plan.routes_considered, 1);
}

TEST(Planner, P_19_No_direct_route_note)
{
    const SearchPlan plan = plan_cat(make_request(match("Debrecen"), 0, hand_place("Barcelona", station("BCN")->location),
                                                  50, day(2026, 11, 10), day(2026, 11, 20)));
    ASSERT_FALSE(plan.notes.empty());
    EXPECT_EQ(plan.notes[0], "Wizz Air has no direct route between these areas. From DEB Wizz Air flies to: London.");
    EXPECT_EQ(plan.notes.size(), 1u);
}

TEST(Planner, P_20_Overlap_info_note)
{
    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 200, match("Bratislava"), 10, day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"BUD", "DEB"}));
    EXPECT_EQ(codes(plan.destination_airports), (Strings{"BTS"}));
    EXPECT_EQ(plan.notes, (Strings{"Wizz Air has no direct route between these areas. From BUD, DEB Wizz Air flies to: "
                                   "Alghero (Sardinia), Barcelona El Prat, Kraków, London, Naples, Tirana.",
                                   "BTS lie(s) in both the start and the destination area and is used as a destination "
                                   "only."}));
}

TEST(Planner, P_21_Truncating_notes_end_with_an_ellipsis_entry)
{
    const Date late = day(2027, 2, 1);
    // ORG lists N1..N5 (not yet flying), M1..M4 (mirror: they do not list ORG back).
    std::vector<Station> stations{{.iata = "ORG",
                                   .location = {10, 10},
                                   .connections = {{"N1", at(late)},
                                                   {"N2", at(late.add_days(1))},
                                                   {"N3", at(late.add_days(2))},
                                                   {"N4", at(late.add_days(3))},
                                                   {"N5", at(late.add_days(4))},
                                                   {"M1", std::nullopt},
                                                   {"M2", std::nullopt},
                                                   {"M3", std::nullopt},
                                                   {"M4", std::nullopt}}}};
    for (const char* code : {"N1", "N2", "N3", "N4", "N5"}) {
        stations.push_back({.iata = code, .location = {20, 20}, .connections = undated({"ORG"})});
    }
    for (const char* code : {"M1", "M2", "M3", "M4"}) {
        stations.push_back({.iata = code, .location = {30, 30}, .connections = undated({"XXX"})});
    }
    const AirportCatalog catalog = hand_catalog(stations);
    const SearchPlan plan = planner::plan(
        catalog,
        make_request(hand_place("Origin", {10, 10}), 0, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)), kToday);

    EXPECT_TRUE(plan.requests.empty());
    EXPECT_EQ(plan.notes,
              (Strings{"4 route(s) skipped because Wizz Air flies them from a sibling airport (searched separately if it "
                       "is in range): ORG → M1; ORG → M2; ORG → M3; ….",
                       "5 route(s) skipped because their next flight is after your dates: ORG → N1: next flight 01 Feb "
                       "2027; ORG → N2: next flight 02 Feb 2027; ORG → N3: next flight 03 Feb 2027; …."}));
    EXPECT_EQ(plan.routes_considered, 5);
    EXPECT_EQ(plan.routes_skipped_not_yet_flying, 5);
    for (const std::string& note : plan.notes) {
        EXPECT_TRUE(note.ends_with("; …."));
    }

    // Exactly three entries: no ellipsis.
    std::vector<Station> three{{.iata = "ORG",
                                .location = {10, 10},
                                .connections = {{"N1", at(late)}, {"N2", at(late)}, {"N3", at(late)}, {"M1", std::nullopt}}}};
    for (const char* code : {"N1", "N2", "N3"}) {
        three.push_back({.iata = code, .location = {20, 20}, .connections = undated({"ORG"})});
    }
    three.push_back({.iata = "M1", .location = {30, 30}});
    const AirportCatalog small = hand_catalog(three);
    const SearchPlan short_plan = planner::plan(
        small, make_request(hand_place("Origin", {10, 10}), 0, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)),
        kToday);
    EXPECT_EQ(short_plan.notes,
              (Strings{"1 route(s) skipped because Wizz Air flies them from a sibling airport (searched separately if it "
                       "is in range): ORG → M1.",
                       "3 route(s) skipped because their next flight is after your dates: ORG → N1: next flight 01 Feb "
                       "2027; ORG → N2: next flight 01 Feb 2027; ORG → N3: next flight 01 Feb 2027."}));
}

TEST(Planner, P_22_Group_names_and_labels)
{
    const AirportCatalog& catalog = *cat();
    EXPECT_EQ(planner::group_name(catalog, "LON"), "London");
    EXPECT_EQ(planner::group_name(catalog, "SPQ"), "Katowice / Kraków");
    EXPECT_EQ(planner::group_name(catalog, "VBR"), "Bratislava");
    EXPECT_EQ(planner::group_name(catalog, "BUD"), "Budapest");
    EXPECT_EQ(planner::group_name(catalog, "XXX"), "XXX");
    // 03 §5.9 GroupLabel.
    EXPECT_EQ(planner::group_label(catalog, "VBR"), "Bratislava (BTS)");
    EXPECT_EQ(planner::group_label(catalog, "SPQ"), "Katowice / Kraków");
    EXPECT_EQ(planner::group_label(catalog, "LON"), "London (LON)");
    EXPECT_EQ(planner::group_label(catalog, "BCN"), "Barcelona El Prat (BCN)");
    EXPECT_EQ(planner::group_label(catalog, "BUD"), "Budapest (BUD)");
    EXPECT_EQ(planner::group_label(catalog, "TIA"), "Tirana (TIA)");
    EXPECT_EQ(planner::group_label(catalog, "XXX"), "XXX");
    // Codes are looked up ignoring case.
    EXPECT_EQ(planner::group_name(catalog, "lon"), "London");
    EXPECT_EQ(planner::group_label(catalog, "vbr"), "Bratislava (BTS)");
}

TEST(Planner, P_30_Bud_nap_fixture_window_is_planned_from_its_first_departure)
{
    // The unused fixture's first row departs exactly at the map's operationStartDate (05 §2.2).
    const wizz::json::Value timetable = wizz::json::parse(wizz::test::read_fixture("timetable_v2_bud_nap.json"));
    const wizz::json::Value* flights = timetable.find("outboundFlights");
    ASSERT_NE(flights, nullptr);
    ASSERT_EQ(flights->as_array().size(), 10u);
    const std::optional<std::string_view> first = flights->at(0).get_string("departureDate");
    ASSERT_TRUE(first.has_value());
    const std::optional<LocalDateTime> first_departure = wizz::parse_local_datetime(*first);
    ASSERT_TRUE(first_departure.has_value());
    EXPECT_EQ(*first_departure, at(day(2026, 12, 1), 21, 5));
    const auto nap = std::ranges::find(station("BUD")->connections, std::string{"NAP"}, &wizz::RouteConnection::iata);
    ASSERT_NE(nap, station("BUD")->connections.end());
    EXPECT_EQ(nap->next_departure, first_departure);

    const SearchPlan plan =
        plan_cat(make_request(match("Budapest"), 0, std::nullopt, 100, day(2026, 11, 25), day(2026, 12, 10)));
    EXPECT_EQ(describe(plan), (Strings{
                                  "BUD→BCN [BCN] 2026-11-25..2026-12-10",
                                  "BUD→LGW [LON] 2026-11-25..2026-12-10",
                                  "BUD→NAP [NAP] 2026-11-25..2026-12-10",
                                  "BUD→KRK [SPQ] 2026-11-25..2026-12-10",
                                  "BUD→TIA [TIA] 2026-11-25..2026-12-10",
                              }));
    EXPECT_EQ(plan.notes, (Strings{"1 route(s) skipped because their next flight is after your dates: BUD → Alghero "
                                   "(Sardinia): next flight 07 Jun 2027."}));
}

// ---------------------------------------------------------------------------
// 03 §4.3.x rules and examples
// ---------------------------------------------------------------------------

TEST(Planner, Rule_4_3_1_Today_itself_is_not_skipped_and_to_before_from_is_an_empty_range)
{
    const SearchPlan today_only =
        plan_cat(make_request(match("Budapest"), 0, match("London"), 0, kToday, kToday));
    EXPECT_TRUE(today_only.notes.empty());
    EXPECT_EQ(describe(today_only), (Strings{"BUD→LGW [LON] 2026-11-02..2026-11-02"}));

    // To < From with future dates: same B5 text, no I1.
    const SearchPlan reversed =
        plan_cat(make_request(match("Budapest"), 0, match("London"), 0, day(2026, 12, 10), day(2026, 12, 1)));
    EXPECT_EQ(reversed.notes, (Strings{"The date range is empty or entirely in the past."}));
    EXPECT_TRUE(reversed.requests.empty());
    EXPECT_EQ(reversed.effective_from, day(2026, 12, 10));
    EXPECT_EQ(reversed.effective_to, day(2026, 12, 1));
}

TEST(Planner, Rule_4_3_2_Pins_expand_groups_keep_siblings_out_and_sort_stably)
{
    const AirportCatalog& catalog = *cat();
    // London r30: LGW 40.6, LTN 44.1 although both are outside 30 km.
    EXPECT_EQ(km_labels(planner::airports_for(catalog, match("London"), 30), 1), (Strings{"LGW 40.6", "LTN 44.1"}));
    // A pinned real code is just itself: KRK's sibling KTW is not added.
    Place krakow = hand_place("Kraków", station("KRK")->location);
    krakow.pinned_airports = {"KRK"};
    EXPECT_EQ(codes(planner::airports_for(catalog, krakow, 0)), (Strings{"KRK"}));
    // A pinned pseudo code expands to its members; unknown codes add nothing; nothing twice.
    Place pins = hand_place("Somewhere", {0, 0});
    pins.pinned_airports = {"ZZZ", "LON", "LTN", "BUD", "lon"};
    EXPECT_EQ(codes(planner::airports_for(catalog, pins, 10)), (Strings{"BUD", "LGW", "LTN"}));
    // GroupMembersOrSelf.
    const auto member_codes = [](const std::vector<AirportPtr>& airports) {
        Strings result;
        for (const AirportPtr& a : airports) {
            result.push_back(a->iata);
        }
        return result;
    };
    EXPECT_EQ(member_codes(planner::group_members_or_self(catalog, "LON")), (Strings{"LGW", "LTN"}));
    EXPECT_EQ(member_codes(planner::group_members_or_self(catalog, "KRK")), (Strings{"KRK"}));
    EXPECT_EQ(member_codes(planner::group_members_or_self(catalog, " ltn ")), (Strings{"LTN"}));
    EXPECT_TRUE(planner::group_members_or_self(catalog, "SPQ").empty());
    EXPECT_TRUE(planner::group_members_or_self(catalog, "XXX").empty());

    // Ties keep Near's (name) order, then pin order.
    const AirportCatalog tied = hand_catalog({
        {.iata = "BBB", .location = {1, 0}, .name = "Beta"},
        {.iata = "AAA", .location = {1, 0}, .name = "Alpha"},
        {.iata = "CCC", .location = {0, 1}, .name = "Gamma"},
        {.iata = "DDD", .location = {0, 1}, .name = "Delta"},
    });
    Place centre = hand_place("Centre", {0, 0});
    centre.pinned_airports = {"CCC", "DDD"};
    EXPECT_EQ(codes(planner::airports_for(tied, centre, 200)), (Strings{"AAA", "BBB", "DDD", "CCC"}));
    EXPECT_EQ(codes(planner::airports_for(tied, centre, 100)), (Strings{"CCC", "DDD"}));
    // NaN radius finds nothing near (Math.Max(0, NaN) is NaN) but still adds pins.
    EXPECT_EQ(codes(planner::airports_for(tied, centre, std::numeric_limits<double>::quiet_NaN())),
              (Strings{"CCC", "DDD"}));
}

TEST(Planner, Rule_4_3_3_Same_area_threshold_is_strictly_below_40_km)
{
    const AirportCatalog catalog = hand_catalog({
        {.iata = "AAA", .location = {0, 0}, .connections = undated({"BBB"})},
        {.iata = "BBB", .location = {10, 10}, .connections = undated({"AAA"})},
    });
    // 0.36 degrees of latitude ≈ 40.03 km; 0.359 ≈ 39.92 km.
    const Place a = hand_place("A", {0, 0});
    const Place far = hand_place("Far", {0.36, 0});
    const Place close = hand_place("Close", {0.359, 0});
    ASSERT_GE(wizz::distance_km(a.location, far.location), 40.0);
    ASSERT_LT(wizz::distance_km(a.location, close.location), 40.0);

    const SearchPlan same =
        planner::plan(catalog, make_request(a, 10, close, 10, day(2026, 11, 10), day(2026, 11, 20)), kToday);
    EXPECT_TRUE(same.same_area);
    EXPECT_EQ(same.notes, (Strings{"Start and destination are the same area (40 km apart). Choose a destination farther "
                                   "away, or leave “To” empty to search anywhere."}));
    const SearchPlan apart =
        planner::plan(catalog, make_request(a, 10, far, 10, day(2026, 11, 10), day(2026, 11, 20)), kToday);
    EXPECT_FALSE(apart.same_area);
    // The far place has no airport: B4 (destination role), no B1.
    EXPECT_EQ(apart.notes, (Strings{"No Wizz Air airport within 10 km of Far. The nearest is AAA (AAA), 40 km away. Set "
                                    "the destination radius to at least 50 km."}));
}

TEST(Planner, Rule_4_3_5_No_airport_note_hint_rounding_and_limits)
{
    const AirportCatalog& catalog = *cat();
    // Exactly 70.0 km → 70 (ceil of an exact multiple stays).
    const AirportCatalog one = hand_catalog({{.iata = "AAA", .location = {0, 0}}});
    const double degrees_for_70 = 70.0 / (wizz::kEarthRadiusKm * std::numbers::pi / 180.0);
    Place p = hand_place("P", {degrees_for_70, 0});
    const double km = wizz::distance_km(p.location, {0, 0});
    const std::string expected_hint = std::ceil(km / 10) * 10 == 70.0 ? "70" : "80";
    EXPECT_EQ(planner::no_airport_note(one, p, 10, planner::AreaRole::start),
              "No Wizz Air airport within 10 km of P. The nearest is AAA (AAA), 70 km away. Set the start radius to at "
              "least " +
                  expected_hint + " km.");
    // 120.3 → 130; 181.4 → 190 (03 §4.3.5 examples of the ceiling).
    EXPECT_EQ(std::ceil(120.3 / 10) * 10, 130.0);
    EXPECT_EQ(std::ceil(181.4 / 10) * 10, 190.0);

    // The radius is printed raw (negative values too); the destination role.
    EXPECT_EQ(planner::no_airport_note(catalog, hand_place("Graz", {47.07, 15.44}), -5, planner::AreaRole::destination),
              "No Wizz Air airport within -5 km of Graz. The nearest is Bratislava (BTS), 181 km away. Set the "
              "destination radius to at least 190 km.");
    EXPECT_EQ(planner::no_airport_note(catalog, hand_place("Graz", {47.07, 15.44}), 99.5, planner::AreaRole::start),
              "No Wizz Air airport within 100 km of Graz. The nearest is Bratislava (BTS), 181 km away. Set the start "
              "radius to at least 190 km.");

    // No real station at all.
    const AirportCatalog pseudo_only = hand_catalog(
        {{.iata = "LON", .location = {51.5, 0}, .mac = "LON", .name = "London (All Airports)", .fake = true}});
    EXPECT_EQ(planner::no_airport_note(pseudo_only, hand_place("Nowhere", {0, 0}), 100, planner::AreaRole::start),
              "No Wizz Air airport near the start (Nowhere).");
    EXPECT_EQ(planner::no_airport_note(pseudo_only, hand_place("Nowhere", {0, 0}), 100, planner::AreaRole::destination),
              "No Wizz Air airport near the destination (Nowhere).");

    // The hint stops above 1500 km.
    const AirportCatalog distant = hand_catalog({{.iata = "AAA", .location = {0, 0}}});
    const double degrees_for_1500 = 1500.0 / (wizz::kEarthRadiusKm * std::numbers::pi / 180.0);
    const Place at_limit = hand_place("Edge", {degrees_for_1500 * (1 - 1e-12), 0});
    const Place beyond = hand_place("Beyond", {degrees_for_1500 * 1.002, 0});
    EXPECT_NE(planner::no_airport_note(distant, at_limit, 0, planner::AreaRole::start).find("Set the start radius to at "
                                                                                            "least 1500 km."),
              std::string::npos);
    EXPECT_EQ(planner::no_airport_note(distant, beyond, 0, planner::AreaRole::start),
              "No Wizz Air airport within 0 km of Beyond. The nearest is AAA (AAA), 1503 km away.");

    // Origins found but no destinations: B4 only (at most one NoAirportNote).
    const SearchPlan plan = plan_cat(make_request(match("Budapest"), 0, hand_place("Graz", {47.07, 15.44}), 100,
                                                  day(2026, 11, 10), day(2026, 11, 20)));
    EXPECT_EQ(plan.notes, (Strings{"No Wizz Air airport within 100 km of Graz. The nearest is Bratislava (BTS), 181 km "
                                   "away. Set the destination radius to at least 190 km."}));
    // Neither side: only the start note; B3 and B5 can both appear.
    const SearchPlan both = plan_cat(make_request(hand_place("Graz", {47.07, 15.44}), 100,
                                                  hand_place("Graz", {47.07, 15.44 + 1}), 10, day(2026, 10, 1),
                                                  day(2026, 10, 2)));
    EXPECT_EQ(both.notes, (Strings{"No Wizz Air airport within 100 km of Graz. The nearest is Bratislava (BTS), 181 km "
                                   "away. Set the start radius to at least 190 km.",
                                   "The date range is empty or entirely in the past.",
                                   "Dates before today were skipped (searching from 2026-11-02)."}));
}

TEST(Planner, Rule_4_3_7_Lists_back_reciprocity)
{
    // LTN (city group LON with a pseudo station) and KTW/KRK (regional group SPQ without one).
    const AirportCatalog catalog = hand_catalog({
        {.iata = "LON", .location = {51.5, -0.1}, .mac = "LON", .connections = undated({"TCC"}), .name = "London (All Airports)", .fake = true},
        {.iata = "LTN", .location = {51.87, -0.37}, .mac = "LON", .connections = undated({"TCC", "TLT"})},
        {.iata = "LGW", .location = {51.15, -0.19}, .mac = "LON", .connections = undated({"TPG"})},
        {.iata = "KTW", .location = {50.48, 19.08}, .mac = "SPQ", .connections = undated({"TSP"})},
        {.iata = "TCC", .location = {41, 19}, .connections = undated({"LON"})},       // lists the city code
        {.iata = "TLT", .location = {42, 19}, .connections = undated({"LTN"})},       // lists LTN itself
        {.iata = "TSP", .location = {43, 19}, .connections = undated({"SPQ"})},       // lists a regional code
        {.iata = "TPG", .location = {44, 19}, .connections = undated({"XXX"})},       // lists nothing relevant
        {.iata = "PAR", .location = {48.8, 2.3}, .mac = "PAR", .connections = undated({"KTW"}), .name = "Paris (All Airports)", .fake = true},
        {.iata = "BVA", .location = {49.45, 2.11}, .mac = "PAR", .connections = undated({"XXX"})},
        {.iata = "ORY", .location = {48.72, 2.38}, .mac = "PAR", .connections = undated({"LTN"})},
    });
    const auto lb = [&](std::string_view target, std::string_view origin) {
        return planner::lists_back(catalog, *station(catalog, target), *station(catalog, origin));
    };
    EXPECT_TRUE(lb("TCC", "LTN"));   // the target lists the origin's city pseudo-code
    EXPECT_TRUE(lb("TLT", "LTN"));   // the target lists the origin
    EXPECT_FALSE(lb("TSP", "KTW"));  // a regional group code does not count
    EXPECT_FALSE(lb("TPG", "LGW"));
    EXPECT_TRUE(lb("PAR", "LTN"));   // pseudo target: its member ORY lists LTN
    EXPECT_FALSE(lb("PAR", "LGW"));  // no member lists LGW or LON
    EXPECT_TRUE(lb("BVA", "KTW"));   // the target's city pseudo station PAR lists KTW
    EXPECT_FALSE(lb("BVA", "LGW"));
    // Case-insensitive code compare.
    const AirportCatalog lower = hand_catalog({
        {.iata = "AAA", .location = {0, 0}, .connections = undated({"bbb"})},
        {.iata = "BBB", .location = {1, 1}, .connections = undated({"aaa"})},
    });
    EXPECT_TRUE(planner::lists_back(lower, *station(lower, "BBB"), *station(lower, "AAA")));
}

TEST(Planner, Rule_4_3_7_Mirror_check_runs_before_the_home_area_filter_and_irrelevant_mirrors_are_dropped)
{
    // HOM and SIB are both home; HOM → SIB is not listed back, so it is a mirror entry
    // even though the home-area exclusion would also have skipped it.
    const AirportCatalog catalog = hand_catalog({
        {.iata = "HOM", .location = {0, 0}, .connections = undated({"SIB", "DST", "OTH"})},
        {.iata = "SIB", .location = {0, 0.1}, .connections = undated({"XXX"})},
        {.iata = "DST", .location = {10, 10}, .connections = undated({"HOM"})},
        {.iata = "OTH", .location = {20, 20}, .connections = undated({"XXX"})},
    });
    const SearchPlan anywhere = planner::plan(
        catalog, make_request(hand_place("Home", {0, 0}), 50, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)),
        kToday);
    EXPECT_EQ(codes(anywhere.origin_airports), (Strings{"HOM", "SIB"}));
    EXPECT_EQ(describe(anywhere), (Strings{"HOM→DST [DST] 2026-11-10..2026-11-20"}));
    EXPECT_EQ(anywhere.notes, (Strings{"2 route(s) skipped because Wizz Air flies them from a sibling airport (searched "
                                       "separately if it is in range): HOM → SIB; HOM → OTH."}));

    // With a destination, the mirror route to a non-destination group (OTH) is dropped silently.
    const SearchPlan named = planner::plan(catalog,
                                           make_request(hand_place("Home", {0, 0}), 0, hand_place("Dest", {10, 10}), 10,
                                                        day(2026, 11, 10), day(2026, 11, 20)),
                                           kToday);
    EXPECT_EQ(describe(named), (Strings{"HOM→DST [DST] 2026-11-10..2026-11-20"}));
    EXPECT_TRUE(named.notes.empty());
}

TEST(Planner, Rule_4_3_8_Pseudo_target_counts_only_without_separately_listed_members)
{
    // ORG lists only the pseudo code LON: LON is in radius and becomes the QueryTo.
    const AirportCatalog catalog = hand_catalog({
        {.iata = "ORG", .location = {47, 19}, .connections = undated({"LON"})},
        {.iata = "LON", .location = {51.51, -0.118}, .mac = "LON", .connections = undated({"ORG"}), .name = "London (All Airports)", .fake = true},
        {.iata = "LTN", .location = {51.87, -0.37}, .mac = "LON", .connections = undated({"ORG"}), .name = "London Luton"},
        {.iata = "LGW", .location = {51.15, -0.19}, .mac = "LON", .connections = undated({"ORG"}), .name = "London Gatwick"},
    });
    Place london = hand_place("London", {51.51, -0.118});
    london.pinned_airports = {"LTN"};
    const SearchPlan plan = planner::plan(
        catalog, make_request(hand_place("Origin", {47, 19}), 0, london, 0, day(2026, 11, 10), day(2026, 11, 20)), kToday);
    EXPECT_EQ(describe(plan), (Strings{"ORG→LON [LON] 2026-11-10..2026-11-20"}));
    EXPECT_TRUE(plan.notes.empty());  // a pseudo station never goes into outsideRadius
}

TEST(Planner, Rule_4_3_8_Outside_radius_keeps_first_insertion_and_lists_three_nearest)
{
    // Two origins see the same out-of-radius targets; I3 lists the three nearest (stable).
    const AirportCatalog catalog = hand_catalog({
        {.iata = "OA", .location = {0, 0}, .connections = undated({"D4", "D3", "D2", "D1", "DIN"})},
        {.iata = "OB", .location = {0, 0.01}, .connections = undated({"D1", "DIN"})},
        {.iata = "DIN", .location = {20, 20}, .mac = "GRP", .connections = undated({"OA", "OB"}), .name = "Inside"},
        {.iata = "D1", .location = {20.5, 20}, .mac = "GRP", .connections = undated({"OA", "OB"}), .name = "One"},
        {.iata = "D2", .location = {21, 20}, .mac = "GRP", .connections = undated({"OA"}), .name = "Two"},
        {.iata = "D3", .location = {21.5, 20}, .mac = "GRP", .connections = undated({"OA"}), .name = "Three"},
        {.iata = "D4", .location = {22, 20}, .mac = "GRP", .connections = undated({"OA"}), .name = "Four"},
    });
    const SearchPlan plan = planner::plan(catalog,
                                          make_request(hand_place("Home", {0, 0}), 5, hand_place("Dest", {20, 20}), 10,
                                                       day(2026, 11, 10), day(2026, 11, 20)),
                                          kToday);
    EXPECT_EQ(describe(plan), (Strings{"OA→DIN [GRP] 2026-11-10..2026-11-20", "OB→DIN [GRP] 2026-11-10..2026-11-20"}));
    EXPECT_EQ(plan.notes, (Strings{"Served but outside your 10 km destination radius: One (D1) 56 km, Two (D2) 111 km, "
                                   "Three (D3) 167 km."}));

    // routes = 0: B6 picks the nearest; ties go to the earliest inserted.
    const AirportCatalog tie = hand_catalog({
        {.iata = "ORG", .location = {0, 0}, .connections = undated({"TB", "TA"})},
        {.iata = "DIN", .location = {20, 20}, .mac = "GRP", .name = "Inside"},
        {.iata = "TA", .location = {20, 20.5}, .mac = "GRP", .connections = undated({"ORG"}), .name = "Alpha"},
        {.iata = "TB", .location = {20, 19.5}, .mac = "GRP", .connections = undated({"ORG"}), .name = "Beta"},
    });
    const SearchPlan b6 = planner::plan(tie,
                                        make_request(hand_place("Home", {0, 0}), 0, hand_place("Dest", {20, 20}), 0,
                                                     day(2026, 11, 10), day(2026, 11, 20)),
                                        kToday);
    ASSERT_EQ(b6.notes.size(), 1u);
    EXPECT_EQ(b6.notes[0], "Wizz Air does not fly from ORG to within 0 km of Dest. Nearest served: Beta (TB), 52 km away "
                           "— set the destination radius to at least 60 km.");
}

TEST(Planner, Rule_4_3_9_Anywhere_skips_pseudo_targets_whose_members_are_all_home)
{
    const AirportCatalog catalog = hand_catalog({
        {.iata = "HA", .location = {0, 0}, .mac = "HOM", .connections = undated({"HOM", "MIX", "EMP", "HB"})},
        {.iata = "HB", .location = {0, 0.1}, .mac = "HOM", .connections = undated({"HA"})},
        {.iata = "HOM", .location = {0, 0.05}, .mac = "HOM", .connections = undated({"HA"}), .name = "Home (All Airports)", .fake = true},
        {.iata = "MIX", .location = {5, 5}, .mac = "MIX", .connections = undated({"HA"}), .name = "Mix (All Airports)", .fake = true},
        {.iata = "MX1", .location = {0, 0.2}, .mac = "MIX", .connections = undated({"HA"})},  // at home
        {.iata = "MX2", .location = {5, 5}, .mac = "MIX", .connections = undated({"HA"})},    // away
        {.iata = "EMP", .location = {9, 9}, .mac = "EMP", .connections = undated({"HA"}), .name = "Empty (All Airports)", .fake = true},
    });
    const SearchPlan plan = planner::plan(
        catalog, make_request(hand_place("Home", {0, 0}), 30, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)),
        kToday);
    EXPECT_EQ(codes(plan.origin_airports), (Strings{"HA", "HB", "MX1"}));
    // HOM (all members home), EMP (no members: vacuously all home) and HB (home) are skipped;
    // MIX (only some members home) is kept as a pseudo QueryTo.
    EXPECT_EQ(describe(plan), (Strings{"HA→MIX [MIX] 2026-11-10..2026-11-20"}));
    EXPECT_TRUE(plan.notes.empty());
    EXPECT_EQ(plan.routes_considered, 1);
}

TEST(Planner, Rule_4_3_10_QueryTo_prefers_the_first_real_in_area_airport)
{
    // ORG lists LON (pseudo) first, then LGW, then LTN: QueryTo is LGW, never LON.
    const AirportCatalog catalog = hand_catalog({
        {.iata = "ORG", .location = {47, 19}, .connections = undated({"LON", "LGW", "LTN"})},
        {.iata = "LON", .location = {51.51, -0.118}, .mac = "LON", .connections = undated({"ORG"}), .name = "London (All Airports)", .fake = true},
        {.iata = "LTN", .location = {51.87, -0.37}, .mac = "LON", .connections = undated({"ORG"}), .name = "London Luton"},
        {.iata = "LGW", .location = {51.15, -0.19}, .mac = "LON", .connections = undated({"ORG"}), .name = "London Gatwick"},
    });
    const SearchPlan anywhere = planner::plan(
        catalog, make_request(hand_place("Origin", {47, 19}), 0, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)),
        kToday);
    EXPECT_EQ(describe(anywhere), (Strings{"ORG→LGW [LON] 2026-11-10..2026-11-20"}));
    EXPECT_EQ(anywhere.routes_considered, 1);
}

TEST(Planner, Rule_4_3_10_Next_departure_tri_state)
{
    using planner::merge_next_departures;
    using planner::normalize_next_departure;
    const NextDeparture jan5 = NextDeparture::at(at(day(2027, 1, 5), 9, 30));
    const NextDeparture mar1 = NextDeparture::at(at(day(2027, 3, 1)));
    const NextDeparture old = normalize_next_departure(at(day(1900, 1, 1)));

    EXPECT_TRUE(normalize_next_departure(std::nullopt).is_unknown());
    EXPECT_TRUE(old.is_suspended());
    EXPECT_TRUE(normalize_next_departure(at(day(1999, 12, 31), 23, 59)).is_suspended());
    EXPECT_EQ(normalize_next_departure(at(day(2000, 1, 1))), NextDeparture::at(at(day(2000, 1, 1))));
    EXPECT_EQ(normalize_next_departure(at(day(2027, 1, 5), 9, 30)), jan5);
    EXPECT_EQ(jan5.date(), day(2027, 1, 5));
    EXPECT_EQ(old.date(), std::nullopt);
    EXPECT_EQ(old.time(), std::nullopt);
    EXPECT_EQ(NextDeparture{}.kind(), NextDeparture::Kind::unknown);

    // {1900, 2027-01-05} → 2027-01-05; {UNKNOWN, 2027-03-01} → UNKNOWN; {1900, 1900} → SUSPENDED.
    EXPECT_EQ(merge_next_departures(old, jan5), jan5);
    EXPECT_EQ(merge_next_departures(jan5, old), jan5);
    EXPECT_EQ(merge_next_departures(NextDeparture::unknown(), mar1), NextDeparture::unknown());
    EXPECT_EQ(merge_next_departures(mar1, NextDeparture::unknown()), NextDeparture::unknown());
    EXPECT_EQ(merge_next_departures(old, old), NextDeparture::suspended());
    EXPECT_EQ(merge_next_departures(old, NextDeparture::unknown()), NextDeparture::unknown());
    EXPECT_EQ(merge_next_departures(jan5, mar1), jan5);
    EXPECT_EQ(merge_next_departures(mar1, jan5), jan5);

    // The same three cases through Plan: one regional group G (G1, G2) reached twice.
    const auto plan_with = [](std::optional<LocalDateTime> first, std::optional<LocalDateTime> second) {
        const AirportCatalog catalog = hand_catalog({
            {.iata = "ORG", .location = {0, 0}, .connections = {{"G1", first}, {"G2", second}}},
            {.iata = "G1", .location = {10, 10}, .mac = "GGG", .connections = undated({"ORG"}), .name = "Gee One"},
            {.iata = "G2", .location = {10, 10.1}, .mac = "GGG", .connections = undated({"ORG"}), .name = "Gee Two"},
        });
        return planner::plan(
            catalog, make_request(hand_place("Origin", {0, 0}), 0, std::nullopt, 100, day(2026, 11, 10), day(2027, 1, 31)),
            kToday);
    };
    const SearchPlan dated = plan_with(at(day(1900, 1, 1)), at(day(2027, 1, 5), 9, 30));
    EXPECT_EQ(describe(dated), (Strings{"ORG→G1 [GGG] 2026-12-22..2027-01-31"}));
    const SearchPlan unknown = plan_with(std::nullopt, at(day(2027, 3, 1)));
    EXPECT_EQ(describe(unknown), (Strings{"ORG→G1 [GGG] 2026-11-10..2026-12-21", "ORG→G1 [GGG] 2026-12-22..2027-01-31"}));
    EXPECT_TRUE(unknown.notes.empty());
    const SearchPlan suspended = plan_with(at(day(1900, 1, 1)), at(day(1900, 1, 1)));
    EXPECT_TRUE(suspended.requests.empty());
    EXPECT_EQ(suspended.notes, (Strings{"1 route(s) skipped: Wizz Air shows no upcoming flights on them."}));
    const SearchPlan later = plan_with(at(day(2027, 3, 1)), std::optional<LocalDateTime>{at(day(2027, 2, 14))});
    EXPECT_EQ(later.notes, (Strings{"1 route(s) skipped because their next flight is after your dates: ORG → Gee One / "
                                    "Gee Two: next flight 14 Feb 2027."}));
    EXPECT_EQ(later.routes_skipped_not_yet_flying, 1);
}

TEST(Planner, D_14_Next_is_merged_only_over_surviving_connections)
{
    // With a destination, ORG lists LGW (dated, in radius) and the pseudo LON (undated):
    // LON is skipped before the merge because ORG lists LGW separately, so its unknown
    // date does not win and the route is not yet flying.
    const AirportCatalog catalog = hand_catalog({
        {.iata = "ORG", .location = {47, 19}, .connections = {{"LGW", at(day(2027, 5, 1))}, {"LON", std::nullopt}}},
        {.iata = "LON", .location = {51.51, -0.118}, .mac = "LON", .connections = undated({"ORG"}), .name = "London (All Airports)", .fake = true},
        {.iata = "LGW", .location = {51.15, -0.19}, .mac = "LON", .connections = undated({"ORG"}), .name = "London Gatwick"},
    });
    Place london = hand_place("London", {51.51, -0.118});
    london.pinned_airports = {"LON"};
    const SearchPlan plan = planner::plan(
        catalog, make_request(hand_place("Origin", {47, 19}), 0, london, 30, day(2026, 11, 10), day(2026, 11, 20)), kToday);
    EXPECT_TRUE(plan.requests.empty());
    EXPECT_EQ(plan.notes, (Strings{"1 route(s) skipped because their next flight is after your dates: ORG → London: next "
                                   "flight 01 May 2027."}));
}

TEST(Planner, Rule_4_3_11_Chunks_before_the_next_departure_are_skipped)
{
    // 03 §4.3.11: TIA r0 → Katowice r30, 2026-11-10..2027-06-30; next date 2027-03-28.
    const SearchPlan plan =
        plan_cat(make_request(match("Tirana"), 0, match("Katowice"), 30, day(2026, 11, 10), day(2027, 6, 30)));
    EXPECT_EQ(describe(plan), (Strings{"TIA→KTW [SPQ] 2027-03-16..2027-04-26", "TIA→KTW [SPQ] 2027-04-27..2027-06-07",
                                       "TIA→KTW [SPQ] 2027-06-08..2027-06-30"}));
    // KRK is served from TIA but lies outside the 30 km destination radius.
    EXPECT_EQ(plan.notes, (Strings{"Served but outside your 30 km destination radius: Kraków (KRK) 67 km."}));
    EXPECT_EQ(plan.routes_considered, 1);
    EXPECT_EQ(plan.routes_skipped_not_yet_flying, 0);

    // A chunk ending on the next departure date is kept; a time of day does not matter.
    const AirportCatalog catalog = hand_catalog({
        {.iata = "ORG", .location = {0, 0}, .connections = {{"DST", at(day(2026, 11, 20), 23, 59)}}},
        {.iata = "DST", .location = {10, 10}, .connections = undated({"ORG"})},
    });
    const SearchPlan edge = planner::plan(
        catalog, make_request(hand_place("Origin", {0, 0}), 0, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 20)),
        kToday);
    EXPECT_EQ(describe(edge), (Strings{"ORG→DST [DST] 2026-11-10..2026-11-20"}));
    const SearchPlan missed = planner::plan(
        catalog, make_request(hand_place("Origin", {0, 0}), 0, std::nullopt, 100, day(2026, 11, 10), day(2026, 11, 19)),
        kToday);
    EXPECT_EQ(missed.notes, (Strings{"1 route(s) skipped because their next flight is after your dates: ORG → DST: next "
                                     "flight 20 Nov 2026."}));
}

TEST(Planner, Rule_4_3_12_Requests_sort_by_origin_rank_destination_rank_group_and_date)
{
    // 03 §11 return leg: destRank BUD 0, VBR 1, DEB 2 (not the ordinal group order).
    const SearchPlan plan =
        plan_cat(make_request(match("London"), 30, match("Budapest"), 200, day(2026, 11, 10), day(2027, 1, 31)));
    EXPECT_EQ(describe(plan), (Strings{
                                  "LGW→BUD [BUD] 2026-11-10..2026-12-21",
                                  "LGW→BUD [BUD] 2026-12-22..2027-01-31",
                                  "LTN→BUD [BUD] 2026-11-10..2026-12-21",
                                  "LTN→BUD [BUD] 2026-12-22..2027-01-31",
                                  "LTN→BTS [VBR] 2026-11-10..2026-12-21",
                                  "LTN→BTS [VBR] 2026-12-22..2027-01-31",
                                  "LTN→DEB [DEB] 2026-11-10..2026-12-21",
                                  "LTN→DEB [DEB] 2026-12-22..2027-01-31",
                              }));
}

// ---------------------------------------------------------------------------
// 03 §4.4 ReachableNames, Display
// ---------------------------------------------------------------------------

TEST(Planner, ReachableNames_collapse_prefixes_join_others_and_sort_by_culture)
{
    const AirportCatalog catalog = hand_catalog({
        {.iata = "ORG", .location = {0, 0}, .connections = undated({"MIL", "BGY", "BE1", "BE2", "ZZZ", "NOB", "ZAG", "ZG2", "ORG"})},
        {.iata = "MIL", .location = {45.5, 9.2}, .mac = "MIL", .connections = undated({"ORG"}), .name = "Milan (All Airports)", .fake = true},
        {.iata = "BGY", .location = {45.67, 9.7}, .mac = "MIL", .connections = undated({"ORG"}), .name = "Milan Bergamo"},
        {.iata = "BE1", .location = {1, 1}, .mac = "BEE", .connections = undated({"ORG"}), .name = "Bee Two"},
        {.iata = "BE2", .location = {1, 2}, .mac = "BEE", .connections = undated({"ORG"}), .name = "Bee One"},
        {.iata = "NOB", .location = {2, 2}, .connections = undated({"XXX"}), .name = "Not Back"},
        {.iata = "ZAG", .location = {3, 3}, .connections = undated({"ORG"}), .name = "Zagreb"},
        {.iata = "ZG2", .location = {3, 4}, .connections = undated({"ORG"}), .name = "Zagreb"},
    });
    const AirportPtr origin = station(catalog, "ORG");
    const std::vector<AirportPtr> origins{origin};
    const Strings exclude{"org"};
    // "Milan" (pseudo) + "Milan Bergamo" → Milan; "Bee One" + "Bee Two" → "Bee One / Bee Two";
    // NOB does not list ORG back; ZZZ is unknown; ORG is excluded (ci); the two Zagreb groups
    // both stay (duplicates across groups are not removed).
    EXPECT_EQ(planner::reachable_names(catalog, origins, exclude),
              (Strings{"Bee One / Bee Two", "Milan", "Zagreb", "Zagreb"}));
    // Fixture: B8 lists of 03 §4.5.
    EXPECT_EQ(planner::reachable_names(*cat(), std::vector<AirportPtr>{station("BUD")}, Strings{"BUD"}),
              (Strings{"Alghero (Sardinia)", "Barcelona El Prat", "Kraków", "London", "Naples", "Tirana"}));
    EXPECT_EQ(planner::reachable_names(*cat(), std::vector<AirportPtr>{station("DEB")}, Strings{"DEB"}), (Strings{"London"}));
    EXPECT_TRUE(planner::reachable_names(*cat(), std::vector<AirportPtr>{}, Strings{}).empty());

    // Culture-equal spellings (NFC "Kraków" and NFD "Kraków") collapse inside one
    // group's set; the first spelling inserted stays. Origins are walked in the given order.
    const AirportCatalog accents = hand_catalog({
        {.iata = "OA", .location = {0, 0}, .connections = undated({"K1"})},
        {.iata = "OB", .location = {0, 1}, .connections = undated({"K2", "K1"})},
        {.iata = "K1", .location = {50, 19}, .mac = "KKK", .connections = undated({"OA", "OB"}), .name = "Kraków"},
        {.iata = "K2", .location = {50, 20}, .mac = "KKK", .connections = undated({"OB"}), .name = "Kraków"},
    });
    EXPECT_EQ(planner::reachable_names(accents, std::vector<AirportPtr>{station(accents, "OB"), station(accents, "OA")},
                                       Strings{}),
              (Strings{"Kraków"}));
    EXPECT_EQ(planner::reachable_names(accents, std::vector<AirportPtr>{station(accents, "OA"), station(accents, "OB")},
                                       Strings{}),
              (Strings{"Kraków"}));
}

TEST(Planner, B8_lists_twelve_names_then_and_n_more)
{
    std::vector<Station> stations{{.iata = "ORG", .location = {0, 0}, .connections = {}}};
    stations.push_back({.iata = "DST", .location = {50, 50}, .name = "Target"});
    for (int i = 0; i < 14; ++i) {
        const std::string code{static_cast<char>('A' + i), static_cast<char>('A' + i), 'X'};
        stations.front().connections.emplace_back(code, std::nullopt);
        stations.push_back({.iata = code, .location = {10, 10.0 + i}, .connections = undated({"ORG"}),
                            .name = std::string{"Place "} + static_cast<char>('A' + i)});
    }
    const AirportCatalog catalog = hand_catalog(stations);
    const SearchPlan plan = planner::plan(catalog,
                                          make_request(hand_place("Home", {0, 0}), 0, hand_place("There", {50, 50}), 0,
                                                       day(2026, 11, 10), day(2026, 11, 20)),
                                          kToday);
    EXPECT_EQ(plan.notes, (Strings{"Wizz Air has no direct route between these areas. From ORG Wizz Air flies to: Place A, "
                                   "Place B, Place C, Place D, Place E, Place F, Place G, Place H, Place I, Place J, Place "
                                   "K, Place L and 2 more."}));

    // No genuine route at all: the bare sentence.
    const AirportCatalog none = hand_catalog({{.iata = "ORG", .location = {0, 0}, .connections = undated({"DST"})},
                                              {.iata = "DST", .location = {50, 50}, .name = "Target"}});
    const SearchPlan bare = planner::plan(none,
                                          make_request(hand_place("Home", {0, 0}), 0, hand_place("There", {50, 50}), 0,
                                                       day(2026, 11, 10), day(2026, 11, 20)),
                                          kToday);
    EXPECT_EQ(bare.notes, (Strings{"Wizz Air has no direct route between these areas.",
                                   "1 route(s) skipped because Wizz Air flies them from a sibling airport (searched "
                                   "separately if it is in range): ORG → Target."}));
}

TEST(Planner, Origin_list_names_the_first_four_origins)
{
    std::vector<Station> stations;
    for (int i = 0; i < 5; ++i) {
        const std::string code{'O', static_cast<char>('A' + i), 'X'};
        stations.push_back({.iata = code, .location = {0, 0.01 * i}, .connections = undated({"FAR"})});
    }
    stations.push_back({.iata = "FAR", .location = {40, 40}, .connections = undated({"OAX", "OBX", "OCX", "ODX", "OEX"}), .name = "Far"});
    const AirportCatalog catalog = hand_catalog(stations);
    const SearchPlan plan = planner::plan(catalog,
                                          make_request(hand_place("Home", {0, 0}), 10, hand_place("Dest", {10, 10}), 10,
                                                       day(2026, 11, 10), day(2026, 11, 20)),
                                          kToday);
    // No destination airport → B4 instead; use a destination with an airport but no route.
    stations.push_back({.iata = "DST", .location = {10, 10}, .name = "Dest"});
    const AirportCatalog with_dest = hand_catalog(stations);
    const SearchPlan b8 = planner::plan(with_dest,
                                        make_request(hand_place("Home", {0, 0}), 10, hand_place("Dest", {10, 10}), 10,
                                                     day(2026, 11, 10), day(2026, 11, 20)),
                                        kToday);
    EXPECT_EQ(plan.notes.size(), 1u);
    EXPECT_TRUE(plan.notes[0].starts_with("No Wizz Air airport within 10 km of Dest."));
    EXPECT_EQ(b8.notes, (Strings{"Wizz Air has no direct route between these areas. From OAX, OBX, OCX, ODX Wizz Air flies "
                                 "to: Far."}));
}

TEST(Planner, Display_falls_back_to_the_code)
{
    EXPECT_EQ(planner::display(*cat(), "LTN"), "London Luton (LTN)");
    EXPECT_EQ(planner::display(*cat(), "ltn"), "London Luton (LTN)");
    EXPECT_EQ(planner::display(*cat(), "LON"), "London (All Airports) (LON)");
    EXPECT_EQ(planner::display(*cat(), "ZZZ"), "ZZZ");
}

TEST(Planner, Notes_use_the_given_locale_for_dates)
{
    const std::shared_ptr<const Locale> german = Locale::create("de-DE");
    const SearchPlan plan = planner::plan(
        *cat(), make_request(match("Budapest"), 0, std::nullopt, 100, day(2026, 10, 30), day(2026, 11, 30)), kToday,
        *german);
    ASSERT_EQ(plan.notes.size(), 2u);
    EXPECT_EQ(plan.notes[0], "Dates before today were skipped (searching from 2026-11-02).");
    EXPECT_EQ(plan.notes[1], "2 route(s) skipped because their next flight is after your dates: BUD → Alghero "
                             "(Sardinia): next flight " +
                                 german->format_date(day(2027, 6, 7), "dd MMM yyyy") +
                                 "; BUD → Naples: next flight " + german->format_date(day(2026, 12, 1), "dd MMM yyyy") +
                                 ".");
}

} // namespace
