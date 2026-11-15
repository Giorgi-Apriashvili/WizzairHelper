// AirportCatalog on the frozen live route map of 2026-11-02 (02 §14, D-24):
// the "[verified]" tables of 02 §2.3, §2.4, §3.3, §5.2–§5.9 and the offline rows
// of the 02 §9 worked examples, plus protocol-reference §4 statistics.
// Locale en-US (05 §1.11).

#include "wizz/core/catalog.hpp"

#include "wizz/core/geo.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/text.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
using wizz::Place;
using wizz::PlaceSource;
using Codes = std::vector<std::string>;

/// The catalog of LiveCache/map.json under en-US. Immutable, built once.
[[nodiscard]] const AirportCatalogPtr& live()
{
    static const AirportCatalogPtr catalog =
        AirportCatalog::from_map_json(wizz::test::read_live_cache("map.json"), wizz::Locale::en_us());
    return catalog;
}

[[nodiscard]] const Airport& station(std::string_view code)
{
    const AirportPtr airport = live()->find(code);
    if (!airport) {
        throw std::logic_error("live station missing");
    }
    return *airport;
}

[[nodiscard]] Codes codes(std::span<const AirportPtr> airports)
{
    Codes result;
    for (const AirportPtr& airport : airports) {
        result.push_back(airport->iata);
    }
    return result;
}

[[nodiscard]] double round_places(double value, int places)
{
    const double scale = std::pow(10.0, places);
    return std::nearbyint(value * scale) / scale;
}

[[nodiscard]] bool is_ascii(std::u16string_view text)
{
    return std::ranges::all_of(text, [](char16_t c) { return c < 0x80; });
}

// ---------------------------------------------------------------------------
// The dataset and what FromMapJson makes of it (02 §14, §2.3, §2.4, §5.9)
// ---------------------------------------------------------------------------

TEST(CatalogLive, Map_snapshot_is_byte_exact)
{
    const std::string map = wizz::test::read_live_cache("map.json");
    EXPECT_EQ(map.size(), 696'606u);
    EXPECT_EQ(wizz::test::sha256_hex(map), "e6da1ea37f6e548fca379b25586070f73cf56b3a22d6ffcf07683d88a0b6b3b7");
}

TEST(CatalogLive, Counts_and_pseudo_stations)
{
    const AirportCatalog& catalog = *live();
    EXPECT_EQ(catalog.all().size(), 198u);
    EXPECT_EQ(catalog.real_airports().size(), 189u);

    // 02 §5.9: the nine pseudo stations in file order, each one's mac equal to its iata.
    struct Pseudo {
        std::string_view iata, name, city;
    };
    const Pseudo expected[] = {
        {"PAR", "Paris (All Airports)", "Paris"},       {"MIL", "Milan (All Airports)", "Milan"},
        {"ROM", "Rome (All Airports)", "Rome"},         {"VEN", "Venice (All Airports)", "Venice"},
        {"NLM", "Eindhoven / Maastricht", "Eindhoven / Maastricht"},
        {"OOS", "Oslo (All Airports)", "Oslo"},         {"WSW", "Warsaw Any", "Warsaw"},
        {"STO", "Stockholm (All Airports)", "Stockholm"}, {"LON", "London (All Airports)", "London"},
    };
    std::vector<AirportPtr> fakes;
    std::ranges::copy_if(catalog.all(), std::back_inserter(fakes), [](const AirportPtr& a) { return a->is_fake_station; });
    ASSERT_EQ(fakes.size(), std::size(expected));
    for (std::size_t i = 0; i < fakes.size(); ++i) {
        SCOPED_TRACE(expected[i].iata);
        EXPECT_EQ(fakes[i]->iata, expected[i].iata);
        EXPECT_EQ(fakes[i]->name, expected[i].name);
        EXPECT_EQ(fakes[i]->mac, std::optional<std::string>{expected[i].iata});
        EXPECT_EQ(AirportCatalog::city_name_of_fake(fakes[i]->name), expected[i].city);
    }
    // STO sits at Arlanda (59.652, 17.919), not the city centre.
    EXPECT_EQ(round_places(station("STO").location.latitude, 3), 59.652);
    EXPECT_EQ(round_places(station("STO").location.longitude, 3), 17.919);
    EXPECT_EQ(station("STO").location, station("ARN").location);
    // Not in the map (02 §5.9, protocol-reference §4).
    for (const std::string_view code : {"VIE", "STN", "TBS", "GRZ", "DUB"}) {
        EXPECT_EQ(live()->find(code), nullptr) << code;
    }
}

TEST(CatalogLive, Parsed_fields_match_the_live_statistics)
{
    // protocol-reference §4: 198 entries in 44 countries, 3445 connections; 32 names end in "\r\n".
    std::set<std::string> countries;
    std::size_t connections = 0;
    std::size_t with_date = 0;
    std::size_t suspended = 0;
    std::size_t unknown_targets = 0;
    for (const AirportPtr& airport : live()->all()) {
        countries.insert(airport->country_code);
        EXPECT_EQ(wizz::text::trim(airport->name), airport->name) << airport->iata;
        for (const wizz::RouteConnection& connection : airport->connections) {
            ++connections;
            with_date += connection.next_departure.has_value() ? 1 : 0;
            if (connection.next_departure && wizz::date_of(*connection.next_departure).year() < 2000) {
                ++suspended;
            }
            unknown_targets += live()->find(connection.iata) ? 0 : 1;
        }
    }
    EXPECT_EQ(countries.size(), 44u);
    EXPECT_EQ(connections, 3445u);
    EXPECT_EQ(with_date, 3445u);  // 02 §2.4: every operationStartDate is a parsable naive date-time
    EXPECT_EQ(suspended, 211u);   // 1900-01-01T00:00:00 markers
    EXPECT_EQ(unknown_targets, 0u);
    EXPECT_EQ(station("KRK").name, "Kraków");    // raw "Kraków\r\n"
    EXPECT_EQ(station("DXB").name, "Dubai");     // raw "Dubai \r\n"
    EXPECT_EQ(station("LPA").name, "Gran Canaria (Canary Islands)");
    EXPECT_EQ(station("PLQ").name, "Palanga \u2013 Klaipeda");
}

TEST(CatalogLive, Has_city_group_marks_exactly_the_nine_city_groups)
{
    // 02 §2.3: true for LTN, LON and every member of the nine city groups; false for KRK, BUD, BTS, ...
    const std::set<std::string> expected = {"PAR", "BVA", "ORY", "MIL", "BGY", "MXP", "ROM", "CIA", "FCO", "VEN",
                                            "VCE", "TSF", "NLM", "EIN", "MST", "OOS", "OSL", "TRF", "WSW", "WAW",
                                            "WMI", "RDO", "STO", "ARN", "NYO", "LON", "LTN", "LGW"};
    std::set<std::string> actual;
    for (const AirportPtr& airport : live()->all()) {
        if (airport->has_city_group) {
            actual.insert(airport->iata);
        }
    }
    EXPECT_EQ(actual, expected);
    EXPECT_FALSE(station("KRK").has_city_group);
    EXPECT_FALSE(station("BUD").has_city_group);
    EXPECT_FALSE(station("BTS").has_city_group);  // mac VBR has no pseudo station
}

TEST(CatalogLive, Mac_groups_match_02_5_9)
{
    std::map<std::string, Codes> groups;
    for (const AirportPtr& airport : live()->real_airports()) {
        if (airport->mac) {
            groups[*airport->mac].push_back(airport->iata);
        }
    }
    for (auto& [mac, members] : groups) {
        std::ranges::sort(members);
    }
    const std::map<std::string, Codes> expected = {
        {"BXW", {"CRL"}},        {"BIH", {"SJJ", "TZL"}}, {"BAS", {"BSL", "MLH"}}, {"PAR", {"BVA", "ORY"}},
        {"GEL", {"KUT"}},        {"APU", {"BDS"}},        {"MIL", {"BGY", "MXP"}}, {"ROM", {"CIA", "FCO"}},
        {"VEN", {"TSF", "VCE"}}, {"LTE", {"KUN", "VNO"}}, {"NLM", {"EIN", "MST"}}, {"OOS", {"OSL", "TRF"}},
        {"SPQ", {"KRK", "KTW"}}, {"WSW", {"RDO", "WAW", "WMI"}}, {"ARW", {"ARW", "TSR"}}, {"BUH", {"BBU", "OTP"}},
        {"ERA", {"IAS"}},        {"VBR", {"BTS"}},        {"VAL", {"CDT", "VLC"}}, {"STO", {"ARN", "NYO"}},
        {"UAE", {"AUH", "DXB"}}, {"SCT", {"GLA"}},        {"DSA", {"LBA"}},        {"LON", {"LGW", "LTN"}},
    };
    EXPECT_EQ(groups.size(), 24u);
    EXPECT_EQ(groups, expected);
}

// ---------------------------------------------------------------------------
// GroupMembers, Near, SuggestionNames (02 §5.2, §5.3)
// ---------------------------------------------------------------------------

TEST(CatalogLive, Group_members_follow_the_collation_order)
{
    struct Case {
        std::string_view key;
        Codes members;
    };
    const Case cases[] = {
        {"LON", {"LGW", "LTN"}}, {"lon", {"LGW", "LTN"}},       {"PAR", {"BVA", "ORY"}},
        {"MIL", {"BGY", "MXP"}}, {"ROM", {"CIA", "FCO"}},       {"WSW", {"WAW", "WMI", "RDO"}},
        {"STO", {"ARN", "NYO"}}, {"OOS", {"OSL", "TRF"}},       {"NLM", {"EIN", "MST"}},
        {"SPQ", {"KTW", "KRK"}}, {"BUH", {"BBU", "OTP"}},       {"BAS", {"MLH", "BSL"}},  // equal names: map order
        {"VEN", {"VCE", "TSF"}},  // "Venice" < "Venice Treviso"
        {"KRK", {"KRK"}},        {"BTS", {"BTS"}},              {"VBR", {"BTS"}},
        {"XYZ", {}},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(codes(live()->group_members(c.key)), c.members) << c.key;
    }
}

TEST(CatalogLive, Near_Budapest_matches_the_protocol_reference_example)
{
    // protocol-reference §4: real stations within 400 km of BUD were BTS 173, DEB 184, TAT 196,
    // KSC 201, OMR 205, ARW 207, TSR 241, SUJ 274, KRK 296, BEG 302, ... (km).
    const std::vector<AreaAirport> near = live()->airports_near(station("BUD").location, 400);
    struct Expected {
        std::string_view iata;
        double km;
    };
    const Expected expected[] = {{"BUD", 0},   {"BTS", 173}, {"DEB", 184}, {"TAT", 196}, {"KSC", 201}, {"OMR", 205},
                                 {"ARW", 207}, {"TSR", 241}, {"SUJ", 274}, {"KRK", 296}, {"BEG", 302}};
    ASSERT_GE(near.size(), std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        EXPECT_EQ(near[i].airport->iata, expected[i].iata) << i;
        EXPECT_EQ(round_places(near[i].distance_km, 0), expected[i].km) << expected[i].iata;
    }
    EXPECT_TRUE(std::ranges::all_of(near, [](const AreaAirport& e) { return e.distance_km <= 400; }));
    // The nearest-station probe of SearchPlanner.NoAirportNote (double.MaxValue).
    EXPECT_EQ(live()->airports_near(GeoPoint{}, std::numeric_limits<double>::max()).size(), 189u);
}

TEST(CatalogLive, Suggestion_names_collapse_to_196)
{
    const std::vector<std::string> names = live()->suggestion_names();
    EXPECT_EQ(names.size(), 196u);
    const std::vector<std::string> first(names.begin(), names.begin() + 8);
    EXPECT_EQ(first, (std::vector<std::string>{"Aberdeen", "Abu Dhabi", "Agadir", "Alesund", "Alexandria",
                                               "Alghero (Sardinia)", "Alicante", "Almería"}));
    EXPECT_EQ(std::ranges::count(names, std::string{"Venice"}), 1);
    EXPECT_EQ(std::ranges::count(names, std::string{"Basel-Mulhouse-Freiburg"}), 1);
    EXPECT_EQ(std::ranges::count(names, std::string{"Warsaw"}), 1);
    EXPECT_EQ(std::ranges::count(names, std::string{"Eindhoven / Maastricht"}), 1);
    EXPECT_EQ(std::ranges::count(names, std::string{"London (All Airports)"}), 0);
}

// ---------------------------------------------------------------------------
// MatchPlace (02 §5.5, §5.9)
// ---------------------------------------------------------------------------

TEST(CatalogLive, Match_place_of_every_code_returns_the_station_itself)
{
    for (const AirportPtr& airport : live()->all()) {
        for (const std::string& code : {airport->iata, wizz::text::to_lower_invariant(airport->iata)}) {
            SCOPED_TRACE(code);
            const std::optional<Place> place = live()->match_place(code);
            ASSERT_TRUE(place.has_value());
            EXPECT_EQ(place->name, airport->name);
            EXPECT_EQ(place->location, airport->location);
            if (airport->is_fake_station) {
                EXPECT_EQ(place->source, PlaceSource::wizz_city);
                EXPECT_EQ(place->pinned_airports, codes(live()->group_members(airport->iata)));
            } else {
                EXPECT_EQ(place->source, PlaceSource::wizz_airport);
                EXPECT_EQ(place->description, airport->display() + ", " + airport->country_name);
                EXPECT_EQ(place->pinned_airports, (Codes{airport->iata}));
            }
        }
    }
}

TEST(CatalogLive, Every_pseudo_city_name_is_caught_before_prefixes)
{
    // R3 for eight pseudo stations (the place sits at the pseudo station).
    struct Case {
        std::string_view typed, iata;
        Codes pins;
    };
    const Case r3[] = {
        {"London", "LON", {"LGW", "LTN"}},     {"Paris", "PAR", {"BVA", "ORY"}},
        {"Milan", "MIL", {"BGY", "MXP"}},      {"Rome", "ROM", {"CIA", "FCO"}},
        {"Eindhoven / Maastricht", "NLM", {"EIN", "MST"}}, {"Oslo", "OOS", {"OSL", "TRF"}},
        {"Warsaw", "WSW", {"WAW", "WMI", "RDO"}}, {"Stockholm", "STO", {"ARN", "NYO"}},
    };
    for (const Case& c : r3) {
        SCOPED_TRACE(c.typed);
        const std::optional<Place> place = live()->match_place(c.typed);
        ASSERT_TRUE(place.has_value());
        EXPECT_EQ(place->name, c.typed);
        EXPECT_EQ(place->source, PlaceSource::wizz_city);
        EXPECT_EQ(place->location, station(c.iata).location);
        EXPECT_EQ(place->pinned_airports, c.pins);
    }
    // R2 beats R3: "Venice" is the VCE station only, not the VEN group (02 §9 example 13).
    const std::optional<Place> venice = live()->match_place("Venice");
    ASSERT_TRUE(venice.has_value());
    EXPECT_EQ(venice->name, "Venice");
    EXPECT_EQ(venice->description, "Venice (VCE), Italy");
    EXPECT_EQ(venice->source, PlaceSource::wizz_airport);
    EXPECT_EQ(venice->pinned_airports, (Codes{"VCE"}));
    EXPECT_EQ(venice->location, station("VCE").location);
    EXPECT_EQ(round_places(wizz::distance_km(station("VCE").location, station("TSF").location), 0), 20);
}

TEST(CatalogLive, Match_place_consequences_of_02_5_5)
{
    // R4 with a pseudo code gives the raw pseudo name; "London" (R3) gives "London".
    for (const std::string_view typed : {"Lon", "LON"}) {
        const std::optional<Place> place = live()->match_place(typed);
        ASSERT_TRUE(place.has_value()) << typed;
        EXPECT_EQ(place->name, "London (All Airports)");
        EXPECT_EQ(place->source, PlaceSource::wizz_city);
        EXPECT_EQ(place->pinned_airports, (Codes{"LGW", "LTN"}));
    }
    EXPECT_EQ(live()->match_place("London")->name, "London");
    // R1 keeps whatever was typed as the name.
    const std::optional<Place> foo = live()->match_place("Foo (LTN)");
    ASSERT_TRUE(foo.has_value());
    EXPECT_EQ(foo->name, "Foo (LTN)");
    EXPECT_EQ(foo->description, "London Luton (LTN), United Kingdom");
    for (const std::string_view typed : {"london luton (ltn)", "x ( LTN )", "(BUD)"}) {
        EXPECT_TRUE(live()->match_place(typed).has_value()) << typed;
    }
    EXPECT_FALSE(live()->match_place("x (LT N)").has_value());
    EXPECT_FALSE(live()->match_place("BUD)").has_value());
    // Niš has key "nis" and code INI; no 3-unit name key collides with another code.
    EXPECT_EQ(live()->match_place("Niš")->pinned_airports, (Codes{"INI"}));
    EXPECT_EQ(live()->match_place("nis")->pinned_airports, (Codes{"INI"}));
    // Lone or group-less prefix matches return null.
    for (const std::string_view typed : {"Barcelona", "Frankfurt", "Tenerife", "Bucharest", "Basel", "Berlin",
                                         "Brussels", "Santiago", "Cairo", "Cluj", "Alghero"}) {
        EXPECT_FALSE(live()->match_place(typed).has_value()) << typed;
    }
}

TEST(CatalogLive, Shared_name_Basel_Mulhouse_Freiburg_hits_R5)
{
    const std::optional<Place> basel = live()->match_place("Basel-Mulhouse-Freiburg");
    ASSERT_TRUE(basel.has_value());
    EXPECT_EQ(basel->name, "Basel-Mulhouse-Freiburg");
    EXPECT_EQ(basel->description, "Basel-Mulhouse-Freiburg, France \u2014 Wizz Air airports: MLH, BSL");
    EXPECT_EQ(basel->source, PlaceSource::wizz_city);
    EXPECT_EQ(basel->pinned_airports, (Codes{"MLH", "BSL"}));
    // 02 §3.3: Basel MLH+BSL centroid (47.601, 7.5254).
    EXPECT_EQ(round_places(basel->location.latitude, 3), 47.601);
    EXPECT_EQ(round_places(basel->location.longitude, 4), 7.5254);
    const GeoPoint mlh = station("MLH").location;
    const GeoPoint bsl = station("BSL").location;
    EXPECT_EQ(basel->location, (GeoPoint{(mlh.latitude + bsl.latitude) / 2, (mlh.longitude + bsl.longitude) / 2}));
}

// ---------------------------------------------------------------------------
// PrefixMatches / PrefixPlace (02 §5.7) and the offline rows of 02 §9
// ---------------------------------------------------------------------------

TEST(CatalogLive, Prefix_matches_table_of_02_5_7)
{
    struct Case {
        std::string_view typed;
        Codes matches;
    };
    const Case cases[] = {
        {"Barcelona", {"BCN"}}, {"Tenerife", {"TFS", "TFN"}}, {"Frankfurt", {"HHN"}},  {"Berlin", {"BER"}},
        {"Brussels", {"CRL"}},  {"Basel", {"MLH", "BSL"}},    {"Bucharest", {"BBU", "OTP"}},
        {"London", {"LGW", "LTN"}}, {"Venice", {"TSF"}},      {"Santiago", {"SCQ"}},   {"Cairo", {"SPX"}},
        {"Karlsruhe", {"FKB"}}, {"Cluj", {"CLJ"}},            {"Lon", {}},             {"Al", {}},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(codes(live()->prefix_matches(c.typed)), c.matches) << c.typed;
    }
    // Locations: the airport, the centroid, HHN 102.9 km from Frankfurt, the LON pseudo station.
    EXPECT_EQ(live()->prefix_place("Barcelona")->location, station("BCN").location);
    const GeoPoint tenerife = live()->prefix_place("Tenerife")->location;
    EXPECT_EQ(round_places(tenerife.latitude, 4), 28.2636);
    EXPECT_EQ(round_places(tenerife.longitude, 4), -16.4571);
    const GeoPoint frankfurt = live()->prefix_place("Frankfurt")->location;
    EXPECT_EQ(frankfurt, station("HHN").location);
    EXPECT_EQ(round_places(wizz::distance_km(GeoPoint{50.1106444, 8.6820917}, frankfurt), 1), 102.9);
    EXPECT_EQ(round_places(wizz::distance_km(GeoPoint{50.1106444, 8.6820917}, frankfurt), 2), 102.89);  // 02 §3.2
    const GeoPoint bucharest = live()->prefix_place("Bucharest")->location;
    const GeoPoint bbu = station("BBU").location;
    const GeoPoint otp = station("OTP").location;
    EXPECT_EQ(bucharest, (GeoPoint{(bbu.latitude + otp.latitude) / 2, (bbu.longitude + otp.longitude) / 2}));
    EXPECT_EQ(live()->prefix_place("London")->location, station("LON").location);
    EXPECT_FALSE(live()->prefix_place("Lon").has_value());
    // PrefixPlace("Barcelona") in full.
    const Place barcelona = *live()->prefix_place("Barcelona");
    EXPECT_EQ(barcelona.name, "Barcelona");
    EXPECT_EQ(barcelona.description, "Barcelona, Spain \u2014 Wizz Air airports: BCN");
    EXPECT_EQ(barcelona.source, PlaceSource::wizz_city);
    EXPECT_EQ(barcelona.pinned_airports, (Codes{"BCN"}));
}

TEST(CatalogLive, Worked_examples_02_9_offline_rows)
{
    // #1 London (S1, R3).
    const Place london = *live()->match_place("London");
    EXPECT_EQ(london.source, PlaceSource::wizz_city);
    EXPECT_EQ(london.name, "London");
    EXPECT_EQ(london.description, "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN");
    EXPECT_EQ(london.location.latitude, 51.51);
    EXPECT_EQ(round_places(london.location.longitude, 5), -0.11806);
    EXPECT_EQ(london.pinned_airports, (Codes{"LGW", "LTN"}));
    // #2 LON / Lon (S1, R4): same pins and location.
    for (const std::string_view typed : {"LON", "Lon"}) {
        const Place place = *live()->match_place(typed);
        EXPECT_EQ(place.source, PlaceSource::wizz_city);
        EXPECT_EQ(place.name, "London (All Airports)");
        EXPECT_EQ(place.location, london.location);
        EXPECT_EQ(place.pinned_airports, london.pinned_airports);
    }
    // #3 ltn (R4).
    const Place ltn = *live()->match_place("ltn");
    EXPECT_EQ(ltn.source, PlaceSource::wizz_airport);
    EXPECT_EQ(ltn.name, "London Luton");
    EXPECT_EQ(ltn.description, "London Luton (LTN), United Kingdom");
    EXPECT_EQ(ltn.pinned_airports, (Codes{"LTN"}));
    // #5b Barcelona offline: the S4 fallback PrefixPlace sits at the airport.
    const Place barcelona = *live()->prefix_place("Barcelona");
    EXPECT_EQ(barcelona.description, "Barcelona, Spain \u2014 Wizz Air airports: BCN");
    EXPECT_EQ(barcelona.location, station("BCN").location);
    // #6b Tenerife offline: centroid (28.2636, −16.4571).
    const Place tenerife = *live()->prefix_place("Tenerife");
    EXPECT_EQ(tenerife.source, PlaceSource::wizz_city);
    EXPECT_EQ(tenerife.name, "Tenerife");
    EXPECT_EQ(tenerife.description, "Tenerife, Spain \u2014 Wizz Air airports: TFS, TFN");
    EXPECT_EQ(round_places(tenerife.location.latitude, 4), 28.2636);
    EXPECT_EQ(round_places(tenerife.location.longitude, 4), -16.4571);
    // #7b Frankfurt offline: at HHN (49.9486, 7.2639).
    const Place frankfurt = *live()->prefix_place("Frankfurt");
    EXPECT_EQ(frankfurt.name, "Frankfurt");
    EXPECT_EQ(frankfurt.description, "Frankfurt, Germany \u2014 Wizz Air airports: HHN");
    EXPECT_EQ(round_places(frankfurt.location.latitude, 4), 49.9486);
    EXPECT_EQ(round_places(frankfurt.location.longitude, 4), 7.2639);
    // #10 Dublin offline (catalog part): FuzzyCorrect → "Lublin", which MatchPlace resolves to LUZ.
    EXPECT_EQ(live()->fuzzy_correct("Dublin"), std::optional<std::string>{"Lublin"});
    const Place lublin = *live()->match_place("Lublin");
    EXPECT_EQ(lublin.source, PlaceSource::wizz_airport);
    EXPECT_EQ(lublin.name, "Lublin");
    EXPECT_EQ(lublin.description, "Lublin (LUZ), Poland");
    // #11 STN: not a Wizz code; too short to correct.
    EXPECT_EQ(live()->find("STN"), nullptr);
    EXPECT_FALSE(live()->match_place("STN").has_value());
    EXPECT_EQ(live()->fuzzy_correct("STN"), std::nullopt);
    // #12b Graz offline on the live map: "Gran" (first word of "Gran Canaria (Canary Islands)").
    EXPECT_EQ(live()->fuzzy_correct("Graz"), std::optional<std::string>{"Gran"});
    EXPECT_FALSE(live()->match_place("Gran").has_value());
    const Place gran = *live()->prefix_place("Gran");
    EXPECT_EQ(gran.source, PlaceSource::wizz_city);
    EXPECT_EQ(gran.name, "Gran");
    EXPECT_EQ(gran.description, "Gran, Spain \u2014 Wizz Air airports: LPA");
    // #13 Venice: R2, the VCE station only.
    EXPECT_EQ(live()->match_place("Venice")->pinned_airports, (Codes{"VCE"}));
    // #14 Bucharest offline: no offline match; the centroid CityPlace.
    EXPECT_FALSE(live()->match_place("Bucharest").has_value());
    const Place bucharest = *live()->prefix_place("Bucharest");
    EXPECT_EQ(bucharest.name, "Bucharest");
    EXPECT_EQ(bucharest.description, "Bucharest, Romania \u2014 Wizz Air airports: BBU, OTP");
    EXPECT_EQ(bucharest.pinned_airports, (Codes{"BBU", "OTP"}));
    // #15 "x": nothing offline.
    EXPECT_FALSE(live()->match_place("x").has_value());
}

// ---------------------------------------------------------------------------
// FuzzyCorrect / NameIndex (02 §5.8, §4.2)
// ---------------------------------------------------------------------------

TEST(CatalogLive, Fuzzy_correct_table_of_02_5_8)
{
    struct Case {
        std::string_view typed;
        std::optional<std::string> expected;
    };
    const Case cases[] = {
        {"Londn", "London"},       {"Lodnon", "London"},      {"Budpest", "Budapest"},  {"Budapets", "Budapest"},
        {"Bratislvaa", "Bratislava"}, {"Barcelna", "Barcelona"}, {"Frankfrut", "Frankfurt"}, {"Tenerif", "Tenerife"},
        {"Pariss", "Paris"},       {"Romr", "Rome"},          {"Milano", "Milan"},      {"Varsaw", "Warsaw"},
        {"Krakw", "Kraków"},       {"Katowce", "Katowice"},   {"Stokholm", "Stockholm"}, {"Gdnask", "Gdansk"},
        {"Sofai", "Sofia"},        {"Basle", "Basel"},        {"Dublin", "Lublin"},     {"Lubin", "Lublin"},
        {"Leon", "Lyon"},          {"Lyons", "Lyon"},         {"Homburg", "Hamburg"},   {"Tirano", "Tirana"},
        {"Graz", "Gran"},          {"Viena", std::nullopt},   {"Vienna", std::nullopt}, {"Wien", std::nullopt},
        {"Rom", std::nullopt},     {"Bergen", std::nullopt},  {"Akureyri", std::nullopt}, {"Tbilisi", std::nullopt},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(live()->fuzzy_correct(c.typed), c.expected) << c.typed;
    }
}

TEST(CatalogLive, Name_index_has_235_entries_and_one_non_ascii_key)
{
    EXPECT_EQ(live()->name_index().size(), 235u);
    // 02 §4.2: the only live station key that is not ASCII is "palanga – klaipeda" (EN DASH).
    std::vector<std::u16string> non_ascii_station_keys;
    for (const AirportPtr& airport : live()->all()) {
        std::u16string key = wizz::text::normalize_key(wizz::text::utf8_to_utf16(airport->name));
        if (!is_ascii(key)) {
            non_ascii_station_keys.push_back(std::move(key));
        }
    }
    EXPECT_EQ(non_ascii_station_keys, (std::vector<std::u16string>{u"palanga \u2013 klaipeda"}));
    std::vector<std::u16string> non_ascii_index_keys;
    for (const wizz::NameIndexEntry& entry : live()->name_index()) {
        if (!is_ascii(entry.key)) {
            non_ascii_index_keys.push_back(entry.key);
        }
    }
    EXPECT_EQ(non_ascii_index_keys, (std::vector<std::u16string>{u"palanga \u2013 klaipeda"}));
    // Displays are full names, pseudo city names or original first words (accents kept).
    const auto has = [](std::u16string_view key, std::string_view display) {
        return std::ranges::find(live()->name_index(), wizz::NameIndexEntry{std::u16string{key}, std::string{display}}) !=
               live()->name_index().end();
    };
    EXPECT_TRUE(has(u"gran", "Gran"));
    EXPECT_TRUE(has(u"barcelona", "Barcelona"));
    EXPECT_TRUE(has(u"basel", "Basel"));
    EXPECT_TRUE(has(u"krakow", "Kraków"));
    EXPECT_TRUE(has(u"london", "London"));
    EXPECT_TRUE(has(u"warsaw", "Warsaw"));
    EXPECT_TRUE(has(u"eindhoven / maastricht", "Eindhoven / Maastricht"));
    EXPECT_TRUE(has(u"palanga", "Palanga"));
}

} // namespace
