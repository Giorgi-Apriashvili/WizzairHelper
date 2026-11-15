// PlaceResolver on the frozen live dataset of 2026-11-02 (02 §9 worked examples,
// §14; DECISIONS D-24). LiveCache/geocode.json — the Photon answers the app
// memoised that day — is copied into a temporary cache directory with a fresh
// timestamp, so rows 4, 5, 6, 7 and 12 resolve against LiveCache/map.json with
// zero HTTP requests. The offline rows run with every request failing, and the
// synthetic rows with hand-written Photon answers. en-US, FIXED clock.

#include "wizz/services/place_resolver.hpp"

#include "wizz/core/catalog.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/locale.hpp"

#include "services/place_resolver_harness.hpp"
#include "support/fake_transport.hpp"
#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

using wizz::Airport;
using wizz::AirportCatalog;
using wizz::AirportCatalogPtr;
using wizz::Duration;
using wizz::FileCache;
using wizz::GeoPoint;
using wizz::Place;
using wizz::PlaceLookup;
using wizz::PlaceSource;
using wizz::geocoding::GeocodeCacheEntry;
using wizz::geocoding::PlaceDto;
using wizz::test::RecordedRequest;
using wizz::test::ScriptedResponse;
using wizz::test::json_response;
using wizz::test::read_live_cache;
using namespace wizz::test::geocoder;

namespace geocoding = wizz::geocoding;
namespace json = wizz::json;

using Codes = std::vector<std::string>;

/// The catalog of LiveCache/map.json under en-US.
[[nodiscard]] const AirportCatalogPtr& live()
{
    static const AirportCatalogPtr catalog =
        AirportCatalog::from_map_json(read_live_cache("map.json"), wizz::Locale::en_us());
    return catalog;
}

/// CAT (05 §0.3), for row 12c.
[[nodiscard]] const AirportCatalogPtr& fixture_catalog()
{
    static const AirportCatalogPtr catalog =
        AirportCatalog::from_map_json(wizz::test::read_fixture("map_subset.json"), wizz::Locale::en_us());
    return catalog;
}

[[nodiscard]] const Airport& station(std::string_view code)
{
    const wizz::AirportPtr airport = live()->find(code);
    if (!airport) {
        throw std::logic_error("live station missing");
    }
    return *airport;
}

/// The memoised answers of LiveCache/geocode.json.
[[nodiscard]] const std::vector<GeocodeCacheEntry>& live_memo()
{
    static const std::vector<GeocodeCacheEntry> entries = geocoding::parse_geocode_cache(read_live_cache("geocode.json"));
    return entries;
}

[[nodiscard]] const std::vector<PlaceDto>& cached_hits(std::string_view key)
{
    for (const GeocodeCacheEntry& entry : live_memo()) {
        if (entry.key == key) {
            return entry.hits;
        }
    }
    throw std::logic_error("key missing from geocode.json");
}

void expect_place(const Place& actual, std::string_view name, std::string_view description, GeoPoint location,
                  PlaceSource source, const Codes& pins)
{
    EXPECT_EQ(actual.name, name);
    EXPECT_EQ(actual.description, description);
    EXPECT_EQ(actual.location.latitude, location.latitude);
    EXPECT_EQ(actual.location.longitude, location.longitude);
    EXPECT_EQ(actual.source, source);
    EXPECT_EQ(actual.pinned_airports, pins);
}

/// The cached hit as the resolver turns it into a candidate (ToPlace, optionally pinned).
void expect_cached_candidate(const Place& actual, const PlaceDto& hit, const Codes& pins = {})
{
    expect_place(actual, hit.name, hit.detail, {hit.lat, hit.lon}, PlaceSource::geocoder, pins);
}

[[nodiscard]] double rounded(double value, int places)
{
    const double scale = std::pow(10.0, places);
    return std::nearbyint(value * scale) / scale;
}

/// Any request at all fails the test: the answers must come from geocode.json.
[[nodiscard]] wizz::test::Responder no_network()
{
    return [](const RecordedRequest&) -> ScriptedResponse { throw std::logic_error("must not be called"); };
}

/// A resolver whose temporary cache holds LiveCache/geocode.json, rewritten one hour
/// before the FIXED now so the 90-day TTL keeps it fresh.
struct LiveCacheRig {
    LiveCacheRig() : rig(no_network(), directory.cache())
    {
        write_bytes(directory.geocode_file(), read_live_cache("geocode.json"));
        set_last_write_utc(directory.geocode_file(), rig.clock->utc_now() - std::chrono::hours{1});
    }

    TempDirectory directory;
    ResolverRig rig;

    [[nodiscard]] PlaceLookup resolve(std::string_view text) { return rig.resolve(text, *live()); }

    /// Zero HTTP, zero notices, and the file was not rewritten (memo hits never write).
    void expect_answered_from_the_cache() const
    {
        EXPECT_EQ(rig.transport->request_count(), 0u);
        EXPECT_TRUE(rig.log.notices().empty());
        EXPECT_EQ(read_bytes(directory.geocode_file()), read_live_cache("geocode.json"));
    }
};

/// A resolver over the live map whose every request fails (02 §9 "offline").
struct OfflineRig {
    OfflineRig() : rig(offline) {}
    ResolverRig rig;
    [[nodiscard]] PlaceLookup resolve(std::string_view text) { return rig.resolve(text, *live()); }
};

// ===========================================================================
// 02 §14 — the frozen geocode.json
// ===========================================================================

TEST(PlaceResolverLive, Frozen_geocode_json_is_intact_and_round_trips_byte_for_byte)
{
    const std::string file = read_live_cache("geocode.json");
    EXPECT_EQ(file.size(), 3469u);
    EXPECT_EQ(wizz::test::sha256_hex(file), "1cc8d93603362c7a6a2ce1081eb2f1a21f27c375db4eefe13b1ea84fef666120");

    // Keys in order, with hit counts (02 §14).
    std::vector<std::pair<std::string, std::size_t>> keys;
    for (const GeocodeCacheEntry& entry : live_memo()) {
        keys.emplace_back(entry.key, entry.hits.size());
    }
    EXPECT_EQ(keys, (std::vector<std::pair<std::string, std::size_t>>{
                        {"graz", 6}, {"tbilisi", 2}, {"akureyri", 1}, {"londn", 6},
                        {"barcelona", 6}, {"frankfurt", 6}, {"tenerife", 3}}));
    EXPECT_EQ(cached_hits("frankfurt")[3].detail, "Francourt, Bourgogne \u2013 Franche-Comté, France");

    // The C++ writer reproduces the .NET serialisation exactly (\uXXXX escapes, shortest doubles).
    EXPECT_EQ(geocoding::serialize_geocode_cache(live_memo()), file);
}

// ===========================================================================
// 02 §9 — rows answered from the cached Photon answers, zero HTTP
// ===========================================================================

TEST(PlaceResolverLive, Example_4_Londn_suggests_London_because_no_cached_hit_agrees)
{
    LiveCacheRig cached;

    const PlaceLookup result = cached.resolve("Londn");

    cached.expect_answered_from_the_cache();
    EXPECT_TRUE(result.needs_confirmation);
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"London"});
    EXPECT_FALSE(result.lookup_failed);
    ASSERT_EQ(result.candidates.size(), 7u);
    expect_place(result.candidates[0], "London",
                 "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN (from \u201CLondn\u201D)", {51.51, -0.11805555555555555},
                 PlaceSource::wizz_city, {"LGW", "LTN"});
    const std::vector<PlaceDto>& hits = cached_hits("londn");
    ASSERT_EQ(hits.size(), 6u);
    for (std::size_t i = 0; i < hits.size(); ++i) {
        SCOPED_TRACE(hits[i].detail);
        expect_cached_candidate(result.candidates[i + 1], hits[i]);
        EXPECT_GE(wizz::distance_km(result.candidates[i + 1].location, result.candidates[0].location), 150.0);
    }
    EXPECT_EQ(result.candidates[2].description, "Londa, Tuscany, Italy");
}

TEST(PlaceResolverLive, Example_5_Barcelona_pins_BCN_to_the_Spanish_city_and_keeps_the_namesakes)
{
    LiveCacheRig cached;

    const PlaceLookup result = cached.resolve("Barcelona");

    cached.expect_answered_from_the_cache();
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_FALSE(result.lookup_failed);
    const std::vector<PlaceDto>& hits = cached_hits("barcelona");
    ASSERT_EQ(result.candidates.size(), 6u);
    expect_cached_candidate(result.candidates[0], hits[0], {"BCN"});
    expect_place(result.candidates[0], "Barcelona", "Barcelona, Catalonia, Spain", {41.3825802, 2.177073},
                 PlaceSource::geocoder, {"BCN"});
    EXPECT_EQ(rounded(wizz::distance_km(result.candidates[0].location, station("BCN").location), 1), 12.6);
    for (std::size_t i = 1; i < hits.size(); ++i) {
        SCOPED_TRACE(hits[i].detail);
        expect_cached_candidate(result.candidates[i], hits[i]);
    }
}

TEST(PlaceResolverLive, Example_6_Tenerife_demotes_the_Colombian_namesake)
{
    LiveCacheRig cached;

    const PlaceLookup result = cached.resolve("Tenerife");

    cached.expect_answered_from_the_cache();
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
    const std::vector<PlaceDto>& hits = cached_hits("tenerife");
    ASSERT_EQ(result.candidates.size(), 3u);
    expect_place(result.candidates[0], "Santa Cruz de Tenerife", "Santa Cruz de Tenerife, Canary Islands, Spain",
                 {28.467178, -16.2507843}, PlaceSource::geocoder, {"TFS", "TFN"});
    EXPECT_EQ(rounded(wizz::distance_km(result.candidates[0].location, station("TFS").location), 1), 56.6);
    EXPECT_EQ(rounded(wizz::distance_km(result.candidates[0].location, station("TFN").location), 1), 9.1);
    expect_cached_candidate(result.candidates[1], hits[0]);
    EXPECT_EQ(result.candidates[1].description, "Tenerife, Magdalena, Colombia");
    expect_cached_candidate(result.candidates[2], hits[2]);
    EXPECT_EQ(result.candidates[2].description, "Tenetiše, Slovenia");
}

TEST(PlaceResolverLive, Example_7_Frankfurt_pins_Hahn_to_the_Hesse_city_only)
{
    LiveCacheRig cached;

    const PlaceLookup result = cached.resolve("Frankfurt");

    cached.expect_answered_from_the_cache();
    EXPECT_FALSE(result.needs_confirmation);
    const std::vector<PlaceDto>& hits = cached_hits("frankfurt");
    ASSERT_EQ(result.candidates.size(), 6u);
    expect_place(result.candidates[0], "Frankfurt", "Frankfurt, Hesse, Germany", {50.1106444, 8.6820917},
                 PlaceSource::geocoder, {"HHN"});
    EXPECT_EQ(rounded(wizz::distance_km(result.candidates[0].location, station("HHN").location), 1), 102.9);
    for (std::size_t i = 1; i < hits.size(); ++i) {
        SCOPED_TRACE(hits[i].detail);
        expect_cached_candidate(result.candidates[i], hits[i]);
    }
    EXPECT_EQ(result.candidates[1].name, "Frankfurt (Oder)");
    EXPECT_EQ(std::round(wizz::distance_km(result.candidates[1].location, station("HHN").location)), 573.0);
}

TEST(PlaceResolverLive, Example_12_Graz_is_the_one_same_name_hit_with_Wizz_nearby)
{
    LiveCacheRig cached;

    const PlaceLookup result = cached.resolve("Graz");

    cached.expect_answered_from_the_cache();
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
    const std::vector<PlaceDto>& hits = cached_hits("graz");
    ASSERT_EQ(result.candidates.size(), 6u);
    for (std::size_t i = 0; i < hits.size(); ++i) {
        SCOPED_TRACE(hits[i].detail);
        expect_cached_candidate(result.candidates[i], hits[i]);
    }
    const auto near_graz = live()->airports_near(result.candidates[0].location, 300.0);
    ASSERT_FALSE(near_graz.empty());
    EXPECT_EQ(near_graz[0].airport->iata, "LJU");
    EXPECT_EQ(std::round(near_graz[0].distance_km), 120.0);
}

// ===========================================================================
// 02 §9 — rows that need no network at all
// ===========================================================================

TEST(PlaceResolverLive, Examples_1_2_3_13_15_resolve_without_network)
{
    LiveCacheRig cached;
    const GeoPoint lon{51.51, -0.11805555555555555};

    const PlaceLookup london = cached.resolve("London");  // 1: R3
    ASSERT_EQ(london.candidates.size(), 1u);
    expect_place(london.candidates[0], "London", "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN", lon,
                 PlaceSource::wizz_city, {"LGW", "LTN"});

    for (const std::string_view code : {"LON", "Lon"}) {  // 2: R4
        SCOPED_TRACE(code);
        const PlaceLookup result = cached.resolve(code);
        ASSERT_EQ(result.candidates.size(), 1u);
        expect_place(result.candidates[0], "London (All Airports)",
                     "London (All Airports), United Kingdom \u2014 Wizz Air airports: LGW, LTN", lon, PlaceSource::wizz_city,
                     {"LGW", "LTN"});
    }

    const PlaceLookup luton = cached.resolve("ltn");  // 3: R4
    ASSERT_EQ(luton.candidates.size(), 1u);
    expect_place(luton.candidates[0], "London Luton", "London Luton (LTN), United Kingdom", station("LTN").location,
                 PlaceSource::wizz_airport, {"LTN"});

    const PlaceLookup venice = cached.resolve("Venice");  // 13: R2, not the VEN group
    ASSERT_EQ(venice.candidates.size(), 1u);
    expect_place(venice.candidates[0], "Venice", "Venice (VCE), Italy", station("VCE").location,
                 PlaceSource::wizz_airport, {"VCE"});

    const PlaceLookup x = cached.resolve("x");  // 15: S2
    EXPECT_TRUE(x.candidates.empty());
    EXPECT_FALSE(x.lookup_failed);

    for (const PlaceLookup* result : {&london, &luton, &venice, &x}) {
        EXPECT_FALSE(result->needs_confirmation);
        EXPECT_FALSE(result->corrected_text.has_value());
    }
    EXPECT_EQ(cached.rig.transport->request_count(), 0u);
}

// ===========================================================================
// 02 §9 — offline rows (every request throws a transport error)
// ===========================================================================

TEST(PlaceResolverLive, Example_5b_6b_7b_14_offline_station_named_cities_fall_back_to_the_prefix_place)
{
    OfflineRig offline_rig;

    const PlaceLookup barcelona = offline_rig.resolve("Barcelona");
    ASSERT_EQ(barcelona.candidates.size(), 1u);
    expect_place(barcelona.candidates[0], "Barcelona", "Barcelona, Spain \u2014 Wizz Air airports: BCN",
                 station("BCN").location, PlaceSource::wizz_city, {"BCN"});

    const PlaceLookup tenerife = offline_rig.resolve("Tenerife");
    ASSERT_EQ(tenerife.candidates.size(), 1u);
    const GeoPoint between[] = {station("TFS").location, station("TFN").location};
    expect_place(tenerife.candidates[0], "Tenerife", "Tenerife, Spain \u2014 Wizz Air airports: TFS, TFN",
                 wizz::centroid(between), PlaceSource::wizz_city, {"TFS", "TFN"});
    EXPECT_EQ(rounded(tenerife.candidates[0].location.latitude, 4), 28.2636);
    EXPECT_EQ(rounded(tenerife.candidates[0].location.longitude, 4), -16.4571);

    const PlaceLookup frankfurt = offline_rig.resolve("Frankfurt");  // degraded: centred on the airport
    ASSERT_EQ(frankfurt.candidates.size(), 1u);
    expect_place(frankfurt.candidates[0], "Frankfurt", "Frankfurt, Germany \u2014 Wizz Air airports: HHN",
                 {49.948611111111106, 7.263888888888889}, PlaceSource::wizz_city, {"HHN"});

    const PlaceLookup bucharest = offline_rig.resolve("Bucharest");  // group BUH has no pseudo station
    ASSERT_EQ(bucharest.candidates.size(), 1u);
    const GeoPoint bucharest_airports[] = {station("BBU").location, station("OTP").location};
    expect_place(bucharest.candidates[0], "Bucharest", "Bucharest, Romania \u2014 Wizz Air airports: BBU, OTP",
                 wizz::centroid(bucharest_airports), PlaceSource::wizz_city, {"BBU", "OTP"});

    for (const PlaceLookup* result : {&barcelona, &tenerife, &frankfurt, &bucharest}) {
        EXPECT_FALSE(result->needs_confirmation);
        EXPECT_FALSE(result->corrected_text.has_value());
        EXPECT_FALSE(result->lookup_failed);
    }
    EXPECT_EQ(offline_rig.rig.transport->request_count(), 8u);
}

TEST(PlaceResolverLive, Example_10_offline_Dublin_is_only_suggested_as_Lublin)
{
    OfflineRig offline_rig;

    const PlaceLookup result = offline_rig.resolve("Dublin");

    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Lublin", "Lublin (LUZ), Poland (from \u201CDublin\u201D)", station("LUZ").location,
                 PlaceSource::wizz_airport, {"LUZ"});
    EXPECT_TRUE(result.needs_confirmation);
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"Lublin"});
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_EQ(offline_rig.rig.transport->urls(), (Codes{photon("Dublin"), nominatim("Dublin")}));
    // "1969 km from Dublin" (OpenStreetMap's Dublin point).
    EXPECT_EQ(std::round(wizz::distance_km({53.3493795, -6.2605593}, station("LUZ").location)), 1969.0);
}

TEST(PlaceResolverLive, Example_11b_offline_STN_is_a_lookup_failure)
{
    OfflineRig offline_rig;

    const PlaceLookup result = offline_rig.resolve("STN");

    EXPECT_TRUE(result.candidates.empty());
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_TRUE(result.lookup_failed);
    EXPECT_EQ(live()->find("STN"), nullptr) << "Stansted is not in the map";
    EXPECT_EQ(live()->fuzzy_correct("STN"), std::nullopt) << "key length 3";
    EXPECT_EQ(offline_rig.rig.transport->urls(), (Codes{photon("STN"), nominatim("STN")}));
}

TEST(PlaceResolverLive, Example_12b_offline_Graz_on_the_live_map_suggests_Gran_after_four_attempts)
{
    OfflineRig offline_rig;
    ASSERT_EQ(live()->fuzzy_correct("Graz"), std::optional<std::string>{"Gran"});
    ASSERT_EQ(live()->match_place("Gran"), std::nullopt);

    const PlaceLookup result = offline_rig.resolve("Graz");

    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Gran", "Gran, Spain \u2014 Wizz Air airports: LPA (from \u201CGraz\u201D)",
                 station("LPA").location, PlaceSource::wizz_city, {"LPA"});
    EXPECT_TRUE(result.needs_confirmation);
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"Gran"});
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_EQ(offline_rig.rig.transport->urls(),
              (Codes{photon("Graz"), nominatim("Graz"), photon("Gran"), nominatim("Gran")}));
    EXPECT_EQ(offline_rig.rig.log.messages(), (Codes{photon_notice("Graz"), nominatim_notice("Graz"),
                                                     photon_notice("Gran"), nominatim_notice("Gran")}));
    // The second Nominatim call waits for its 1.1 s slot (FIXED clock: the full spacing).
    EXPECT_EQ(offline_rig.rig.delay.delays(), std::vector<Duration>{geocoding::kNominatimSpacing});
}

TEST(PlaceResolverLive, Example_12c_offline_Graz_on_the_fixture_catalog_finds_nothing)
{
    {
        SCOPED_TRACE("Photon and Nominatim both throw");
        ResolverRig rig(offline);
        const PlaceLookup result = rig.resolve("Graz", *fixture_catalog());
        EXPECT_TRUE(result.candidates.empty());
        EXPECT_FALSE(result.needs_confirmation);
        EXPECT_TRUE(result.lookup_failed);
        EXPECT_EQ(rig.transport->request_count(), 2u);
    }
    {
        SCOPED_TRACE("Nominatim answers []");
        ResolverRig rig(photon_responder(offline));
        const PlaceLookup result = rig.resolve("Graz", *fixture_catalog());
        EXPECT_TRUE(result.candidates.empty());
        EXPECT_FALSE(result.lookup_failed);
    }
}

// ===========================================================================
// 02 §9 — rows with a synthetic Photon answer, on the live map
// ===========================================================================

/// A one-feature-per-entry Photon answer.
[[nodiscard]] std::string photon_body(std::initializer_list<std::pair<PlaceDto, std::optional<std::string>>> hits)
{
    json::Writer writer;
    writer.begin_object().key("features").begin_array();
    for (const auto& [hit, country] : hits) {
        writer.begin_object().key("properties").begin_object().key("name").string(hit.name);
        if (country) {
            writer.key("country").string(*country);
        }
        writer.end_object().key("geometry").begin_object().key("coordinates");
        writer.begin_array().number(hit.lon).number(hit.lat).end_array().end_object().end_object();
    }
    writer.end_array().end_object();
    return writer.take();
}

// 02 §9 prints the synthetic points rounded; its distances come from the full
// OpenStreetMap coordinates used below (London 51.5074456, -0.1277653 as in
// geo_photon_london.json; Vienna 48.2083537, 16.3725042; Dublin 53.3493795, -6.2605593).

TEST(PlaceResolverLive, Example_4b_Londn_backed_by_London_England_is_corrected_silently)
{
    ResolverRig rig(photon_answers(photon_body({{{"London", "", 51.5074456, -0.1277653, ""}, "United Kingdom"}})));

    const PlaceLookup result = rig.resolve("Londn", *live());

    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"London"});
    ASSERT_EQ(result.candidates.size(), 2u);
    EXPECT_EQ(result.candidates[0].pinned_airports, (Codes{"LGW", "LTN"}));
    EXPECT_EQ(result.candidates[0].source, PlaceSource::wizz_city);
    EXPECT_EQ(rounded(wizz::distance_km(result.candidates[1].location, result.candidates[0].location), 2), 0.73);
}

TEST(PlaceResolverLive, Example_8_Vienna_namesakes_prefer_the_one_with_Wizz_nearby)
{
    const GeoPoint vienna{48.2083537, 16.3725042};
    ResolverRig rig(photon_answers(photon_body({{{"Vienna", "", vienna.latitude, vienna.longitude, ""}, "Austria"},
                                                {{"Vienna", "", 38.90, -77.26, ""}, "United States"}})));

    const PlaceLookup result = rig.resolve("Vienna", *live());

    EXPECT_FALSE(result.needs_confirmation);
    ASSERT_EQ(result.candidates.size(), 2u);
    EXPECT_EQ(result.candidates[0].description, "Vienna, Austria");
    EXPECT_EQ(result.candidates[1].description, "Vienna, United States");
    const auto near_vienna = live()->airports_near(vienna, 300.0);
    Codes codes;
    std::vector<double> distances;
    for (const wizz::AreaAirport& area : near_vienna) {
        codes.push_back(area.airport->iata);
        distances.push_back(rounded(area.distance_km, 1));
    }
    EXPECT_EQ(codes, (Codes{"BTS", "BUD", "PRG", "LJU"}));
    EXPECT_EQ(distances, (std::vector<double>{62.8, 231.7, 260.5, 263.6}));
    EXPECT_TRUE(live()->airports_near({38.90, -77.26}, 300.0).empty());
}

TEST(PlaceResolverLive, Example_9_Wien_needs_confirmation_because_the_name_differs)
{
    ResolverRig rig(photon_answers(photon_body({{{"Vienna", "", 48.2083537, 16.3725042, ""}, "Austria"}})));
    ASSERT_EQ(live()->fuzzy_correct("Wien"), std::nullopt);

    const PlaceLookup result = rig.resolve("Wien", *live());

    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].description, "Vienna, Austria");
    EXPECT_TRUE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
}

TEST(PlaceResolverLive, Example_10b_online_Dublin_is_an_exact_hit_with_Liverpool_nearby)
{
    const GeoPoint dublin{53.3493795, -6.2605593};
    ResolverRig rig(photon_answers(photon_body({{{"Dublin", "", dublin.latitude, dublin.longitude, ""}, "Ireland"}})));

    const PlaceLookup result = rig.resolve("Dublin", *live());

    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].description, "Dublin, Ireland");
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value()) << "an exact hit skips the typo step";
    const auto nearest = live()->airports_near(dublin, 300.0);
    ASSERT_EQ(nearest.size(), 1u);
    EXPECT_EQ(nearest[0].airport->iata, "LPL");
    EXPECT_EQ(std::round(nearest[0].distance_km), 226.0);
}

TEST(PlaceResolverLive, Example_11_online_STN_is_looked_up_as_a_place_name)
{
    ResolverRig rig(
        photon_answers(R"({"features":[{"properties":{"name":"Mosonszentmiklós","state":"Győr-Moson-Sopron","country":"Hungary"},"geometry":{"coordinates":[17.4289,47.7271]}}]})"));

    const PlaceLookup result = rig.resolve("STN", *live());

    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].description, "Mosonszentmiklós, Győr-Moson-Sopron, Hungary");
    EXPECT_TRUE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_EQ(rig.transport->urls(), Codes{photon("STN")});
}

} // namespace
