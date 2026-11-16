// SearchPlanner on the frozen live route map of 2026-11-02 (02 §14, DECISIONS
// D-24): the real-map examples of 03 §4.3.5, §4.3.7, §4.4 and §4.5. TODAY =
// 2026-11-02, locale en-US.
//
// Places. Where the spec names a place only implicitly, it is derived as follows:
// - "Budapest", "London", "Kaunas": the offline MatchPlace of that text (R2 / R3).
// - "Basel": the offline PrefixPlace("Basel") — the MLH/BSL centroid CityPlace
//   pinned [MLH, BSL] (02 §5.7; MatchPlace returns null for it, 02 §5.5).
// - "Graz": the first `graz` answer of LiveCache/geocode.json (47.0708678, 15.4382786).
// - "Reykjavik": the hand-made place of CT-PLAN-05 part 2, (64.1, −21.9).
// - "geocoded Bucharest": not in geocode.json. The Photon / OpenStreetMap city
//   node of Bucharest, (44.4361414, 26.1027202), pinned [BBU, OTP] as the place
//   resolver's step S4 does (02 §6, row 14 of 02 §11). It reproduces the 03
//   §4.3.7 detail "BBU → Antalya is flown from Bucharest Otopeni (OTP), 15 km away".
// Dates. The Budapest r250 example names its window (10 Nov – 21 Dec 2026, exactly
// one 42-day chunk). The other examples give none; they use the UI's default
// outbound window, today + 7 .. today + 37 = 2026-11-09..2026-12-09 (01 §6). The
// Basel counts depend on it: MLH → Yerevan starts on 18 Dec 2026, so a window
// reaching past that date plans MLH:12 (asserted below to document the dependency).

#include "wizz/core/planner.hpp"

#include "wizz/core/catalog.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/format.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/locale.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using wizz::AirportCatalog;
using wizz::AirportCatalogPtr;
using wizz::AirportPtr;
using wizz::AreaAirport;
using wizz::Date;
using wizz::GeoPoint;
using wizz::LegSearchRequest;
using wizz::Locale;
using wizz::Place;
using wizz::PlannedRequest;
using wizz::SearchPlan;
using Strings = std::vector<std::string>;

namespace planner = wizz::planner;

constexpr Date kToday = Date::from_ymd(2026, 11, 2);
/// The UI's default outbound window (01 §6: today + 7 .. today + 37).
constexpr Date kFrom = kToday.add_days(7);
constexpr Date kTo = kToday.add_days(37);
/// The window of the 03 §4.5 Budapest r250 example.
constexpr Date kExampleFrom = Date::from_ymd(2026, 11, 10);
constexpr Date kExampleTo = Date::from_ymd(2026, 12, 21);

/// The catalog of LiveCache/map.json under en-US. Immutable, built once.
[[nodiscard]] const AirportCatalog& live()
{
    static const AirportCatalogPtr catalog =
        AirportCatalog::from_map_json(wizz::test::read_live_cache("map.json"), Locale::en_us());
    return *catalog;
}

[[nodiscard]] AirportPtr station(std::string_view code)
{
    AirportPtr airport = live().find(code);
    if (!airport) {
        throw std::logic_error("live station missing");
    }
    return airport;
}

[[nodiscard]] Place match(std::string_view text)
{
    std::optional<Place> place = live().match_place(text);
    if (!place) {
        throw std::logic_error("place missing");
    }
    return *place;
}

[[nodiscard]] Place hand_place(std::string name, GeoPoint location, Strings pins = {})
{
    Place place;
    place.description = name;
    place.name = std::move(name);
    place.location = location;
    place.source = wizz::PlaceSource::geocoder;
    place.pinned_airports = std::move(pins);
    return place;
}

[[nodiscard]] LegSearchRequest make_request(Place origin, double origin_km, std::optional<Place> destination,
                                            double destination_km, Date from = kFrom, Date to = kTo)
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

[[nodiscard]] SearchPlan plan_live(const LegSearchRequest& request)
{
    return planner::plan(live(), request, kToday);
}

[[nodiscard]] Strings km_labels(const std::vector<AreaAirport>& airports)
{
    Strings result;
    for (const AreaAirport& entry : airports) {
        result.push_back(entry.airport->iata + " " +
                         wizz::format_label(entry.distance_km, 0, Locale::en_us()->number_symbols()));
    }
    return result;
}

/// Requests per origin IATA.
[[nodiscard]] std::map<std::string, int> requests_per_origin(const SearchPlan& plan)
{
    std::map<std::string, int> counts;
    for (const PlannedRequest& r : plan.requests) {
        ++counts[r.from->iata];
    }
    return counts;
}

/// The note that starts with `prefix`, or "" (one at most is expected).
[[nodiscard]] std::string note_starting_with(const SearchPlan& plan, std::string_view prefix)
{
    const auto it = std::ranges::find_if(plan.notes, [prefix](const std::string& n) { return n.starts_with(prefix); });
    return it == plan.notes.end() ? std::string{} : *it;
}

constexpr std::string_view kMirrorNote =
    " route(s) skipped because Wizz Air flies them from a sibling airport (searched separately if it is in range): ";
constexpr std::string_view kNotYetFlyingNote = " route(s) skipped because their next flight is after your dates: ";

// ---------------------------------------------------------------------------
// 03 §4.5 real map
// ---------------------------------------------------------------------------

TEST(PlannerLive, Example_4_5_Budapest_r200_to_London_r100_plans_four_requests)
{
    const SearchPlan plan = plan_live(make_request(match("Budapest"), 200, match("London"), 100));
    Strings routes;
    for (const PlannedRequest& r : plan.requests) {
        routes.push_back(r.from->iata + "→" + r.query_to);
    }
    EXPECT_EQ(routes, (Strings{"BUD→LGW", "BTS→LTN", "DEB→LTN", "TAT→LTN"}));
    for (const PlannedRequest& r : plan.requests) {
        EXPECT_EQ(r.group_key, "LON");
    }
}

TEST(PlannerLive, Example_4_5_Budapest_r250_to_anywhere_plans_124_requests)
{
    const SearchPlan plan =
        plan_live(make_request(match("Budapest"), 250, std::nullopt, 100, kExampleFrom, kExampleTo));

    EXPECT_EQ(km_labels(plan.origin_airports),
              (Strings{"BUD 0", "BTS 173", "DEB 184", "TAT 196", "KSC 201", "OMR 205", "ARW 207", "TSR 241"}));
    EXPECT_EQ(plan.routes_considered, 166);
    EXPECT_EQ(plan.routes_skipped_not_yet_flying, 42);  // 14 suspended + 28 not yet flying
    EXPECT_EQ(plan.requests.size(), 124u);
    EXPECT_EQ(plan.routes_considered - plan.routes_skipped_not_yet_flying, 124);  // one 42-day chunk each

    const std::string mirror = note_starting_with(plan, "11" + std::string{kMirrorNote});
    EXPECT_FALSE(mirror.empty());
    EXPECT_TRUE(mirror.ends_with("; …."));
    EXPECT_EQ(note_starting_with(plan, "14 route(s) skipped: "),
              "14 route(s) skipped: Wizz Air shows no upcoming flights on them.");
    const std::string later = note_starting_with(plan, "28" + std::string{kNotYetFlyingNote});
    EXPECT_FALSE(later.empty());
    EXPECT_TRUE(later.ends_with("; …."));
    EXPECT_EQ(plan.notes.size(), 3u);  // I4, I5, I6 in that order
    if (plan.notes.size() == 3u) {
        EXPECT_EQ(plan.notes[0], mirror);
        EXPECT_EQ(plan.notes[2], later);
    }

    // Requests run nearest origin first; within an origin the groups are ordinal.
    std::vector<std::size_t> ranks;
    for (const PlannedRequest& r : plan.requests) {
        const auto it = std::ranges::find(plan.origin_airports, r.from, &AreaAirport::airport);
        ranks.push_back(static_cast<std::size_t>(it - plan.origin_airports.begin()));
    }
    EXPECT_TRUE(std::ranges::is_sorted(ranks));
    for (std::size_t i = 1; i < plan.requests.size(); ++i) {
        if (ranks[i] == ranks[i - 1]) {
            EXPECT_LT(plan.requests[i - 1].group_key, plan.requests[i].group_key);
        }
    }
    // UI guards (01): above 60 asks for confirmation, above 500 refuses.
    EXPECT_GT(plan.requests.size(), 60u);
    EXPECT_LE(plan.requests.size(), 500u);
}

// ---------------------------------------------------------------------------
// 03 §4.3.7 real map: mirror stations
// ---------------------------------------------------------------------------

TEST(PlannerLive, Example_4_3_7_Kaunas_r30_to_anywhere_plans_one_request_and_skips_25_mirrors)
{
    const SearchPlan plan = plan_live(make_request(match("Kaunas"), 30, std::nullopt, 100));
    Strings origins;
    for (const AreaAirport& entry : plan.origin_airports) {
        origins.push_back(entry.airport->iata);
    }
    EXPECT_EQ(origins, (Strings{"KUN"}));
    ASSERT_EQ(plan.requests.size(), 1u);
    EXPECT_EQ(plan.requests[0].from->iata, "KUN");
    EXPECT_EQ(plan.requests[0].query_to, "LTN");
    const std::string mirror = note_starting_with(plan, "25" + std::string{kMirrorNote});
    EXPECT_FALSE(mirror.empty());
    EXPECT_NE(mirror.find("KUN → "), std::string::npos);
    EXPECT_NE(mirror.find(" is flown from Vilnius (VNO), "), std::string::npos);
}

TEST(PlannerLive, Example_4_3_7_Geocoded_Bucharest_r50_to_anywhere)
{
    const Place bucharest = hand_place("Bucharest", {44.4361414, 26.1027202}, {"BBU", "OTP"});
    // The example's sibling distance: OTP is 15 km from the place.
    EXPECT_EQ(wizz::format_label(wizz::distance_km(bucharest.location, station("OTP")->location), 0,
                                 Locale::en_us()->number_symbols()),
              "15");

    const SearchPlan plan = plan_live(make_request(bucharest, 50, std::nullopt, 100));
    Strings origins;
    for (const AreaAirport& entry : plan.origin_airports) {
        origins.push_back(entry.airport->iata);
    }
    EXPECT_EQ(origins, (Strings{"BBU", "OTP"}));  // BBU is nearer
    EXPECT_EQ(requests_per_origin(plan), (std::map<std::string, int>{{"BBU", 31}, {"OTP", 53}}));
    ASSERT_FALSE(plan.requests.empty());
    EXPECT_EQ(plan.requests.front().from->iata, "BBU");  // the BBU requests still run first
    const std::string mirror = note_starting_with(plan, "43" + std::string{kMirrorNote});
    EXPECT_FALSE(mirror.empty());
    EXPECT_TRUE(mirror.ends_with("; …."));
    EXPECT_NE(mirror.find("BBU → "), std::string::npos);
}

TEST(PlannerLive, Example_4_3_7_Bucharest_mirror_entry_names_Otopeni_15_km_away)
{
    // The spec's sample entry, reproduced through a destination that keeps only that route:
    // Antalya r0 from the same Bucharest place (BBU lists AYT, AYT does not list BBU back).
    const Place bucharest = hand_place("Bucharest", {44.4361414, 26.1027202}, {"BBU", "OTP"});
    const SearchPlan plan = plan_live(make_request(bucharest, 50, match("Antalya"), 0));
    EXPECT_TRUE(std::ranges::any_of(plan.notes, [](const std::string& note) {
        return note.find("BBU → Antalya is flown from Bucharest Otopeni (OTP), 15 km away") != std::string::npos;
    }));
}

TEST(PlannerLive, Example_4_3_7_Basel_r30_to_anywhere)
{
    const std::optional<Place> basel = live().prefix_place("Basel");
    ASSERT_TRUE(basel.has_value());
    EXPECT_EQ(basel->pinned_airports, (Strings{"MLH", "BSL"}));
    const SearchPlan plan = plan_live(make_request(*basel, 30, std::nullopt, 100));
    EXPECT_EQ(requests_per_origin(plan), (std::map<std::string, int>{{"BSL", 14}, {"MLH", 11}}));
    EXPECT_EQ(plan.routes_considered, 28);

    // The window dependency: MLH → Yerevan (next flight 18 Dec 2026) joins a longer window.
    const SearchPlan longer = plan_live(make_request(*basel, 30, std::nullopt, 100, kExampleFrom, kExampleTo));
    EXPECT_EQ(requests_per_origin(longer), (std::map<std::string, int>{{"BSL", 14}, {"MLH", 12}}));
}

// ---------------------------------------------------------------------------
// 03 §4.3.5 and §4.4 real-map notes
// ---------------------------------------------------------------------------

TEST(PlannerLive, Example_4_3_5_Graz_r50_names_Ljubljana)
{
    const wizz::json::Value geocode = wizz::json::parse(wizz::test::read_live_cache("geocode.json"));
    const wizz::json::Value* graz = geocode.find("graz");
    ASSERT_NE(graz, nullptr);
    const wizz::json::Value& first = graz->at(0);
    ASSERT_EQ(first.get_string("Name"), std::optional<std::string_view>{"Graz"});
    const Place place = hand_place("Graz", {first.find("Lat")->as_double(), first.find("Lon")->as_double()});
    EXPECT_EQ(place.location, (GeoPoint{47.0708678, 15.4382786}));

    const SearchPlan plan = plan_live(make_request(place, 50, std::nullopt, 100));
    EXPECT_EQ(plan.notes, (Strings{"No Wizz Air airport within 50 km of Graz. The nearest is Ljubljana (LJU), 120 km "
                                   "away. Set the start radius to at least 130 km."}));
    EXPECT_TRUE(plan.requests.empty());
}

TEST(PlannerLive, Example_4_4_Reykjavik_r100_to_London_lists_KEF_destinations)
{
    const SearchPlan plan = plan_live(make_request(hand_place("Reykjavik", {64.1, -21.9}), 100, match("London"), 100));
    ASSERT_FALSE(plan.notes.empty());
    EXPECT_EQ(plan.notes[0], "Wizz Air has no direct route between these areas. From KEF Wizz Air flies to: Budapest, "
                             "Gdansk, Katowice, Milan, Vilnius, Warsaw, Wroclaw.");
    EXPECT_TRUE(plan.requests.empty());
}

} // namespace
