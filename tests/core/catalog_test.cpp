// AirportCatalog on the fixture map and on hand-built catalogs: 05 §2.3, §3.2
// CT-CAT-01..30, CT-LATENT-02 (catalog part), P-11 (b)/(c) texts, 02 §5.1–§5.8,
// 02 §10 AirportCatalogTests. Locale en-US (05 §1.11).

#include "wizz/core/catalog.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/text.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using wizz::Airport;
using wizz::AirportCatalog;
using wizz::AirportCatalogPtr;
using wizz::AirportPtr;
using wizz::AreaAirport;
using wizz::GeoPoint;
using wizz::Locale;
using wizz::NameIndexEntry;
using wizz::Place;
using wizz::PlaceSource;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// CAT (05 §0.3): the catalog of fixtures/map_subset.json under en-US. Immutable, built once.
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

[[nodiscard]] std::vector<std::string> codes(std::span<const AirportPtr> airports)
{
    std::vector<std::string> result;
    for (const AirportPtr& airport : airports) {
        result.push_back(airport->iata);
    }
    return result;
}

[[nodiscard]] std::vector<std::string> codes(const std::vector<AreaAirport>& airports)
{
    std::vector<std::string> result;
    for (const AreaAirport& entry : airports) {
        result.push_back(entry.airport->iata);
    }
    return result;
}

[[nodiscard]] std::vector<std::string> sorted(std::vector<std::string> values)
{
    std::ranges::sort(values);
    return values;
}

using Codes = std::vector<std::string>;

/// xUnit Assert.Equal(expected, actual, places): Math.Round (half to even) on both sides.
[[nodiscard]] double round_places(double value, int places)
{
    const double scale = std::pow(10.0, places);
    return std::nearbyint(value * scale) / scale;
}

/// A hand-built station: Name, CountryName "X", CountryCode "XX", EUR (05 §3.6 style).
[[nodiscard]] Airport make_station(std::string iata, std::string name, std::optional<std::string> mac = std::nullopt,
                                   bool fake = false, GeoPoint location = {}, std::string country = "X")
{
    Airport airport;
    airport.iata = std::move(iata);
    airport.name = std::move(name);
    airport.country_name = std::move(country);
    airport.country_code = "XX";
    airport.currency_code = "EUR";
    airport.location = location;
    airport.mac = std::move(mac);
    airport.is_fake_station = fake;
    return airport;
}

[[nodiscard]] AirportCatalog make_catalog(std::vector<Airport> airports)
{
    return AirportCatalog{std::move(airports), Locale::en_us()};
}

/// Connections rendered like the 05 §2.3 table: `IATA@HH:MM` on 2026-11-02, else
/// `IATA@yyyy-MM-dd HH:MM`; seconds only when not zero; `IATA@null` when unknown.
[[nodiscard]] std::string connections_text(const Airport& airport)
{
    std::string text;
    for (const wizz::RouteConnection& connection : airport.connections) {
        if (!text.empty()) {
            text += ", ";
        }
        text += connection.iata + "@";
        if (!connection.next_departure) {
            text += "null";
            continue;
        }
        const wizz::LocalDateTime when = *connection.next_departure;
        if (wizz::date_of(when) != wizz::Date::from_ymd(2026, 11, 2)) {
            text += wizz::date_of(when).iso() + " ";
        }
        text += wizz::format_hhmm(when);
        if (const std::string iso = wizz::format_iso_seconds(when); !iso.ends_with(":00")) {
            text += iso.substr(iso.size() - 3);
        }
    }
    return text;
}

/// Replace `from` (which must occur exactly once) with `to`.
[[nodiscard]] std::string replace_once(std::string text, std::string_view from, std::string_view to)
{
    const std::size_t at = text.find(from);
    if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) {
        throw std::logic_error("replace_once: not exactly one occurrence");
    }
    return text.replace(at, from.size(), to);
}

/// The JSON object starting at `start` (a '{'), found by brace matching outside strings.
[[nodiscard]] std::string_view json_object_at(std::string_view text, std::size_t start)
{
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return text.substr(start, i - start + 1);
        }
    }
    throw std::logic_error("unterminated JSON object");
}

[[nodiscard]] AirportCatalogPtr parse_map(std::string_view json)
{
    return AirportCatalog::from_map_json(json, Locale::en_us());
}

template <class Error>
[[nodiscard]] std::string error_text(std::string_view json)
{
    try {
        (void)parse_map(json);
    } catch (const Error& error) {
        return error.what();
    }
    return "(no error)";
}

// ---------------------------------------------------------------------------
// Fixture integrity and the parsed station table (05 §2.2, §2.3)
// ---------------------------------------------------------------------------

TEST(Catalog, Fixture_map_subset_is_byte_exact)
{
    const std::string map = wizz::test::read_fixture("map_subset.json");
    EXPECT_EQ(map.size(), 16279u);
    EXPECT_TRUE(wizz::test::sha256_hex(map).starts_with("e027fc99e2e88c64"));
    EXPECT_EQ(map.find_first_of("\r\n"), std::string::npos);  // single line, no CR/LF bytes
}

TEST(Catalog, CT_CAT_01_Parses_map_and_trims_names)
{
    const AirportCatalog& catalog = *cat();
    // [A]
    EXPECT_EQ(catalog.all().size(), 12u);
    EXPECT_EQ(catalog.real_airports().size(), 11u);
    const AirportPtr krk = catalog.find("krk");
    ASSERT_NE(krk, nullptr);
    EXPECT_EQ(krk->iata, "KRK");
    EXPECT_EQ(krk->name, "Kraków");
    EXPECT_EQ(krk->currency_code, "PLN");
    EXPECT_EQ(krk->group_key(), "SPQ");
    ASSERT_NE(catalog.find("LON"), nullptr);
    EXPECT_TRUE(catalog.find("LON")->is_fake_station);
    EXPECT_EQ(catalog.find("BUD")->group_key(), "BUD");
    const AirportPtr bud = catalog.find("BUD");
    const auto aho = std::ranges::find(bud->connections, std::string{"AHO"}, &wizz::RouteConnection::iata);
    ASSERT_NE(aho, bud->connections.end());
    ASSERT_TRUE(aho->next_departure.has_value());
    EXPECT_EQ(wizz::date_of(*aho->next_departure), wizz::Date::from_ymd(2027, 6, 7));
    // [D] JSON order and the exact naive date-time (no zone conversion).
    EXPECT_EQ(codes(catalog.all()),
              (Codes{"TIA", "BUD", "DEB", "AHO", "NAP", "KTW", "KRK", "BTS", "BCN", "LON", "LTN", "LGW"}));
    EXPECT_EQ(*aho->next_departure, wizz::make_local(wizz::Date::from_ymd(2027, 6, 7), 18, 10));
}

TEST(Catalog, Fixture_stations_match_the_05_2_3_table)
{
    struct Row {
        std::string_view iata, name;
        std::optional<std::string> mac;
        bool fake;
        std::string_view currency, country, country_code;
        double lat, lon;
        std::string_view connections;
    };
    const Row rows[] = {
        {"TIA", "Tirana", std::nullopt, false, "ALL", "Albania", "AL", 41.415, 19.72083333333333,
         "AHO@2027-06-23 14:00, BCN@05:55, BTS@2026-11-03 08:55, BUD@13:45, KRK@2027-03-28 15:10, "
         "KTW@2027-03-28 15:10, LGW@09:40, LON@09:40, LTN@09:40, NAP@2026-12-03 06:10"},
        {"BUD", "Budapest", std::nullopt, false, "HUF", "Hungary", "HU", 47.43694444444444, 19.255555555555556,
         "AHO@2027-06-07 18:10, BCN@06:00, KRK@19:50, LGW@06:00, LON@06:00, LTN@06:00, NAP@2026-12-01 21:05, TIA@11:45"},
        {"DEB", "Debrecen", std::nullopt, false, "HUF", "Hungary", "HU", 47.55, 21.7, "LTN@10:20, LON@10:20"},
        {"AHO", "Alghero (Sardinia)", std::nullopt, false, "EUR", "Italy", "IT", 40.63027777777778, 8.289722222222222,
         "BUD@2027-06-07 20:55, TIA@2027-06-23 16:25"},
        {"NAP", "Naples", std::nullopt, false, "EUR", "Italy", "IT", 40.88611111111111, 14.291666666666666,
         "BCN@2026-12-14 06:40, BTS@2026-12-03 20:50, BUD@2026-12-01 18:40, KTW@2026-12-03 08:45, "
         "TIA@2026-12-03 08:05"},
        {"KTW", "Katowice", "SPQ", false, "PLN", "Poland", "PL", 50.483333333333334, 19.083333333333332,
         "BCN@05:30, LTN@06:05, LON@06:05, NAP@2026-12-03 06:00, TIA@2027-03-28 12:45"},
        {"KRK", "Kraków", "SPQ", false, "PLN", "Poland", "PL", 50.07777777777778, 19.784722222222225,
         "BCN@05:30, BUD@21:40, LGW@06:00, LON@06:00, LTN@06:00, TIA@2027-03-28 12:45"},
        {"BTS", "Bratislava", "VBR", false, "EUR", "Slovakia", "SK", 48.166666666666664, 17.216666666666665,
         "BCN@08:25, LTN@09:15, LON@09:15, NAP@2026-12-03 18:30, TIA@2026-11-03 06:45"},
        {"BCN", "Barcelona El Prat", std::nullopt, false, "EUR", "Spain", "ES", 41.29694444444444, 2.0783333333333336,
         "BTS@12:00, BUD@09:25, KRK@09:05, KTW@09:05, LTN@09:50, LON@09:50, NAP@2026-12-14 09:25, TIA@09:20"},
        {"LON", "London (All Airports)", "LON", true, "GBP", "United Kingdom", "GB", 51.51, -0.11805555555555555,
         "BUD@08:30, KRK@08:25, TIA@05:00, BCN@06:00, BTS@11:25, DEB@06:05, KTW@08:25"},
        {"LTN", "London Luton", "LON", false, "GBP", "United Kingdom", "GB", 51.874722222222225, -0.3683333333333333,
         "BCN@06:00, BTS@11:25, BUD@08:30, DEB@06:05, KRK@08:25, KTW@08:25, TIA@05:00"},
        {"LGW", "London Gatwick", "LON", false, "GBP", "United Kingdom", "GB", 51.14805555555556, -0.19027777777777777,
         "BUD@08:30, KRK@08:25, TIA@05:00"},
    };
    ASSERT_EQ(cat()->all().size(), std::size(rows));
    for (std::size_t i = 0; i < std::size(rows); ++i) {
        const Row& row = rows[i];
        const Airport& a = *cat()->all()[i];
        SCOPED_TRACE(row.iata);
        EXPECT_EQ(a.iata, row.iata);
        EXPECT_EQ(a.name, row.name);
        EXPECT_EQ(a.mac, row.mac);
        EXPECT_EQ(a.is_fake_station, row.fake);
        EXPECT_EQ(a.currency_code, row.currency);
        EXPECT_EQ(a.country_name, row.country);
        EXPECT_EQ(a.country_code, row.country_code);
        EXPECT_EQ(a.location.latitude, row.lat);
        EXPECT_EQ(a.location.longitude, row.lon);
        EXPECT_EQ(connections_text(a), row.connections);
    }
    // [D] derived facts: HasCityGroup only for LON, LTN, LGW (LON sets it on itself).
    for (const AirportPtr& airport : cat()->all()) {
        const bool expected = airport->iata == "LON" || airport->iata == "LTN" || airport->iata == "LGW";
        EXPECT_EQ(airport->has_city_group, expected) << airport->iata;
    }
    // [D] RealAirports order (by name, culture-aware).
    EXPECT_EQ(codes(cat()->real_airports()),
              (Codes{"AHO", "BCN", "BTS", "BUD", "DEB", "KTW", "KRK", "LGW", "LTN", "NAP", "TIA"}));
}

// ---------------------------------------------------------------------------
// GroupMembers, Near, Find (CT-CAT-02, CT-CAT-03, 02 §5.3)
// ---------------------------------------------------------------------------

TEST(Catalog, CT_CAT_02_Group_members_exclude_pseudo_station)
{
    // [A]
    EXPECT_EQ(sorted(codes(cat()->group_members("LON"))), (Codes{"LGW", "LTN"}));
    EXPECT_EQ(sorted(codes(cat()->group_members("SPQ"))), (Codes{"KRK", "KTW"}));
    EXPECT_EQ(codes(cat()->group_members("BUD")), (Codes{"BUD"}));
    // [D] unsorted results follow RealAirports (name) order.
    EXPECT_EQ(codes(cat()->group_members("LON")), (Codes{"LGW", "LTN"}));
    EXPECT_EQ(codes(cat()->group_members("SPQ")), (Codes{"KTW", "KRK"}));
}

TEST(Catalog, Group_members_fall_back_to_a_real_station_and_ignore_case)
{
    EXPECT_EQ(codes(cat()->group_members("lon")), (Codes{"LGW", "LTN"}));
    EXPECT_EQ(codes(cat()->group_members("KRK")), (Codes{"KRK"}));  // KRK's group key is SPQ
    EXPECT_EQ(codes(cat()->group_members("BTS")), (Codes{"BTS"}));  // BTS's group key is VBR
    EXPECT_EQ(codes(cat()->group_members("VBR")), (Codes{"BTS"}));
    EXPECT_EQ(codes(cat()->group_members("LTN")), (Codes{"LTN"}));
    EXPECT_TRUE(cat()->group_members("XYZ").empty());
}

TEST(Catalog, CT_CAT_03_Near_returns_real_airports_sorted_by_distance)
{
    const std::vector<AreaAirport> near_bud = cat()->airports_near(station("BUD")->location, 200);
    // [A]
    EXPECT_EQ(codes(near_bud), (Codes{"BUD", "BTS", "DEB"}));
    const std::vector<AreaAirport> near_lon = cat()->airports_near(station("LON")->location, 100);
    EXPECT_TRUE(std::ranges::none_of(near_lon, [](const AreaAirport& e) { return e.airport->is_fake_station; }));
    // [D]
    ASSERT_EQ(near_bud.size(), 3u);
    EXPECT_EQ(near_bud[0].distance_km, 0.0);
    EXPECT_EQ(round_places(near_bud[1].distance_km, 4), 172.5439);
    EXPECT_EQ(round_places(near_bud[2].distance_km, 4), 184.0772);
    EXPECT_EQ(near_bud[1].distance_km, 172.54389037013857);  // CT-GEO-01 [D]
    EXPECT_EQ(codes(near_lon), (Codes{"LGW", "LTN"}));
    ASSERT_EQ(near_lon.size(), 2u);
    EXPECT_EQ(round_places(near_lon[0].distance_km, 4), 40.5581);
    EXPECT_EQ(round_places(near_lon[1].distance_km, 4), 44.0719);
}

TEST(Catalog, Near_distances_from_BUD_match_the_05_2_3_table)
{
    const std::vector<AreaAirport> all =
        cat()->airports_near(station("BUD")->location, std::numeric_limits<double>::max());
    EXPECT_EQ(codes(all), (Codes{"BUD", "BTS", "DEB", "KRK", "KTW", "TIA", "NAP", "AHO", "LGW", "LTN", "BCN"}));
    ASSERT_EQ(all.size(), 11u);
    const double expected[] = {0.0, 172.5, 184.1, 296.2, 339.0, 670.6, 828.7, 1156.2, 1464.4, 1490.8, 1522.1};
    for (std::size_t i = 0; i < all.size(); ++i) {
        EXPECT_EQ(round_places(all[i].distance_km, 1), expected[i]) << all[i].airport->iata;
    }
    EXPECT_EQ(round_places(wizz::distance_km(station("KTW")->location, station("KRK")->location), 4), 67.2118);
    EXPECT_EQ(round_places(wizz::distance_km(station("LTN")->location, station("LGW")->location), 4), 81.7358);
}

TEST(Catalog, Near_radius_edges)
{
    const GeoPoint bud = station("BUD")->location;
    EXPECT_TRUE(cat()->airports_near(bud, -1).empty());
    EXPECT_TRUE(cat()->airports_near(bud, -0.0001).empty());
    EXPECT_TRUE(cat()->airports_near(bud, std::numeric_limits<double>::quiet_NaN()).empty());
    EXPECT_EQ(codes(cat()->airports_near(bud, 0)), (Codes{"BUD"}));  // only stations exactly at the point
    // Inclusive bound: a radius equal to the exact distance keeps the station.
    EXPECT_EQ(codes(cat()->airports_near(bud, 172.54389037013857)), (Codes{"BUD", "BTS"}));
    EXPECT_EQ(cat()->airports_near(bud, std::numeric_limits<double>::infinity()).size(), 11u);
}

TEST(Catalog, Near_ties_keep_real_airports_order)
{
    const GeoPoint here{10, 20};
    const AirportCatalog catalog = make_catalog({make_station("BBB", "Beta", std::nullopt, false, here),
                                                 make_station("SA1", "Same", std::nullopt, false, here),
                                                 make_station("AAA", "Alpha", std::nullopt, false, here),
                                                 make_station("SA2", "Same", std::nullopt, false, here),
                                                 make_station("FAR", "Aaa far", std::nullopt, false, {11, 20})});
    EXPECT_EQ(codes(catalog.real_airports()), (Codes{"FAR", "AAA", "BBB", "SA1", "SA2"}));
    EXPECT_EQ(codes(catalog.airports_near(here, 500)), (Codes{"AAA", "BBB", "SA1", "SA2", "FAR"}));
}

TEST(Catalog, Find_trims_and_ignores_case_and_includes_pseudo_stations)
{
    EXPECT_EQ(cat()->find(" bud "), station("BUD"));  // the catalog's own instance
    EXPECT_EQ(cat()->find("bUd"), station("BUD"));
    EXPECT_EQ(cat()->find("\t\u00A0lon\r\n"), station("LON"));
    EXPECT_EQ(cat()->find(std::string_view{}), nullptr);  // .NET null
    EXPECT_EQ(cat()->find(""), nullptr);
    EXPECT_EQ(cat()->find("   "), nullptr);
    EXPECT_EQ(cat()->find("XYZ"), nullptr);
    EXPECT_EQ(cat()->find("BU D"), nullptr);
}

// ---------------------------------------------------------------------------
// MatchPlace (CT-CAT-04..15, CT-CAT-26, 02 §5.5)
// ---------------------------------------------------------------------------

TEST(Catalog, CT_CAT_04_11_MatchPlace_resolves_wizz_names_offline)
{
    struct Case {
        std::string_view id, input;
        PlaceSource source;
        Codes pinned_sorted;
        std::string_view name, description, location_of;
    };
    const Case cases[] = {
        {"CT-CAT-04", "London", PlaceSource::wizz_city, {"LGW", "LTN"}, "London",
         "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN", "LON"},
        {"CT-CAT-05", "london", PlaceSource::wizz_city, {"LGW", "LTN"}, "London",
         "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN", "LON"},
        {"CT-CAT-06", "LON", PlaceSource::wizz_city, {"LGW", "LTN"}, "London (All Airports)",
         "London (All Airports), United Kingdom \u2014 Wizz Air airports: LGW, LTN", "LON"},
        {"CT-CAT-07", "Budapest", PlaceSource::wizz_airport, {"BUD"}, "Budapest", "Budapest (BUD), Hungary", "BUD"},
        {"CT-CAT-08", "BUD", PlaceSource::wizz_airport, {"BUD"}, "Budapest", "Budapest (BUD), Hungary", "BUD"},
        {"CT-CAT-09", "krakow", PlaceSource::wizz_airport, {"KRK"}, "Kraków", "Kraków (KRK), Poland", "KRK"},
        {"CT-CAT-10", "London Luton", PlaceSource::wizz_airport, {"LTN"}, "London Luton",
         "London Luton (LTN), United Kingdom", "LTN"},
        {"CT-CAT-11", "London Luton (LTN)", PlaceSource::wizz_airport, {"LTN"}, "London Luton (LTN)",
         "London Luton (LTN), United Kingdom", "LTN"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.id);
        const std::optional<Place> place = cat()->match_place(c.input);
        ASSERT_TRUE(place.has_value());
        // [A]
        EXPECT_EQ(place->source, c.source);
        EXPECT_EQ(sorted(place->pinned_airports), c.pinned_sorted);
        // [D]
        EXPECT_EQ(place->name, c.name);
        EXPECT_EQ(place->description, c.description);
        EXPECT_EQ(place->location, station(c.location_of)->location);
    }
    // [D] unsorted pins follow GroupMembers (RealAirports) order.
    EXPECT_EQ(cat()->match_place("London")->pinned_airports, (Codes{"LGW", "LTN"}));
}

TEST(Catalog, CT_CAT_12_15_MatchPlace_returns_null_for_unknown_cities)
{
    EXPECT_FALSE(cat()->match_place("Graz").has_value());      // CT-CAT-12
    const std::optional<Place> lon = cat()->match_place("Lon");  // CT-CAT-13: rule 4, the LON code
    ASSERT_TRUE(lon.has_value());
    EXPECT_EQ(lon->source, PlaceSource::wizz_city);
    EXPECT_EQ(*lon, *cat()->match_place("LON"));  // [D] identical to CT-CAT-06
    EXPECT_FALSE(cat()->match_place("").has_value());     // CT-CAT-14
    EXPECT_FALSE(cat()->match_place("   ").has_value());  // CT-CAT-15
    EXPECT_FALSE(cat()->match_place(std::string_view{}).has_value());
}

TEST(Catalog, CT_CAT_26_London_place_sits_at_the_all_airports_position)
{
    const std::optional<Place> london = cat()->match_place("London");
    ASSERT_TRUE(london.has_value());
    EXPECT_EQ(london->location, cat()->find("LON")->location);
    EXPECT_EQ(london->location.latitude, 51.51);
    EXPECT_EQ(london->location.longitude, -0.11805555555555555);
}

TEST(Catalog, MatchPlace_R1_keeps_the_typed_text_as_name)
{
    const auto expect_ltn = [](std::string_view input, std::string_view name) {
        SCOPED_TRACE(input);
        const std::optional<Place> place = cat()->match_place(input);
        ASSERT_TRUE(place.has_value());
        EXPECT_EQ(place->name, name);
        EXPECT_EQ(place->description, "London Luton (LTN), United Kingdom");
        EXPECT_EQ(place->source, PlaceSource::wizz_airport);
        EXPECT_EQ(place->pinned_airports, (Codes{"LTN"}));
    };
    expect_ltn("Foo (LTN)", "Foo (LTN)");
    expect_ltn("london luton (ltn)", "london luton (ltn)");
    expect_ltn("x ( LTN )", "x ( LTN )");
    expect_ltn("  Foo\t(\u00A0ltn\u2003)  ", "Foo\t(\u00A0ltn\u2003)");  // Unicode \s, whole trimmed text
    expect_ltn("a (BUD) (LTN)", "a (BUD) (LTN)");                        // only the last group counts

    const std::optional<Place> bud = cat()->match_place("(BUD)");
    ASSERT_TRUE(bud.has_value());
    EXPECT_EQ(bud->name, "(BUD)");
    EXPECT_EQ(bud->description, "Budapest (BUD), Hungary");

    // A pseudo code through R1 keeps the typed text in the description.
    const std::optional<Place> lon = cat()->match_place("Foo (LON)");
    ASSERT_TRUE(lon.has_value());
    EXPECT_EQ(lon->name, "Foo (LON)");
    EXPECT_EQ(lon->description, "Foo (LON), United Kingdom \u2014 Wizz Air airports: LGW, LTN");
    EXPECT_EQ(lon->source, PlaceSource::wizz_city);

    EXPECT_FALSE(cat()->match_place("x (LT N)").has_value());
    EXPECT_FALSE(cat()->match_place("BUD)").has_value());
    EXPECT_FALSE(cat()->match_place("(ABCD)").has_value());
    EXPECT_FALSE(cat()->match_place("x (\u212ARK)").has_value());  // KELVIN SIGN is not [A-Za-z]
}

TEST(Catalog, MatchPlace_R1_unknown_code_falls_through_with_the_full_text)
{
    const AirportCatalog catalog = make_catalog({make_station("FOB", "Foo (BAR)")});
    const std::optional<Place> place = catalog.match_place("foo (bar)");  // R1 misses (no BAR), R2 hits
    ASSERT_TRUE(place.has_value());
    EXPECT_EQ(place->name, "Foo (BAR)");
    EXPECT_EQ(place->description, "Foo (BAR) (FOB), X");
    EXPECT_FALSE(cat()->match_place("Budapest (XYZ)").has_value());
}

TEST(Catalog, MatchPlace_R4_uses_the_accent_free_key)
{
    const std::optional<Place> place = cat()->match_place("BÜD");
    ASSERT_TRUE(place.has_value());
    EXPECT_EQ(place->name, "Budapest");
    EXPECT_EQ(place->pinned_airports, (Codes{"BUD"}));
    const std::optional<Place> luton = cat()->match_place(" ltn ");
    ASSERT_TRUE(luton.has_value());
    EXPECT_EQ(luton->name, "London Luton");
}

TEST(Catalog, MatchPlace_rule_order_on_hand_built_catalogs)
{
    // R2 needs exactly one name; two equal names fall to R3/R4 first, then R5.
    const AirportCatalog r4 = make_catalog({make_station("AAA", "Foo", std::nullopt, false, {0, 0}),
                                            make_station("BBB", "Foo", std::nullopt, false, {0, 2}),
                                            make_station("FOO", "Elsewhere", std::nullopt, false, {5, 5})});
    const std::optional<Place> code = r4.match_place("foo");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(code->name, "Elsewhere");  // R4 beats R5
    EXPECT_EQ(code->pinned_airports, (Codes{"FOO"}));

    // R5: several real stations share the exact name -> CityPlace at the centroid.
    const AirportCatalog r5 = make_catalog({make_station("TW1", "Twin City", std::nullopt, false, {0, 0}, "Aland"),
                                            make_station("TW2", "Twin City", std::nullopt, false, {0, 2}, "Bland")});
    const std::optional<Place> twin = r5.match_place("twin   CITY");
    ASSERT_TRUE(twin.has_value());
    EXPECT_EQ(twin->name, "Twin   City");  // title case of the lower-cased typed text
    EXPECT_EQ(twin->description, "Twin   City, Aland \u2014 Wizz Air airports: TW1, TW2");
    EXPECT_EQ(twin->location, (GeoPoint{0, 1}));
    EXPECT_EQ(twin->source, PlaceSource::wizz_city);
    EXPECT_EQ(twin->pinned_airports, (Codes{"TW1", "TW2"}));

    // R3 beats R4, and takes the FIRST pseudo station in map order.
    const AirportCatalog r3 = make_catalog({make_station("ABC", "Something"),
                                            make_station("ZZA", "Abc (All Airports)", "ZZA", true, {1, 1}),
                                            make_station("ZZB", "Abc Any", "ZZB", true, {2, 2})});
    const std::optional<Place> abc = r3.match_place("ABC");
    ASSERT_TRUE(abc.has_value());
    EXPECT_EQ(abc->name, "Abc");
    EXPECT_EQ(abc->location, (GeoPoint{1, 1}));
    EXPECT_EQ(abc->description, "Abc, X \u2014 Wizz Air airports: ");  // a pseudo station without members
    EXPECT_TRUE(abc->pinned_airports.empty());
}

TEST(Catalog, MatchPlace_R6_trusts_prefixes_only_inside_one_city_group)
{
    const AirportCatalog metro =
        make_catalog({make_station("GTS", "Gotham South", "MTR", false, {9.9, 10}),
                      make_station("MTR", "Metro (All Airports)", "MTR", true, {10, 10}),
                      make_station("GTN", "Gotham North", "MTR", false, {10.1, 10}),
                      make_station("SP1", "Springfield East", "SPR", false, {20, 20}),
                      make_station("SP2", "Springfield West", "SPR", false, {20, 22})});
    const std::optional<Place> gotham = metro.match_place("Gotham");
    ASSERT_TRUE(gotham.has_value());
    EXPECT_EQ(gotham->name, "Gotham");
    EXPECT_EQ(gotham->description, "Gotham, X \u2014 Wizz Air airports: GTN, GTS");
    EXPECT_EQ(gotham->location, (GeoPoint{10, 10}));  // the pseudo station, not the centroid
    EXPECT_EQ(gotham->source, PlaceSource::wizz_city);
    EXPECT_EQ(gotham->pinned_airports, (Codes{"GTN", "GTS"}));
    // R3 for the pseudo station's own city name.
    const std::optional<Place> city = metro.match_place("metro");
    ASSERT_TRUE(city.has_value());
    EXPECT_EQ(city->name, "Metro");
    EXPECT_EQ(city->description, "Metro, X \u2014 Wizz Air airports: GTN, GTS");
    // A shared mac without a pseudo station is not trusted.
    EXPECT_FALSE(metro.match_place("Springfield").has_value());
    ASSERT_TRUE(metro.prefix_place("Springfield").has_value());
    EXPECT_EQ(metro.prefix_place("Springfield")->location, (GeoPoint{20, 21}));

    // A prefix match without a mac breaks the "one group" rule.
    const AirportCatalog mixed = make_catalog({make_station("MTR", "Metro (All Airports)", "MTR", true, {10, 10}),
                                               make_station("GTN", "Gotham North", "MTR", false, {10, 10}),
                                               make_station("GTW", "Gotham West", std::nullopt, false, {13, 10})});
    EXPECT_FALSE(mixed.match_place("Gotham").has_value());
    const std::optional<Place> fallback = mixed.prefix_place("Gotham");
    ASSERT_TRUE(fallback.has_value());
    EXPECT_EQ(fallback->location, (GeoPoint{11.5, 10}));
}

// ---------------------------------------------------------------------------
// PrefixMatches / PrefixPlace (CT-CAT-16..17, 02 §5.7)
// ---------------------------------------------------------------------------

TEST(Catalog, CT_CAT_16_17_Lone_prefix_matches_are_not_used_as_city_location_but_are_available_for_pinning)
{
    // CT-CAT-16
    EXPECT_FALSE(cat()->match_place("Barcelona").has_value());
    EXPECT_EQ(codes(cat()->prefix_matches("Barcelona")), (Codes{"BCN"}));
    const std::optional<Place> barcelona = cat()->prefix_place("Barcelona");
    ASSERT_TRUE(barcelona.has_value());
    EXPECT_EQ(barcelona->pinned_airports, (Codes{"BCN"}));
    EXPECT_EQ(barcelona->name, "Barcelona");
    EXPECT_EQ(barcelona->description, "Barcelona, Spain \u2014 Wizz Air airports: BCN");
    EXPECT_EQ(barcelona->location.latitude, 41.29694444444444);
    EXPECT_EQ(barcelona->location.longitude, 2.0783333333333336);
    EXPECT_EQ(barcelona->source, PlaceSource::wizz_city);
    // CT-CAT-17
    EXPECT_FALSE(cat()->match_place("Alghero").has_value());
    EXPECT_EQ(codes(cat()->prefix_matches("Alghero")), (Codes{"AHO"}));
    const std::optional<Place> alghero = cat()->prefix_place("Alghero");
    ASSERT_TRUE(alghero.has_value());
    EXPECT_EQ(alghero->pinned_airports, (Codes{"AHO"}));
    EXPECT_EQ(alghero->name, "Alghero");
    EXPECT_EQ(alghero->description, "Alghero, Italy \u2014 Wizz Air airports: AHO");
    EXPECT_EQ(alghero->location, station("AHO")->location);
    EXPECT_EQ(alghero->source, PlaceSource::wizz_city);
}

TEST(Catalog, Prefix_matches_need_a_whole_word_prefix_of_at_least_three_units)
{
    EXPECT_EQ(codes(cat()->prefix_matches("London")), (Codes{"LGW", "LTN"}));
    EXPECT_EQ(codes(cat()->prefix_matches("  LONDON ")), (Codes{"LGW", "LTN"}));
    EXPECT_TRUE(cat()->prefix_matches("Lon").empty());  // not a whole word
    EXPECT_TRUE(cat()->prefix_matches("Al").empty());   // key shorter than 3
    EXPECT_TRUE(cat()->prefix_matches("Budapest").empty());  // equal name is not a prefix
    EXPECT_EQ(codes(cat()->prefix_matches("Barcelona El")), (Codes{"BCN"}));
    EXPECT_TRUE(cat()->prefix_matches("Barcelona E").empty());
    EXPECT_FALSE(cat()->prefix_place("Lon").has_value());
    // London's prefix place sits at the pseudo station (one shared mac with a pseudo station).
    const std::optional<Place> london = cat()->prefix_place("london");
    ASSERT_TRUE(london.has_value());
    EXPECT_EQ(london->location, station("LON")->location);
    EXPECT_EQ(london->description, "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN");
}

// ---------------------------------------------------------------------------
// FuzzyCorrect / NameIndex / EditDistance (CT-CAT-18..25, 02 §5.8)
// ---------------------------------------------------------------------------

TEST(Catalog, CT_CAT_18_21_FuzzyCorrect_fixes_typos_against_wizz_names)
{
    struct Case {
        std::string_view id, input, expected;
        std::u16string_view winning_key;
    };
    const Case cases[] = {
        {"CT-CAT-18", "Londn", "London", u"london"},
        {"CT-CAT-19", "Budpest", "Budapest", u"budapest"},
        {"CT-CAT-20", "Bratislvaa", "Bratislava", u"bratislava"},
        {"CT-CAT-21", "Barcelna", "Barcelona", u"barcelona"},  // first word of "Barcelona El Prat"
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.id);
        EXPECT_EQ(cat()->fuzzy_correct(c.input), std::optional<std::string>{c.expected});  // [A]
        // [D] the winning candidate and its distance.
        const NameIndexEntry winner{std::u16string{c.winning_key}, std::string{c.expected}};
        EXPECT_NE(std::ranges::find(cat()->name_index(), winner), cat()->name_index().end());
        EXPECT_EQ(AirportCatalog::edit_distance(wizz::text::normalize_key(wizz::text::utf8_to_utf16(c.input)),
                                                c.winning_key),
                  1);
    }
}

TEST(Catalog, CT_CAT_22_24_FuzzyCorrect_leaves_other_text_alone)
{
    EXPECT_EQ(cat()->fuzzy_correct("Graz"), std::nullopt);      // CT-CAT-22
    EXPECT_EQ(cat()->fuzzy_correct("Lon"), std::nullopt);       // CT-CAT-23
    EXPECT_EQ(cat()->fuzzy_correct("Budapest"), std::nullopt);  // CT-CAT-24
    // 02 §9 example 16 and §5.8: "Barna" and "Dublin" have no Wizz name close enough in the fixture.
    EXPECT_EQ(cat()->fuzzy_correct("Barna"), std::nullopt);
    EXPECT_EQ(cat()->fuzzy_correct("Dublin"), std::nullopt);
    EXPECT_EQ(cat()->fuzzy_correct(""), std::nullopt);
}

TEST(Catalog, CT_CAT_25_Edit_distance_counts_transpositions_once)
{
    EXPECT_EQ(AirportCatalog::edit_distance(u"lodnon", u"london"), 1);
    EXPECT_EQ(AirportCatalog::edit_distance(u"londn", u"london"), 1);
    EXPECT_EQ(AirportCatalog::edit_distance(u"x", u"x"), 0);
    EXPECT_EQ(AirportCatalog::edit_distance(u"abc", u""), 3);
    // 02 §5.8 table: OSA, not full Damerau-Levenshtein.
    EXPECT_EQ(AirportCatalog::edit_distance(u"", u""), 0);
    EXPECT_EQ(AirportCatalog::edit_distance(u"ca", u"abc"), 3);
    EXPECT_EQ(AirportCatalog::edit_distance(u"", u"abc"), 3);
    EXPECT_EQ(AirportCatalog::edit_distance(u"kitten", u"sitting"), 3);
    EXPECT_EQ(AirportCatalog::edit_distance(u"bratislvaa", u"bratislava"), 1);
    EXPECT_EQ(AirportCatalog::edit_distance(u"\U00010400", u""), 2);  // UTF-16 code units
}

TEST(Catalog, Fixture_name_index_lists_names_and_first_words)
{
    const std::vector<NameIndexEntry> expected = {
        {u"tirana", "Tirana"},
        {u"budapest", "Budapest"},
        {u"debrecen", "Debrecen"},
        {u"alghero (sardinia)", "Alghero (Sardinia)"},
        {u"alghero", "Alghero"},
        {u"naples", "Naples"},
        {u"katowice", "Katowice"},
        {u"krakow", "Kraków"},
        {u"bratislava", "Bratislava"},
        {u"barcelona el prat", "Barcelona El Prat"},
        {u"barcelona", "Barcelona"},
        {u"london", "London"},
        {u"london luton", "London Luton"},
        {u"london gatwick", "London Gatwick"},
    };
    EXPECT_TRUE(std::ranges::equal(cat()->name_index(), expected));
}

TEST(Catalog, FuzzyCorrect_synthetic_ambiguity_checks)
{
    // 02 §5.8: "Lyon" and "Leon" — two different displays at d = 1 are ambiguous.
    const AirportCatalog lyon_leon = make_catalog({make_station("LYS", "Lyon"), make_station("LEN", "Leon")});
    EXPECT_EQ(lyon_leon.fuzzy_correct("Lion"), std::nullopt);
    EXPECT_EQ(lyon_leon.fuzzy_correct("Lyonn"), std::optional<std::string>{"Lyon"});
    // Two first words "Gran": the same display is not ambiguous.
    const AirportCatalog gran =
        make_catalog({make_station("LPA", "Gran Canaria"), make_station("GSA", "Gran Sasso")});
    EXPECT_EQ(gran.fuzzy_correct("Grat"), std::optional<std::string>{"Gran"});
    // Displays equal under OrdinalIgnoreCase (Unicode simple upper case) are not ambiguous; the first is kept.
    const AirportCatalog krakow = make_catalog({make_station("KRK", "Kraków"), make_station("KRB", "KRAKÓW Balice")});
    EXPECT_EQ(krakow.fuzzy_correct("Krakw"), std::optional<std::string>{"Kraków"});
    // A strictly better later hit clears the ambiguity (keys longer than 7 allow d = 2).
    const AirportCatalog better = make_catalog(
        {make_station("AA1", "Abcdefxx"), make_station("AA2", "Abcdefyy"), make_station("AA3", "Abcdefgx")});
    EXPECT_EQ(better.fuzzy_correct("Abcdefgh"), std::optional<std::string>{"Abcdefgx"});
    const AirportCatalog tied = make_catalog({make_station("AA1", "Abcdefxx"), make_station("AA2", "Abcdefyy")});
    EXPECT_EQ(tied.fuzzy_correct("Abcdefgh"), std::nullopt);
    // Lengths are UTF-16 units: "lyo𐐀" is 5 units, 2 edits from "lyon" (a code-point count would give 1).
    EXPECT_EQ(lyon_leon.fuzzy_correct("Lyo\U00010400"), std::nullopt);
    // Pseudo stations contribute their city name.
    const AirportCatalog city = make_catalog({make_station("WSW", "Warsaw Any", "WSW", true)});
    EXPECT_EQ(city.fuzzy_correct("Varsaw"), std::optional<std::string>{"Warsaw"});
    EXPECT_TRUE(std::ranges::equal(city.name_index(), std::vector<NameIndexEntry>{{u"warsaw", "Warsaw"}}));
}

TEST(Catalog, Name_index_first_word_uses_the_original_spelling)
{
    const AirportCatalog catalog = make_catalog({make_station("FMM", "Memmingen / Munich West"),
                                                 make_station("FKB", "Karlsruhe/Baden-Baden"),
                                                 make_station("CLJ", "Cluj-Napoca"),
                                                 make_station("SPX", "Cairo (Sphinx)"),
                                                 make_station("RZE", "Rzeszów"),
                                                 make_station("NBS", "Ünterberg\u00A0Nord")});
    const std::vector<NameIndexEntry> expected = {
        {u"memmingen / munich west", "Memmingen / Munich West"},
        {u"memmingen", "Memmingen"},
        {u"karlsruhe/baden-baden", "Karlsruhe/Baden-Baden"},
        {u"karlsruhe", "Karlsruhe"},
        {u"cluj-napoca", "Cluj-Napoca"},  // first word "cluj" has 4 units, display "Cluj"
        {u"cluj", "Cluj"},
        {u"cairo (sphinx)", "Cairo (Sphinx)"},
        {u"cairo", "Cairo"},
        {u"rzeszow", "Rzeszów"},
        {u"unterberg nord", "Ünterberg\u00A0Nord"},
        {u"unterberg", "Ünterberg\u00A0Nord"},  // U+00A0 is not one of the split characters
    };
    EXPECT_TRUE(std::ranges::equal(catalog.name_index(), expected));
}

// ---------------------------------------------------------------------------
// SuggestionNames, CityNameOfFake (CT-CAT-27..30, 02 §5.3, §5.4)
// ---------------------------------------------------------------------------

TEST(Catalog, CT_CAT_27_Suggestions_include_city_names_of_pseudo_stations)
{
    const std::vector<std::string> names = cat()->suggestion_names();
    const auto contains = [&names](std::string_view name) { return std::ranges::find(names, name) != names.end(); };
    // [A]
    EXPECT_TRUE(contains("London"));
    EXPECT_TRUE(contains("London Luton"));
    EXPECT_TRUE(contains("Kraków"));
    EXPECT_FALSE(contains("London (All Airports)"));
    // [D]
    EXPECT_EQ(names, (std::vector<std::string>{"Alghero (Sardinia)", "Barcelona El Prat", "Bratislava", "Budapest",
                                               "Debrecen", "Katowice", "Kraków", "London", "London Gatwick",
                                               "London Luton", "Naples", "Tirana"}));
}

TEST(Catalog, Suggestion_names_collapse_case_but_not_accents_and_keep_the_first_spelling)
{
    const AirportCatalog catalog = make_catalog({make_station("AA1", "zagreb"), make_station("AA2", "Zagreb"),
                                                 make_station("AA3", "Malmo"), make_station("AA4", "Malmö"),
                                                 make_station("AA5", "Oslo Any", "AA5", true),
                                                 make_station("AA6", "OSLO")});
    EXPECT_EQ(catalog.suggestion_names(), (std::vector<std::string>{"Malmo", "Malmö", "Oslo", "zagreb"}));
}

TEST(Catalog, CT_CAT_28_30_City_name_of_pseudo_station)
{
    EXPECT_EQ(AirportCatalog::city_name_of_fake("London (All Airports)"), "London");           // CT-CAT-28
    EXPECT_EQ(AirportCatalog::city_name_of_fake("Warsaw Any"), "Warsaw");                      // CT-CAT-29
    EXPECT_EQ(AirportCatalog::city_name_of_fake("Eindhoven / Maastricht"), "Eindhoven / Maastricht");  // CT-CAT-30
}

TEST(Catalog, City_name_of_fake_matches_the_02_5_4_table)
{
    struct Case {
        std::string_view input, expected;
    };
    const Case cases[] = {
        {"London (All Airports)", "London"},
        {"Warsaw Any", "Warsaw"},
        {"Eindhoven / Maastricht", "Eindhoven / Maastricht"},
        {"London (all airports)  ", "London"},
        {"London(ALL AIRPORTS)", "London"},
        {"Warsaw any", "Warsaw"},
        {"Warsaw  Any  ", "Warsaw"},
        {"Any", ""},
        {"Company", "Company"},
        {"Germany", "Germany"},
        {"X (  All   Airports )", "X (  All   Airports )"},
        {"London (All Airports) x", "London (All Airports) x"},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(AirportCatalog::city_name_of_fake(c.input), c.expected) << c.input;
    }
}

TEST(Catalog, City_name_of_fake_follows_dotnet_regex_semantics)
{
    // Expected values recorded with .NET 10 Regex.Replace(…, IgnoreCase).Trim() under en-US.
    struct Case {
        std::string_view input, expected;
    };
    const Case cases[] = {
        {"(all airports)", ""},
        {" ( all airports ) ", ""},
        {"Warsaw ANY\n", "Warsaw"},
        {"Warsaw Any\n\n", "Warsaw"},
        {"Warsaw\u00A0Any", "Warsaw"},
        {"x-any", "x-"},                       // '-' is not a word character
        {"x_any", "x_any"},                    // Pc is a word character
        {"x1any", "x1any"},                    // Nd is a word character
        {"x\u0301any", "x\u0301any"},          // Mn is a word character
        {"x\u0903any", "x\u0903"},             // Mc is not
        {"x\u200Dany", "x\u200Dany"},          // ZERO WIDTH JOINER counts as a word character
        {"x\U00010400any", "x\U00010400"},     // a surrogate half is not a word character
        {"x\u2160any", "x\u2160"},             // Nl is not
        {"x\u00B2any", "x\u00B2"},             // No is not
        {"x\u02B0any", "x\u02B0any"},          // Lm is
        {"x (ALL A\u0130RPORTS)", "x"},        // en-US IgnoreCase: 'i' also matches U+0130
        {"x (all a\u0131rports)", "x (all a\u0131rports)"},
        {"x (all airport\u017F)", "x (all airport\u017F)"},
        {"x \u212Aany", "x \u212Aany"},
        {"  Paris (All Airports)\r\n", "Paris"},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(AirportCatalog::city_name_of_fake(c.input), c.expected) << c.input;
    }
}

// ---------------------------------------------------------------------------
// PlaceFor / CityPlace / title case (02 §5.6)
// ---------------------------------------------------------------------------

TEST(Catalog, Place_for_real_and_pseudo_stations)
{
    const Place real = cat()->place_for(*station("KRK"), "Typed");
    EXPECT_EQ(real.name, "Typed");
    EXPECT_EQ(real.description, "Kraków (KRK), Poland");  // airport.Name, not the display name
    EXPECT_EQ(real.location, station("KRK")->location);
    EXPECT_EQ(real.source, PlaceSource::wizz_airport);
    EXPECT_EQ(real.pinned_airports, (Codes{"KRK"}));

    const Place pseudo = cat()->place_for(*station("LON"), "Big Smoke");
    EXPECT_EQ(pseudo.name, "Big Smoke");
    EXPECT_EQ(pseudo.description, "Big Smoke, United Kingdom \u2014 Wizz Air airports: LGW, LTN");
    EXPECT_EQ(pseudo.location, station("LON")->location);
    EXPECT_EQ(pseudo.source, PlaceSource::wizz_city);
    EXPECT_EQ(pseudo.pinned_airports, (Codes{"LGW", "LTN"}));
}

TEST(Catalog, City_place_title_cases_the_lower_cased_typed_text)
{
    // 02 §5.6 table (en-US), observed through CityPlace.
    struct Case {
        std::string_view typed, name;
    };
    const Case cases[] = {
        {"london", "London"},
        {"LONDON", "London"},
        {"frankfurt am main", "Frankfurt Am Main"},
        {"cluj-napoca", "Cluj-Napoca"},
        {"o'hare", "O'hare"},
        {"a\u2019b", "A\u2019B"},
        {"st. petersburg", "St. Petersburg"},
        {"la coruña", "La Coruña"},
        {"x1y", "X1y"},
        {"1st", "1St"},
        {"a_b", "A_B"},
        {"a\u00B7b", "A\u00B7B"},
        {"(xy)", "(Xy)"},
        {"\u01C6emal", "\u01C5emal"},
        {"tenerife (canary islands)", "Tenerife (Canary Islands)"},
    };
    const AirportPtr airports[] = {station("BUD")};
    for (const Case& c : cases) {
        const Place place = cat()->city_place(airports, c.typed);
        EXPECT_EQ(place.name, c.name) << c.typed;
        EXPECT_EQ(place.description, std::string{c.name} + ", Hungary \u2014 Wizz Air airports: BUD");
    }
    // Under tr-TR "islands" becomes "İslands" (07 §1 locale).
    const AirportCatalogPtr turkish =
        AirportCatalog::from_map_json(wizz::test::read_fixture("map_subset.json"), Locale::create("tr-TR"));
    const AirportPtr bud[] = {turkish->find("BUD")};
    EXPECT_EQ(turkish->city_place(bud, "islands").name, "\u0130slands");
    EXPECT_EQ(turkish->city_place(bud, "ISLANDS").name, "\u0130slands");
}

TEST(Catalog, City_place_location_and_codes)
{
    // One shared mac with a pseudo station: its position; codes in the given order.
    const AirportPtr ltn_lgw[] = {station("LTN"), station("LGW")};
    const Place london = cat()->city_place(ltn_lgw, "london");
    EXPECT_EQ(london.location, station("LON")->location);
    EXPECT_EQ(london.description, "London, United Kingdom \u2014 Wizz Air airports: LTN, LGW");
    EXPECT_EQ(london.pinned_airports, (Codes{"LTN", "LGW"}));
    EXPECT_EQ(london.source, PlaceSource::wizz_city);
    // A regional group without a pseudo station: the centroid.
    const AirportPtr ktw_krk[] = {station("KTW"), station("KRK")};
    const GeoPoint ktw = station("KTW")->location;
    const GeoPoint krk = station("KRK")->location;
    EXPECT_EQ(cat()->city_place(ktw_krk, "silesia").location,
              (GeoPoint{(ktw.latitude + krk.latitude) / 2, (ktw.longitude + krk.longitude) / 2}));
    // Mixed macs (null counts as a value): the centroid, the first airport's country.
    const AirportPtr mixed[] = {station("BUD"), station("LTN")};
    const Place both = cat()->city_place(mixed, "Somewhere");
    EXPECT_EQ(both.description, "Somewhere, Hungary \u2014 Wizz Air airports: BUD, LTN");
    const GeoPoint bud = station("BUD")->location;
    const GeoPoint ltn = station("LTN")->location;
    EXPECT_EQ(both.location, (GeoPoint{(bud.latitude + ltn.latitude) / 2, (bud.longitude + ltn.longitude) / 2}));
    // No airports: Centroid's error.
    try {
        (void)cat()->city_place(std::span<const AirportPtr>{}, "x");
        ADD_FAILURE() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "No points given.");
    }
}

// ---------------------------------------------------------------------------
// Constructor (02 §5.2, D-10, CT-LATENT-02)
// ---------------------------------------------------------------------------

TEST(Catalog, CT_LATENT_02_Duplicate_station_keeps_first_position_and_last_value)
{
    const std::string map = wizz::test::read_fixture("map_subset.json");
    const std::size_t bts_at = map.find(R"({"iata":"BTS","mac":"VBR")");
    ASSERT_NE(bts_at, std::string::npos);
    const std::string duplicate = replace_once(std::string{json_object_at(map, bts_at)}, R"("shortName":"Bratislava")",
                                               R"js("shortName":"Bratislava (duplicate)")js");
    const std::string patched = replace_once(map, R"(],"javascript":null})", "," + duplicate + R"(],"javascript":null})");

    const AirportCatalogPtr catalog = parse_map(patched);
    // The station count remains unchanged.
    EXPECT_EQ(catalog->all().size(), 12u);
    EXPECT_EQ(catalog->real_airports().size(), 11u);
    // Lookup returns the new name.
    const AirportPtr bts = catalog->find("BTS");
    ASSERT_NE(bts, nullptr);
    EXPECT_EQ(bts->name, "Bratislava (duplicate)");
    // The original BTS position is retained (JSON index 7), holding the same instance.
    EXPECT_EQ(codes(catalog->all()),
              (Codes{"TIA", "BUD", "DEB", "AHO", "NAP", "KTW", "KRK", "BTS", "BCN", "LON", "LTN", "LGW"}));
    EXPECT_EQ(catalog->all()[7], bts);
    EXPECT_EQ(std::ranges::count(codes(catalog->real_airports()), std::string{"BTS"}), 1);
    // One station per key for planning: groups, areas and names see BTS once.
    EXPECT_EQ(codes(catalog->group_members("VBR")), (Codes{"BTS"}));
    EXPECT_EQ(codes(catalog->airports_near(bts->location, 0)), (Codes{"BTS"}));
    EXPECT_FALSE(catalog->match_place("Bratislava").has_value());
    ASSERT_TRUE(catalog->match_place("Bratislava (duplicate)").has_value());
    EXPECT_EQ(catalog->match_place("BTS")->name, "Bratislava (duplicate)");
}

TEST(Catalog, Constructor_deduplicates_iata_ignoring_case)
{
    const AirportCatalog catalog =
        make_catalog({make_station("BTS", "First", std::nullopt, false, {1, 1}), make_station("XXX", "Other"),
                      make_station("bts", "Second", "VBR", false, {2, 2})});
    ASSERT_EQ(catalog.all().size(), 2u);
    EXPECT_EQ(catalog.all()[0]->iata, "bts");  // every field from the last occurrence
    EXPECT_EQ(catalog.all()[0]->name, "Second");
    EXPECT_EQ(catalog.all()[0]->mac, std::optional<std::string>{"VBR"});
    EXPECT_EQ(catalog.all()[0]->location, (GeoPoint{2, 2}));
    EXPECT_EQ(catalog.all()[1]->iata, "XXX");
    EXPECT_EQ(catalog.find("BTS"), catalog.all()[0]);
    EXPECT_EQ(codes(catalog.real_airports()), (Codes{"XXX", "bts"}));  // "Other" < "Second"
}

TEST(Catalog, Constructor_derives_has_city_group)
{
    Airport preset = make_station("PRE", "Preset");
    preset.has_city_group = true;
    const AirportCatalog catalog = make_catalog({make_station("MTR", "Metro (All Airports)", "MTR", true),
                                                 make_station("GTN", "Gotham North", "mtr"),
                                                 make_station("RG1", "Regional", "REG"),
                                                 make_station("SOL", "Solo"),
                                                 make_station("NOM", "No Mac Any", std::nullopt, true),
                                                 std::move(preset)});
    EXPECT_TRUE(catalog.find("MTR")->has_city_group);   // its own mac names a pseudo station
    EXPECT_TRUE(catalog.find("GTN")->has_city_group);   // OrdinalIgnoreCase
    EXPECT_FALSE(catalog.find("RG1")->has_city_group);  // regional group without a pseudo station
    EXPECT_FALSE(catalog.find("SOL")->has_city_group);
    EXPECT_FALSE(catalog.find("NOM")->has_city_group);  // a pseudo station without a mac
    EXPECT_TRUE(catalog.find("PRE")->has_city_group);   // an input that is already true stays true
}

TEST(Catalog, Real_airports_use_culture_collation_with_stable_ties)
{
    const AirportCatalog catalog = make_catalog({make_station("ZAG", "Zagreb"), make_station("ANG", "Ängelholm"),
                                                 make_station("S1", "Same"), make_station("AAR", "Aarhus"),
                                                 make_station("AAL", "aalborg"), make_station("S2", "Same"),
                                                 make_station("PSE", "Pseudo Any", "PSE", true)});
    // Ordinal order would be Aarhus, Same, Same, Zagreb, aalborg, Ängelholm.
    EXPECT_EQ(codes(catalog.real_airports()), (Codes{"AAL", "AAR", "ANG", "S1", "S2", "ZAG"}));
    EXPECT_EQ(codes(catalog.all()), (Codes{"ZAG", "ANG", "S1", "AAR", "AAL", "S2", "PSE"}));
}

TEST(Catalog, Constructor_requires_a_locale)
{
    EXPECT_THROW((void)AirportCatalog(std::vector<Airport>{make_station("AAA", "A")}, nullptr), std::invalid_argument);
    EXPECT_THROW((void)AirportCatalog::from_map_json(R"({"cities":[{"iata":"AAA"}]})", nullptr), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// FromMapJson (02 §5.1, 05 CT-CAT-01 [N], P-11 (b)/(c))
// ---------------------------------------------------------------------------

TEST(Catalog, FromMapJson_errors_use_the_spec_texts)
{
    constexpr std::string_view kNoCities = "Wizz route map JSON has no 'cities' array.";
    constexpr std::string_view kNoStations = "Wizz route map contains no stations.";
    EXPECT_EQ(error_text<wizz::FormatError>(R"({"x":1})"), kNoCities);  // P-11 (b)
    EXPECT_EQ(error_text<wizz::FormatError>(R"({"cities":{}})"), kNoCities);
    EXPECT_EQ(error_text<wizz::FormatError>(R"({"cities":null,"javascript":null})"), kNoCities);
    EXPECT_EQ(error_text<wizz::FormatError>(R"({"Cities":[{"iata":"BUD"}]})"), kNoCities);  // case-sensitive
    EXPECT_EQ(error_text<wizz::FormatError>(R"({"cities":[]})"), kNoStations);  // P-11 (c)
    EXPECT_EQ(error_text<wizz::FormatError>(R"({"cities":[{"iata":" "},{"iata":5},{"iata":null},{},{"IATA":"BUD"}]})"),
              kNoStations);
}

TEST(Catalog, FromMapJson_shape_errors_are_json_errors)
{
    // InvalidOperationException equivalents: the root or an element is not an object.
    EXPECT_THROW((void)parse_map(R"([{"iata":"BUD"}])"), wizz::json::TypeError);
    EXPECT_THROW((void)parse_map(R"("cities")"), wizz::json::TypeError);
    EXPECT_THROW((void)parse_map("null"), wizz::json::TypeError);
    EXPECT_THROW((void)parse_map(R"({"cities":[{"iata":"BUD"},5]})"), wizz::json::TypeError);
    EXPECT_THROW((void)parse_map(R"({"cities":[{"iata":"BUD","connections":["LTN"]}]})"), wizz::json::TypeError);
    EXPECT_EQ(error_text<wizz::json::TypeError>(R"({"cities":[[]]})"),
              "The requested operation requires an element of type 'Object', but the target element has type 'Array'.");
    // JsonException equivalents: strict RFC 8259.
    EXPECT_THROW((void)parse_map("not json"), wizz::json::ParseError);
    EXPECT_THROW((void)parse_map(R"({"cities":[{"iata":"BUD"},]})"), wizz::json::ParseError);
    EXPECT_THROW((void)parse_map("{\"cities\":[] /* comment */}"), wizz::json::ParseError);
    EXPECT_THROW((void)parse_map(""), wizz::json::ParseError);
    // A coordinate that does not fit a double (GetDouble FormatException).
    EXPECT_THROW((void)parse_map(R"({"cities":[{"iata":"BUD","latitude":1e400}]})"), wizz::json::FormatError);
    // A blank station is skipped before its other fields are read.
    EXPECT_NO_THROW((void)parse_map(R"({"cities":[{"iata":"","connections":[5]},{"iata":"BUD"}]})"));
}

TEST(Catalog, FromMapJson_applies_strict_kind_tests_trimming_and_upper_casing)
{
    const AirportCatalogPtr catalog = parse_map(R"js({"cities":[
        {"iata":" bud ","shortName":"  Budapest\r\n","countryName":" Hungary ","countryCode":" HU ",
         "currencyCode":" huf ","latitude":47.5,"longitude":-19.25,"mac":" vbr ","isFakeStation":false},
        {"iata":"xyz","shortName":"   ","latitude":"47.4","longitude":null,"mac":"  ","isFakeStation":"true"},
        {"iata":" abc ","mac":5,"isFakeStation":1,"countryName":7,"currencyCode":null},
        {"iata":"PAR","shortName":"Paris (All Airports)","mac":"PAR","isFakeStation":true},
        {"iata":"dup","shortName":"First","shortName":"Last","latitude":1,"latitude":2}
    ],"javascript":null})js");
    ASSERT_EQ(catalog->all().size(), 5u);

    const Airport& bud = *catalog->all()[0];
    EXPECT_EQ(bud.iata, "BUD");
    EXPECT_EQ(bud.name, "Budapest");
    EXPECT_EQ(bud.country_name, "Hungary");
    EXPECT_EQ(bud.country_code, "HU");
    EXPECT_EQ(bud.currency_code, "HUF");
    EXPECT_EQ(bud.location, (GeoPoint{47.5, -19.25}));
    EXPECT_EQ(bud.mac, std::optional<std::string>{"VBR"});
    EXPECT_FALSE(bud.is_fake_station);
    EXPECT_TRUE(bud.connections.empty());

    const Airport& xyz = *catalog->all()[1];
    EXPECT_EQ(xyz.iata, "XYZ");
    EXPECT_EQ(xyz.name, "xyz");  // blank shortName: the trimmed iata, not upper-cased
    EXPECT_EQ(xyz.location, (GeoPoint{0, 0}));  // a string or null coordinate is 0
    EXPECT_EQ(xyz.mac, std::nullopt);           // white-space mac is null
    EXPECT_FALSE(xyz.is_fake_station);          // only the literal true counts
    EXPECT_EQ(xyz.country_name, "");
    EXPECT_EQ(xyz.currency_code, "");

    const Airport& abc = *catalog->all()[2];
    EXPECT_EQ(abc.iata, "ABC");
    EXPECT_EQ(abc.name, "abc");  // missing shortName
    EXPECT_EQ(abc.mac, std::nullopt);
    EXPECT_FALSE(abc.is_fake_station);
    EXPECT_EQ(abc.country_name, "");
    EXPECT_EQ(abc.country_code, "");
    EXPECT_EQ(abc.currency_code, "");

    EXPECT_TRUE(catalog->all()[3]->is_fake_station);
    EXPECT_TRUE(catalog->all()[3]->has_city_group);

    const Airport& dup = *catalog->all()[4];  // the last duplicate property wins
    EXPECT_EQ(dup.name, "Last");
    EXPECT_EQ(dup.location.latitude, 2.0);
}

TEST(Catalog, FromMapJson_reads_connections_strictly)
{
    const AirportCatalogPtr catalog = parse_map(R"({"cities":[{"iata":"BUD","connections":[
        {"iata":" ltn ","operationStartDate":"2026-11-02T06:00:00"},
        {"iata":"LON","operationStartDate":"2026-11-02"},
        {"iata":"AHO","operationStartDate":"2026-11-02 06:00"},
        {"iata":"NAP","operationStartDate":"2026-11-02T06:00:00.123"},
        {"iata":"KRK","operationStartDate":"11/02/2026"},
        {"iata":"TIA","operationStartDate":"2026-11-02T06:00:00+02:00"},
        {"iata":"DEB","operationStartDate":"garbage"},
        {"iata":"BCN","operationStartDate":20261102},
        {"iata":"KTW","operationStartDate":null},
        {"iata":"BTS"},
        {"iata":"   ","operationStartDate":"2026-11-02T06:00:00"},
        {"iata":7},
        {"to":"LGW"},
        {"iata":"LTN","operationStartDate":"1900-01-01T00:00:00"}
    ]}]})");
    const Airport& bud = *catalog->find("BUD");
    const wizz::Date day = wizz::Date::from_ymd(2026, 11, 2);
    const std::vector<wizz::RouteConnection> expected = {
        {"LTN", wizz::make_local(day, 6, 0)},
        {"LON", wizz::make_local(day, 0, 0)},
        {"AHO", wizz::make_local(day, 6, 0)},
        {"NAP", wizz::make_local(day, 6, 0) + std::chrono::milliseconds{123}},
        {"KRK", wizz::make_local(day, 0, 0)},
        {"TIA", wizz::make_local(day, 6, 0)},  // D-22: the wall clock is kept
        {"DEB", std::nullopt},
        {"BCN", std::nullopt},
        {"KTW", std::nullopt},
        {"BTS", std::nullopt},
        {"LTN", wizz::make_local(wizz::Date::from_ymd(1900, 1, 1), 0, 0)},  // duplicates and markers are kept
    };
    EXPECT_EQ(bud.connections, expected);
}

TEST(Catalog, FromMapJson_reads_the_fixture_with_its_trailing_javascript_key)
{
    // 05 §2.2: the top-level key "javascript": null is ignored; KRK's "\r\n" decodes to CR LF and is trimmed.
    const std::string map = wizz::test::read_fixture("map_subset.json");
    EXPECT_NE(map.find(R"("shortName":"Kraków\r\n")"), std::string::npos);
    EXPECT_EQ(parse_map(map)->find("KRK")->name, "Kraków");
}

} // namespace
