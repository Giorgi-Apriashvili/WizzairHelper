// FindCityCentre (DECISIONS D-72): the destination city's centre from the geocoder,
// most specific name first, near the airport only, through the place resolver's
// memo and geocode.json.

#include "wizz/services/city_centre.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/geo.hpp"

#include "place_resolver_harness.hpp"

#include <gtest/gtest.h>

#include <format>
#include <map>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace wizz::test::geocoder {
namespace {

using Strings = std::vector<std::string>;

const GeoPoint kLuton{51.874722, -0.368333};
const GeoPoint kLondon{51.5074, -0.1278};

[[nodiscard]] Airport station(std::string iata, std::string name, std::string country, GeoPoint location)
{
    return Airport{.iata = std::move(iata), .name = std::move(name), .country_name = std::move(country),
                   .location = location};
}

/// A Photon answer: one feature per (name, position).
[[nodiscard]] std::string photon_body(const std::vector<std::pair<std::string, GeoPoint>>& hits)
{
    std::string features;
    for (const auto& [name, at] : hits) {
        if (!features.empty()) {
            features += ',';
        }
        features += std::format(R"({{"geometry":{{"coordinates":[{},{}]}},"properties":{{"name":"{}"}}}})",
                                at.longitude, at.latitude, name);
    }
    return std::format(R"({{"features":[{}]}})", features);
}

/// Photon answering per escaped query (`photon(query)` URLs); other Photon queries and
/// every Nominatim request get an empty answer.
[[nodiscard]] Responder photon_by_query(std::map<std::string, std::string> answers)
{
    return [answers = std::move(answers)](const RecordedRequest& request) {
        for (const auto& [query, body] : answers) {
            if (request.url == photon(query)) {
                return json_response(body);
            }
        }
        return json_response(request.url.contains("photon") ? R"({"features":[]})" : "[]");
    };
}

TEST(CityCentre, D_72_The_most_specific_name_that_names_a_city_near_the_airport_wins)
{
    ResolverRig rig(photon_by_query({
        {"London%20Luton", photon_body({{"Luton", GeoPoint{51.8787, -0.42}}})},  // not "London Luton"
        {"London", photon_body({{"London", GeoPoint{42.9849, -81.2453}}, {"London", kLondon}})},  // Ontario first
    }));
    const CityCentreLookup lookup =
        find_city_centre(rig.resolver, station("LTN", "London Luton", "United Kingdom", kLuton), {});

    ASSERT_TRUE(lookup.centre.has_value());
    EXPECT_EQ(lookup.centre->name, "London");
    EXPECT_EQ(lookup.centre->location, kLondon);
    EXPECT_EQ(lookup.place_name, "London Luton");
    EXPECT_FALSE(lookup.lookup_failed);
    EXPECT_EQ(rig.transport->urls(), (Strings{photon("London%20Luton"), photon("London")}));
    EXPECT_TRUE(rig.log.messages().empty()) << "no notices";
}

TEST(CityCentre, D_72_A_hit_must_lie_within_150_km_of_the_airport)
{
    const GeoPoint airport{50.0, 8.0};
    for (const auto& [km, accepted] : {std::pair{149.9, true}, std::pair{150.2, false}}) {
        ResolverRig rig(photon_by_query({{"Faraway", photon_body({{"Faraway", north_of(airport, km)}})}}));
        const CityCentreLookup lookup = find_city_centre(rig.resolver, station("XXX", "Faraway", "", airport), {});
        EXPECT_EQ(lookup.centre.has_value(), accepted) << km;
    }
}

TEST(CityCentre, D_72_An_island_without_a_city_leaves_the_name_to_Airbnb)
{
    // Photon finds nothing, so the resolver asks Nominatim, which finds nothing either.
    ResolverRig rig(photon_by_query({}));
    const CityCentreLookup lookup = find_city_centre(
        rig.resolver, station("LPA", "Gran Canaria (Canary Islands)\n", "Spain", GeoPoint{27.93, -15.39}), {});

    EXPECT_FALSE(lookup.centre.has_value());
    EXPECT_EQ(lookup.place_name, "Gran Canaria");
    EXPECT_FALSE(lookup.lookup_failed);
    EXPECT_EQ(rig.transport->urls(), (Strings{photon("Gran%20Canaria"), nominatim("Gran%20Canaria"), photon("Gran"),
                                              nominatim("Gran")}));
}

TEST(CityCentre, D_72_A_shortened_name_takes_only_the_city_itself)
{
    // Gran Canaria is an island: "Gran" must not land on Gran Tarajal (Fuerteventura, ~138 km).
    const GeoPoint gando{27.93, -15.39};
    ResolverRig island(photon_by_query({{"Gran", photon_body({{"Gran Tarajal", north_of(gando, 138.0)}})}}));
    const CityCentreLookup lookup =
        find_city_centre(island.resolver, station("LPA", "Gran Canaria (Canary Islands)", "Spain", gando), {});
    EXPECT_FALSE(lookup.centre.has_value());
    EXPECT_EQ(lookup.place_name, "Gran Canaria");

    // "Frankfurt Hahn" → "Frankfurt" → "Frankfurt am Main", about 105 km away.
    const GeoPoint hahn{49.9487, 7.2639};
    const GeoPoint frankfurt{50.1106, 8.6821};
    ResolverRig hahn_rig(photon_by_query({{"Frankfurt", photon_body({{"Frankfurt am Main", frankfurt}})}}));
    const CityCentreLookup city =
        find_city_centre(hahn_rig.resolver, station("HHN", "Frankfurt Hahn", "Germany", hahn), {});
    ASSERT_TRUE(city.centre.has_value());
    EXPECT_EQ(city.centre->name, "Frankfurt am Main");

    // The station's whole name still takes a longer hit ("Tel-Aviv" → "Tel Aviv-Yafo").
    const GeoPoint ben_gurion{32.0114, 34.8867};
    ResolverRig tlv(photon_by_query({{"Tel-Aviv", photon_body({{"Tel Aviv-Yafo", GeoPoint{32.0853, 34.7818}}})}}));
    const CityCentreLookup tel_aviv = find_city_centre(tlv.resolver, station("TLV", "Tel-Aviv", "Israel", ben_gurion), {});
    ASSERT_TRUE(tel_aviv.centre.has_value());
    EXPECT_EQ(tel_aviv.centre->name, "Tel Aviv-Yafo");
}

TEST(CityCentre, D_72_When_the_geocoders_cannot_be_reached_the_lookup_stops)
{
    ResolverRig rig(offline);
    const CityCentreLookup lookup =
        find_city_centre(rig.resolver, station("LTN", "London Luton", "United Kingdom", kLuton), {});

    EXPECT_FALSE(lookup.centre.has_value());
    EXPECT_TRUE(lookup.lookup_failed);
    EXPECT_EQ(lookup.place_name, "London Luton");
    EXPECT_EQ(rig.transport->urls(), (Strings{photon("London%20Luton"), nominatim("London%20Luton")}))
        << "\"London\" is not tried";
}

TEST(CityCentre, D_72_A_station_without_a_position_sends_nothing)
{
    ResolverRig rig(photon_by_query({}));
    const CityCentreLookup lookup =
        find_city_centre(rig.resolver, station("XXX", "  Nowhere (Sea) ", "Atlantis", GeoPoint{}), {});
    EXPECT_FALSE(lookup.centre.has_value());
    EXPECT_FALSE(lookup.lookup_failed);
    EXPECT_EQ(lookup.place_name, "Nowhere");
    EXPECT_EQ(rig.transport->request_count(), 0u);
}

TEST(CityCentre, D_72_A_name_with_nothing_left_after_cleaning_is_kept_as_it_is)
{
    ResolverRig rig(photon_by_query({}));
    const CityCentreLookup lookup = find_city_centre(rig.resolver, station("XXX", " (Sea) ", "", GeoPoint{}), {});
    EXPECT_EQ(lookup.place_name, "(Sea)");
}

TEST(CityCentre, D_72_Cancellation_propagates)
{
    ResolverRig rig(photon_by_query({}));
    std::stop_source cancel;
    cancel.request_stop();
    expect_throws_exactly<OperationCancelled>(
        [&] { (void)find_city_centre(rig.resolver, station("LTN", "London Luton", "", kLuton), cancel.get_token()); });
    EXPECT_EQ(rig.transport->request_count(), 0u);
}

TEST(CityCentre, D_72_Answers_are_remembered_in_geocode_json)
{
    const TempDirectory directory;
    const auto answers = std::map<std::string, std::string>{{"London", photon_body({{"London", kLondon}})},
                                                            {"London%20Luton", photon_body({{"Luton", kLuton}})}};
    {
        ResolverRig rig(photon_by_query(answers), directory.cache());
        ASSERT_TRUE(find_city_centre(rig.resolver, station("LTN", "London Luton", "", kLuton), {}).centre);
        EXPECT_EQ(rig.transport->request_count(), 2u);
    }
    ResolverRig again(photon_by_query(answers), directory.cache());  // a new session
    const CityCentreLookup lookup = find_city_centre(again.resolver, station("LTN", "London Luton", "", kLuton), {});
    ASSERT_TRUE(lookup.centre.has_value());
    EXPECT_EQ(lookup.centre->location, kLondon);
    EXPECT_EQ(again.transport->request_count(), 0u);
}

} // namespace
} // namespace wizz::test::geocoder
