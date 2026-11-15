// PlaceResolver and the geocoding helpers (02 §6, §10; 05 §3.9 CT-GEOC-01..10,
// §7.1 P-23..P-25, DECISIONS D-23). Fixture catalog CAT (map_subset.json) under
// en-US; the FIXED clock 2026-11-02T10:00:00Z and a recording Nominatim delay
// unless a case says SYSTEM; the cache is disabled unless a test supplies a
// temporary directory (05 §1.5).

#include "wizz/services/place_resolver.hpp"

#include "wizz/core/catalog.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/locale.hpp"

#include "services/place_resolver_harness.hpp"
#include "support/fake_transport.hpp"
#include "support/fixtures.hpp"
#include "support/recording_delay.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;

using wizz::Airport;
using wizz::AirportCatalog;
using wizz::AirportCatalogPtr;
using wizz::ClientNotice;
using wizz::Duration;
using wizz::FileCache;
using wizz::FixedClock;
using wizz::GeoPoint;
using wizz::OperationCancelled;
using wizz::Place;
using wizz::PlaceLookup;
using wizz::PlaceResolver;
using wizz::PlaceSource;
using wizz::SystemClock;
using wizz::UtcInstant;
using wizz::geocoding::GeocodeCacheEntry;
using wizz::geocoding::PlaceDto;
using wizz::test::FakeTransport;
using wizz::test::RecordedRequest;
using wizz::test::RecordingDelay;
using wizz::test::ScriptedResponse;
using wizz::test::json_response;
using wizz::test::read_fixture;
using namespace wizz::test::geocoder;

namespace geocoding = wizz::geocoding;
namespace http = wizz::http;
namespace json = wizz::json;

using Codes = std::vector<std::string>;

/// CAT (05 §0.3): the catalog of map_subset.json under en-US.
[[nodiscard]] const AirportCatalogPtr& cat()
{
    static const AirportCatalogPtr catalog =
        AirportCatalog::from_map_json(read_fixture("map_subset.json"), wizz::Locale::en_us());
    return catalog;
}

[[nodiscard]] const Airport& station(std::string_view code)
{
    const wizz::AirportPtr airport = cat()->find(code);
    if (!airport) {
        throw std::logic_error("fixture station missing");
    }
    return *airport;
}

[[nodiscard]] Codes sorted(Codes codes)
{
    std::ranges::sort(codes);
    return codes;
}

// 05 §2.7 inline Photon answers.
constexpr std::string_view kPhOnt =
    R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"osm_value":"city","name":"London","state":"Ontario","country":"Canada"},"geometry":{"type":"Point","coordinates":[-81.2453,42.9849]}}]})";
constexpr std::string_view kPhBcn =
    R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"osm_value":"city","name":"Barcelona","state":"Catalonia","country":"Spain"},"geometry":{"type":"Point","coordinates":[2.1774,41.3826]}}]})";
constexpr std::string_view kPhVen =
    R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"osm_value":"town","name":"Barcelona","country":"Venezuela"},"geometry":{"type":"Point","coordinates":[-64.69,10.13]}}]})";
constexpr std::string_view kPhVie =
    R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"osm_value":"city","name":"Vienna","country":"Austria"},"geometry":{"type":"Point","coordinates":[16.37,48.21]}},{"type":"Feature","properties":{"osm_value":"town","name":"Vienna","state":"Virginia","country":"United States"},"geometry":{"type":"Point","coordinates":[-77.26,38.90]}}]})";
/// P-23: Photon finds nothing.
constexpr std::string_view kNoFeatures = R"({"type":"FeatureCollection","features":[]})";

/// A synthetic Photon city feature.
struct Feature {
    std::string name;
    double lat = 0.0;
    double lon = 0.0;
    std::optional<std::string> state;
    std::optional<std::string> country;
};

/// A Photon FeatureCollection built from `features` (coordinates written as [lon, lat]).
[[nodiscard]] std::string photon_body(std::initializer_list<Feature> features)
{
    json::Writer writer;
    writer.begin_object().key("type").string("FeatureCollection").key("features").begin_array();
    for (const Feature& feature : features) {
        writer.begin_object().key("type").string("Feature").key("properties").begin_object();
        writer.key("osm_value").string("city").key("name").string(feature.name);
        if (feature.state) {
            writer.key("state").string(*feature.state);
        }
        if (feature.country) {
            writer.key("country").string(*feature.country);
        }
        writer.end_object().key("geometry").begin_object().key("type").string("Point").key("coordinates");
        writer.begin_array().number(feature.lon).number(feature.lat).end_array().end_object().end_object();
    }
    writer.end_array().end_object();
    return writer.take();
}

/// Field-by-field Place check (02 §2.10).
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

void expect_same_place(const Place& actual, const Place& expected)
{
    expect_place(actual, expected.name, expected.description, expected.location, expected.source,
                 expected.pinned_airports);
}

/// 100 ns ticks of a decimal number of seconds.
[[nodiscard]] Duration seconds(double value)
{
    return Duration{std::llround(value * 1e7)};
}

/// A geocode.json body holding `entries`, as the resolver writes it.
[[nodiscard]] std::string cache_file(std::initializer_list<GeocodeCacheEntry> entries)
{
    return geocoding::serialize_geocode_cache(std::vector<GeocodeCacheEntry>(entries));
}

const PlaceDto kGrazDto{"Graz", "Graz, Styria, Austria", 47.0708678, 15.4382786, "city"};
const PlaceDto kViennaDto{"Vienna", "Vienna, Austria", 48.21, 16.37, "city"};

// ===========================================================================
// 05 §3.9 — CT-GEOC-01..10
// ===========================================================================

TEST(PlaceResolver, CT_GEOC_01_Wizz_names_resolve_offline)
{
    ResolverRig rig(photon_responder([](const RecordedRequest&) -> ScriptedResponse {
        throw std::logic_error("must not be called");
    }));

    const PlaceLookup result = rig.resolve("London", *cat());

    // [A]
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].source, PlaceSource::wizz_city);
    EXPECT_EQ(rig.transport->request_count(), 0u);
    // [D] the candidate equals CT-CAT-04; no confirmation.
    expect_same_place(result.candidates[0], *cat()->match_place("London"));
    expect_place(result.candidates[0], "London", "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN",
                 station("LON").location, PlaceSource::wizz_city, {"LGW", "LTN"});
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_TRUE(rig.log.notices().empty());
}

TEST(PlaceResolver, CT_GEOC_02_Geocoded_city_matching_the_typed_name_needs_no_confirmation)
{
    ResolverRig rig(photon_answers(read_fixture("geo_photon_graz.json")));

    const PlaceLookup result = rig.resolve("Graz", *cat());

    // [A]
    EXPECT_FALSE(result.needs_confirmation);
    ASSERT_FALSE(result.candidates.empty());
    EXPECT_EQ(result.candidates[0].name, "Graz");
    EXPECT_GE(result.candidates[0].location.latitude, 47.0);
    EXPECT_LE(result.candidates[0].location.latitude, 47.1);
    // [D] three candidates; the two Grazacs are about 61 km apart, so both are kept.
    ASSERT_EQ(result.candidates.size(), 3u);
    expect_place(result.candidates[0], "Graz", "Graz, Styria, Austria", {47.0708678, 15.4382786},
                 PlaceSource::geocoder, {});
    expect_place(result.candidates[1], "Grazac", "Grazac, Occitania, France", {43.3115399, 1.4559759},
                 PlaceSource::geocoder, {});
    expect_place(result.candidates[2], "Grazac", "Grazac, Occitania, France", {43.8429722, 1.6573297},
                 PlaceSource::geocoder, {});
    EXPECT_NEAR(wizz::distance_km(result.candidates[1].location, result.candidates[2].location), 61.28, 0.01);
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_FALSE(result.lookup_failed);
    // [D] one URL, the Photon one with q=Graz, and its notice.
    EXPECT_EQ(rig.transport->urls(), Codes{photon("Graz")});
    ASSERT_EQ(rig.log.notices().size(), 1u);
    EXPECT_EQ(rig.log.notices()[0], (ClientNotice{photon_notice("Graz"), std::nullopt}));
    // [N] exactly one same-name candidate has a Wizz airport within 300 km (BTS at 181 km).
    const auto near_graz = cat()->airports_near(result.candidates[0].location, 300.0);
    ASSERT_FALSE(near_graz.empty());
    EXPECT_EQ(near_graz[0].airport->iata, "BTS");
    EXPECT_NEAR(near_graz[0].distance_km, 180.58, 0.01);
}

TEST(PlaceResolver, CT_GEOC_03_City_with_a_lone_named_airport_is_geocoded_and_the_airport_pinned)
{
    ResolverRig rig(photon_answers(std::string{kPhBcn}));

    const PlaceLookup result = rig.resolve("Barcelona", *cat());

    // [A]
    ASSERT_FALSE(result.candidates.empty());
    EXPECT_EQ(result.candidates[0].source, PlaceSource::geocoder);
    EXPECT_EQ(result.candidates[0].pinned_airports, Codes{"BCN"});
    const double km = wizz::distance_km(result.candidates[0].location, station("BCN").location);
    EXPECT_GE(km, 5.0);
    EXPECT_LE(km, 20.0);
    EXPECT_NEAR(km, 12.61, 0.01);
    // [D] the only candidate; no confirmation.
    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Barcelona", "Barcelona, Catalonia, Spain", {41.3826, 2.1774},
                 PlaceSource::geocoder, {"BCN"});
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_EQ(rig.transport->urls(), Codes{photon("Barcelona")});
}

TEST(PlaceResolver, CT_GEOC_04_Typo_is_corrected_to_the_wizz_city_instead_of_a_far_away_namesake)
{
    ResolverRig rig(photon_answers(std::string{kPhOnt}));

    const PlaceLookup result = rig.resolve("Londn", *cat());

    // [A]
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"London"});
    EXPECT_TRUE(result.needs_confirmation);
    ASSERT_EQ(result.candidates.size(), 2u);
    EXPECT_EQ(sorted(result.candidates[0].pinned_airports), (Codes{"LGW", "LTN"}));
    // [D]
    expect_place(result.candidates[0], "London",
                 "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN (from \u201CLondn\u201D)", station("LON").location,
                 PlaceSource::wizz_city, {"LGW", "LTN"});
    expect_place(result.candidates[1], "London", "London, Ontario, Canada", {42.9849, -81.2453},
                 PlaceSource::geocoder, {});
    EXPECT_FALSE(result.lookup_failed);
    // [N] Ontario is far beyond 150 km, so the geocoder does not agree.
    EXPECT_GT(wizz::distance_km(result.candidates[1].location, station("LON").location), 5800.0);
    EXPECT_EQ(rig.transport->urls(), Codes{photon("Londn")});
    EXPECT_EQ(rig.log.messages(), Codes{photon_notice("Londn")});
}

TEST(PlaceResolver, CT_GEOC_05_Typo_correction_backed_by_the_geocoder_is_applied_silently)
{
    ResolverRig rig(photon_answers(read_fixture("geo_photon_london.json")));

    const PlaceLookup result = rig.resolve("Londn", *cat());

    // [A]
    EXPECT_FALSE(result.needs_confirmation);
    ASSERT_FALSE(result.candidates.empty());
    EXPECT_EQ(sorted(result.candidates[0].pinned_airports), (Codes{"LGW", "LTN"}));
    // [D] the fire-boundary London was de-duplicated.
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"London"});
    ASSERT_EQ(result.candidates.size(), 3u);
    expect_place(result.candidates[0], "London",
                 "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN (from \u201CLondn\u201D)", station("LON").location,
                 PlaceSource::wizz_city, {"LGW", "LTN"});
    expect_place(result.candidates[1], "London", "London, England, United Kingdom", {51.5074456, -0.1277653},
                 PlaceSource::geocoder, {});
    expect_place(result.candidates[2], "London", "London, Ontario, Canada", {42.9836747, -81.2496068},
                 PlaceSource::geocoder, {});
    EXPECT_NEAR(wizz::distance_km(result.candidates[1].location, station("LON").location), 0.7295, 0.0001);
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_EQ(rig.transport->urls(), Codes{photon("Londn")});
}

TEST(PlaceResolver, CT_GEOC_06_Offline_typo_correction_is_only_a_suggestion)
{
    // Clock SYSTEM; every request throws a transport error; cache disabled.
    const auto transport = std::make_shared<FakeTransport>(offline);
    PlaceResolver resolver(transport, FileCache::disabled(), std::make_shared<SystemClock>());
    NoticeLog log;

    const PlaceLookup result = resolver.resolve("Budpest", *cat(), log.sink(), {});

    // [A]
    EXPECT_TRUE(result.needs_confirmation);
    ASSERT_FALSE(result.candidates.empty());
    EXPECT_EQ(result.candidates[0].pinned_airports, Codes{"BUD"});
    // [D]
    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Budapest", "Budapest (BUD), Hungary (from \u201CBudpest\u201D)",
                 station("BUD").location, PlaceSource::wizz_airport, {"BUD"});
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"Budapest"});
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_EQ(transport->urls(), (Codes{photon("Budpest"), nominatim("Budpest")}));
    EXPECT_EQ(log.messages(), (Codes{photon_notice("Budpest"), nominatim_notice("Budpest")}));
}

TEST(PlaceResolver, CT_GEOC_07_Station_named_city_ignores_far_away_namesakes)
{
    ResolverRig rig(photon_answers(std::string{kPhVen}));

    const PlaceLookup result = rig.resolve("Barcelona", *cat());

    // [A]
    EXPECT_FALSE(result.needs_confirmation);
    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].pinned_airports, Codes{"BCN"});
    // [D] the PrefixPlace of CT-CAT-16 (WizzCity at the BCN airport); Venezuela is dropped.
    expect_same_place(result.candidates[0], *cat()->prefix_place("Barcelona"));
    expect_place(result.candidates[0], "Barcelona", "Barcelona, Spain \u2014 Wizz Air airports: BCN",
                 {41.29694444444444, 2.0783333333333336}, PlaceSource::wizz_city, {"BCN"});
    EXPECT_NEAR(wizz::distance_km({10.13, -64.69}, station("BCN").location), 7331.0, 1.0);
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_EQ(rig.transport->urls(), Codes{photon("Barcelona")});
}

TEST(PlaceResolver, CT_GEOC_08_Namesakes_resolve_to_the_one_with_a_wizz_airport_nearby)
{
    ResolverRig rig(photon_answers(std::string{kPhVie}));

    const PlaceLookup result = rig.resolve("Vienna", *cat());

    // [A]
    EXPECT_FALSE(result.needs_confirmation);
    ASSERT_FALSE(result.candidates.empty());
    EXPECT_NE(result.candidates[0].description.find("Austria"), std::string::npos);
    // [D] blank parts are skipped in the description.
    ASSERT_EQ(result.candidates.size(), 2u);
    expect_place(result.candidates[0], "Vienna", "Vienna, Austria", {48.21, 16.37}, PlaceSource::geocoder, {});
    expect_place(result.candidates[1], "Vienna", "Vienna, Virginia, United States", {38.90, -77.26},
                 PlaceSource::geocoder, {});
    EXPECT_NEAR(wizz::distance_km({48.21, 16.37}, station("BTS").location), 62.95, 0.01);
    EXPECT_TRUE(cat()->airports_near({38.90, -77.26}, 300.0).empty());
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_FALSE(result.lookup_failed);
}

TEST(PlaceResolver, CT_GEOC_09_Different_name_from_geocoder_asks_for_confirmation)
{
    ResolverRig rig(photon_answers(std::string{kPhBcn}));

    const PlaceLookup result = rig.resolve("Barna", *cat());

    // [A]
    EXPECT_TRUE(result.needs_confirmation);
    // [D]
    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Barcelona", "Barcelona, Catalonia, Spain", {41.3826, 2.1774},
                 PlaceSource::geocoder, {});
    EXPECT_EQ(cat()->fuzzy_correct("Barna"), std::nullopt);
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_FALSE(result.lookup_failed);
}

TEST(PlaceResolver, CT_GEOC_10_Part_1_Photon_down_but_Nominatim_answering_is_not_a_lookup_failure)
{
    ResolverRig rig(photon_responder(offline));

    const PlaceLookup result = rig.resolve("Graz", *cat());

    // [A]
    EXPECT_TRUE(result.candidates.empty());
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_FALSE(result.needs_confirmation);
    // [D] URLs: Photon q=Graz, then Nominatim q=Graz.
    EXPECT_EQ(rig.transport->urls(), (Codes{photon("Graz"), nominatim("Graz")}));
    EXPECT_EQ(rig.log.messages(), (Codes{photon_notice("Graz"), nominatim_notice("Graz")}));
}

TEST(PlaceResolver, CT_GEOC_10_Part_2_Offline_is_a_lookup_failure_but_prefix_names_still_work)
{
    // Every call throws a transport error; Clock SYSTEM; the real Nominatim wait.
    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> nominatim_sent;
    const auto transport = std::make_shared<FakeTransport>([&](const RecordedRequest& request) -> ScriptedResponse {
        if (request.url.find("nominatim") != std::string::npos) {
            const std::scoped_lock lock(mutex);
            nominatim_sent.push_back(std::chrono::steady_clock::now());
        }
        throw http::TransportError("offline");
    });
    PlaceResolver resolver(transport, FileCache::disabled(), std::make_shared<SystemClock>());

    // [A]
    const PlaceLookup graz = resolver.resolve("Graz", *cat(), {}, {});
    EXPECT_TRUE(graz.lookup_failed);
    EXPECT_TRUE(graz.candidates.empty());
    EXPECT_FALSE(graz.needs_confirmation);

    // [A] / [D] the PrefixPlace (WizzCity) of CT-CAT-16.
    const PlaceLookup barcelona = resolver.resolve("Barcelona", *cat(), {}, {});
    ASSERT_EQ(barcelona.candidates.size(), 1u);
    EXPECT_EQ(barcelona.candidates[0].pinned_airports, Codes{"BCN"});
    expect_same_place(barcelona.candidates[0], *cat()->prefix_place("Barcelona"));
    EXPECT_EQ(barcelona.candidates[0].source, PlaceSource::wizz_city);
    EXPECT_FALSE(barcelona.needs_confirmation);
    EXPECT_FALSE(barcelona.lookup_failed);

    EXPECT_EQ(transport->urls(),
              (Codes{photon("Graz"), nominatim("Graz"), photon("Barcelona"), nominatim("Barcelona")}));
    // [D] about 1.1 s of real time: the second Nominatim call waited for its slot.
    ASSERT_EQ(nominatim_sent.size(), 2u);
    EXPECT_GE(nominatim_sent[1] - nominatim_sent[0], 1090ms);
}

// ===========================================================================
// 05 §7.1 — P-23, P-24, P-25
// ===========================================================================

[[nodiscard]] wizz::test::Responder nominatim_graz_responder()
{
    return [nominatim = read_fixture("geo_nominatim_graz.json")](const RecordedRequest& request) {
        if (request.url.find("photon") != std::string::npos) {
            return json_response(std::string{kNoFeatures});
        }
        return json_response(nominatim);
    };
}

TEST(PlaceResolver, P_23_Nominatim_answers_when_Photon_finds_nothing)
{
    ResolverRig rig(nominatim_graz_responder());

    const PlaceLookup result = rig.resolve("Graz", *cat());

    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Graz", "Graz, Styria, Austria", {47.0708678, 15.4382786},
                 PlaceSource::geocoder, {});
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_EQ(rig.transport->urls(), (Codes{photon("Graz"), nominatim("Graz")}));
    EXPECT_EQ(rig.log.messages(), (Codes{photon_notice("Graz"), nominatim_notice("Graz")}));
    EXPECT_TRUE(rig.delay.delays().empty()) << "the first Nominatim call never waits";
}

TEST(PlaceResolver, P_24_Geocode_memo_is_keyed_by_the_normalised_text)
{
    ResolverRig rig(nominatim_graz_responder());
    (void)rig.resolve("Graz", *cat());  // P-23
    ASSERT_EQ(rig.transport->request_count(), 2u);

    const PlaceLookup again = rig.resolve(" GRAZ ", *cat());

    EXPECT_EQ(rig.transport->request_count(), 2u) << "no new HTTP: the key is normalised";
    ASSERT_EQ(again.candidates.size(), 1u);
    EXPECT_EQ(again.candidates[0].name, "Graz");
    EXPECT_FALSE(again.needs_confirmation);
    EXPECT_EQ(rig.log.notices().size(), 2u) << "a memo hit reports nothing";
}

TEST(PlaceResolver, P_24_Only_non_empty_results_are_remembered)
{
    ResolverRig rig(photon_answers(std::string{kNoFeatures}));

    EXPECT_TRUE(rig.resolve("Graz", *cat()).candidates.empty());
    EXPECT_TRUE(rig.resolve("Graz", *cat()).candidates.empty());

    EXPECT_EQ(rig.transport->urls(),
              (Codes{photon("Graz"), nominatim("Graz"), photon("Graz"), nominatim("Graz")}));
}

TEST(PlaceResolver, P_25_Text_that_is_too_short_is_not_looked_up)
{
    ResolverRig rig(photon_answers(std::string{kPhBcn}));

    const PlaceLookup result = rig.resolve("a", *cat());

    EXPECT_TRUE(result.candidates.empty());
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_EQ(rig.transport->request_count(), 0u);
}

TEST(PlaceResolver, S2_counts_the_name_key_in_UTF_16_units)
{
    ResolverRig rig(photon_answers(std::string{kNoFeatures}));

    // 02 §9 example 15, and keys that shrink to one unit after normalisation.
    for (const std::string_view text : {"x"sv, "  a  "sv, "é"sv, "e\xCC\x81"sv, ""sv, "   "sv}) {
        SCOPED_TRACE(text);
        const PlaceLookup result = rig.resolve(text, *cat());
        EXPECT_TRUE(result.candidates.empty());
        EXPECT_FALSE(result.lookup_failed);
    }
    EXPECT_EQ(rig.transport->request_count(), 0u);

    // Two units are enough: "ab", and a supplementary letter (a surrogate pair).
    (void)rig.resolve("ab", *cat());
    (void)rig.resolve("\xF0\x90\x90\x80", *cat());
    EXPECT_EQ(rig.transport->request_count(), 4u);
}

// ===========================================================================
// 02 §6.2 — decision-tree details
// ===========================================================================

TEST(PlaceResolver, S4_pinned_hits_come_first_and_the_rest_keep_geocoder_order)
{
    // Photon ranks a far namesake first; the hit near BCN is pinned and moves to the front.
    ResolverRig rig(photon_answers(photon_body({
        {"Barcelona", 10.13, -64.69, std::nullopt, "Venezuela"},
        {"Barcelona", 41.3826, 2.1774, "Catalonia", "Spain"},
        {"Barcelona", -5.95, -35.93, "Rio Grande do Norte", "Brazil"},
    })));

    const PlaceLookup result = rig.resolve("Barcelona", *cat());

    ASSERT_EQ(result.candidates.size(), 3u);
    expect_place(result.candidates[0], "Barcelona", "Barcelona, Catalonia, Spain", {41.3826, 2.1774},
                 PlaceSource::geocoder, {"BCN"});
    EXPECT_EQ(result.candidates[1].description, "Barcelona, Venezuela");
    EXPECT_EQ(result.candidates[2].description, "Barcelona, Rio Grande do Norte, Brazil");
    EXPECT_TRUE(result.candidates[1].pinned_airports.empty());
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_FALSE(result.corrected_text.has_value());
}

TEST(PlaceResolver, S4_station_named_text_never_reports_a_lookup_failure)
{
    ResolverRig rig(offline);

    const PlaceLookup result = rig.resolve("Alghero", *cat());

    ASSERT_EQ(result.candidates.size(), 1u);
    expect_same_place(result.candidates[0], *cat()->prefix_place("Alghero"));
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_EQ(rig.transport->request_count(), 2u);
}

TEST(PlaceResolver, S4_places_minus_near_is_a_distinct_set_difference)
{
    // Dedupe cannot remove repeats whose distance is NaN; LINQ Except still collapses
    // identical unpinned records (double.Equals treats NaN as equal to NaN).
    ResolverRig rig([](const RecordedRequest& request) {
        if (request.url.find("photon") != std::string::npos) {
            return json_response(std::string{kNoFeatures});
        }
        return json_response(
            R"([{"lat":"41.3826","lon":"2.1774","name":"Barcelona","display_name":"Barcelona, Catalonia, Spain"},)"
            R"({"lat":"NaN","lon":"NaN","name":"Barcelona","display_name":"Barcelona, Nowhere"},)"
            R"({"lat":"NaN","lon":"NaN","name":"Barcelona","display_name":"Barcelona, Nowhere"},)"
            R"({"lat":"NaN","lon":"NaN","name":"Barcelona","display_name":"Barcelona, Elsewhere"}])");
    });

    const PlaceLookup result = rig.resolve("Barcelona", *cat());

    ASSERT_EQ(result.candidates.size(), 3u);
    EXPECT_EQ(result.candidates[0].pinned_airports, Codes{"BCN"});
    EXPECT_EQ(result.candidates[1].description, "Barcelona, Nowhere");
    EXPECT_TRUE(std::isnan(result.candidates[1].location.latitude));
    EXPECT_EQ(result.candidates[2].description, "Barcelona, Elsewhere");
}

TEST(PlaceResolver, S4_pins_stations_within_150_km_inclusive)
{
    const GeoPoint bcn = station("BCN").location;
    const GeoPoint inside = north_of(bcn, 149.9);
    const GeoPoint outside = north_of(bcn, 150.1);
    ASSERT_LT(wizz::distance_km(inside, bcn), 150.0);
    ASSERT_GT(wizz::distance_km(outside, bcn), 150.0);

    const std::vector<PlaceDto> found{{"Inside", "Inside", inside.latitude, inside.longitude, ""},
                                      {"Outside", "Outside", outside.latitude, outside.longitude, ""}};
    const std::vector<Place> places = geocoding::with_pinned(found, *cat(), "Barcelona");

    ASSERT_EQ(places.size(), 2u);
    EXPECT_EQ(places[0].pinned_airports, Codes{"BCN"});
    EXPECT_TRUE(places[1].pinned_airports.empty());
    expect_place(places[0], "Inside", "Inside", inside, PlaceSource::geocoder, {"BCN"});
}

TEST(PlaceResolver, S6_corrected_name_is_geocoded_when_it_is_not_a_wizz_place)
{
    // FuzzyCorrect("Barcelna") = "Barcelona", which MatchPlace does not know; the
    // second lookup's first pinned hit (a Geocoder place) becomes the correction.
    ResolverRig rig(photon_responder([](const RecordedRequest& request) {
        if (request.url.find("q=Barcelona&") != std::string::npos) {
            return json_response(std::string{kPhBcn});
        }
        return json_response(std::string{kNoFeatures});
    }));

    const PlaceLookup result = rig.resolve("Barcelna", *cat());

    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Barcelona", "Barcelona, Catalonia, Spain (from \u201CBarcelna\u201D)",
                 {41.3826, 2.1774}, PlaceSource::geocoder, {"BCN"});
    EXPECT_TRUE(result.needs_confirmation) << "no geocoder candidate can agree";
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"Barcelona"});
    EXPECT_FALSE(result.lookup_failed);
    EXPECT_EQ(rig.transport->urls(), (Codes{photon("Barcelna"), nominatim("Barcelna"), photon("Barcelona")}));
    EXPECT_EQ(rig.log.messages(),
              (Codes{photon_notice("Barcelna"), nominatim_notice("Barcelna"), photon_notice("Barcelona")}));
}

TEST(PlaceResolver, S6_second_lookup_without_a_pinned_hit_falls_back_to_the_prefix_place)
{
    ResolverRig rig(photon_responder([](const RecordedRequest& request) {
        if (request.url.find("q=Barcelona&") != std::string::npos) {
            return json_response(std::string{kPhVen});
        }
        return json_response(std::string{kNoFeatures});
    }));

    const PlaceLookup result = rig.resolve("Barcelna", *cat());

    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Barcelona", "Barcelona, Spain \u2014 Wizz Air airports: BCN (from \u201CBarcelna\u201D)",
                 station("BCN").location, PlaceSource::wizz_city, {"BCN"});
    EXPECT_TRUE(result.needs_confirmation);
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"Barcelona"});
}

TEST(PlaceResolver, S6_geocoder_agreeing_with_the_correction_is_applied_silently)
{
    // Photon answers both queries with Barcelona, Spain: the first lookup's hit lies
    // 0 km from the corrected place, so no confirmation is needed.
    ResolverRig rig(photon_answers(std::string{kPhBcn}));

    const PlaceLookup result = rig.resolve("Barcelna", *cat());

    ASSERT_EQ(result.candidates.size(), 2u);
    expect_place(result.candidates[0], "Barcelona", "Barcelona, Catalonia, Spain (from \u201CBarcelna\u201D)",
                 {41.3826, 2.1774}, PlaceSource::geocoder, {"BCN"});
    expect_place(result.candidates[1], "Barcelona", "Barcelona, Catalonia, Spain", {41.3826, 2.1774},
                 PlaceSource::geocoder, {});
    EXPECT_FALSE(result.needs_confirmation);
    EXPECT_EQ(result.corrected_text, std::optional<std::string>{"Barcelona"});
    EXPECT_EQ(rig.transport->urls(), (Codes{photon("Barcelna"), photon("Barcelona")}));
}

TEST(PlaceResolver, S6_agreement_needs_a_candidate_strictly_within_150_km)
{
    const GeoPoint lon = station("LON").location;
    for (const auto& [km, agrees] : {std::pair{149.9, true}, std::pair{150.1, false}}) {
        SCOPED_TRACE(km);
        const GeoPoint hit = north_of(lon, km);
        ASSERT_EQ(wizz::distance_km(hit, lon) < 150.0, agrees);
        ResolverRig rig(photon_answers(photon_body({{"Somewhere", hit.latitude, hit.longitude, {}, "Nowhere"}})));

        const PlaceLookup result = rig.resolve("Londn", *cat());

        ASSERT_EQ(result.candidates.size(), 2u);
        EXPECT_EQ(result.candidates[0].pinned_airports, (Codes{"LGW", "LTN"}));
        EXPECT_EQ(result.candidates[1].description, "Somewhere, Nowhere");
        EXPECT_EQ(result.needs_confirmation, !agrees);
        EXPECT_EQ(result.corrected_text, std::optional<std::string>{"London"});
    }
}

TEST(PlaceResolver, S7_namesake_with_a_wizz_airport_within_300_km_wins)
{
    // "Vlora": no Wizz name, no typo correction. Only TIA lies within 300 km of a point
    // 299.9 km south of it; 300.1 km south nothing does.
    const GeoPoint tia = station("TIA").location;
    const GeoPoint near_point = north_of(tia, -299.9);
    const GeoPoint far_point = north_of(tia, -300.1);
    ASSERT_EQ(cat()->airports_near(near_point, 300.0).size(), 1u);
    ASSERT_TRUE(cat()->airports_near(far_point, 300.0).empty());
    ASSERT_EQ(cat()->fuzzy_correct("Vlora"), std::nullopt);

    {
        SCOPED_TRACE("the winner moves to the front");
        ResolverRig rig(photon_answers(photon_body({{"Vlora", 0.0, 0.0, {}, "Nowhere"},
                                                    {"Vlora", near_point.latitude, near_point.longitude, {}, "Albania"}})));
        const PlaceLookup result = rig.resolve("Vlora", *cat());
        ASSERT_EQ(result.candidates.size(), 2u);
        EXPECT_EQ(result.candidates[0].description, "Vlora, Albania");
        EXPECT_EQ(result.candidates[1].description, "Vlora, Nowhere");
        EXPECT_FALSE(result.needs_confirmation);
    }
    {
        SCOPED_TRACE("no namesake within 300 km: two same-name candidates need confirmation");
        ResolverRig rig(photon_answers(photon_body({{"Vlora", 0.0, 0.0, {}, "Nowhere"},
                                                    {"Vlora", far_point.latitude, far_point.longitude, {}, "Albania"}})));
        const PlaceLookup result = rig.resolve("Vlora", *cat());
        ASSERT_EQ(result.candidates.size(), 2u);
        EXPECT_EQ(result.candidates[0].description, "Vlora, Nowhere");
        EXPECT_TRUE(result.needs_confirmation);
    }
    {
        SCOPED_TRACE("two namesakes with Wizz nearby: no winner, the user chooses");
        const GeoPoint closer = north_of(tia, -100.0);
        ResolverRig rig(photon_answers(photon_body({{"Vlora", near_point.latitude, near_point.longitude, {}, "Far"},
                                                    {"Vlora", closer.latitude, closer.longitude, {}, "Close"}})));
        const PlaceLookup result = rig.resolve("Vlora", *cat());
        ASSERT_EQ(result.candidates.size(), 2u);
        EXPECT_EQ(result.candidates[0].description, "Vlora, Far");
        EXPECT_EQ(result.candidates[1].description, "Vlora, Close");
        EXPECT_TRUE(result.needs_confirmation);
    }
    {
        SCOPED_TRACE("one exact hit without Wizz nearby needs no confirmation");
        ResolverRig rig(photon_answers(photon_body({{"Vlora", 0.0, 0.0, {}, "Nowhere"}})));
        const PlaceLookup result = rig.resolve("Vlora", *cat());
        ASSERT_EQ(result.candidates.size(), 1u);
        EXPECT_FALSE(result.needs_confirmation);
    }
    {
        SCOPED_TRACE("only the first hit decides exactHit");
        ResolverRig rig(photon_answers(photon_body({{"Vlorë", 0.0, 0.0, {}, "Nowhere"},
                                                    {"Vlora", near_point.latitude, near_point.longitude, {}, "Albania"}})));
        const PlaceLookup result = rig.resolve("Vlora", *cat());
        ASSERT_EQ(result.candidates.size(), 2u);
        EXPECT_EQ(result.candidates[0].name, "Vlorë");
        EXPECT_TRUE(result.needs_confirmation);
    }
}

TEST(PlaceResolver, Fuzzy_correction_is_not_tried_after_an_exact_hit)
{
    // "Londo" would be corrected to London, but Photon's first hit is named "Londo".
    ResolverRig rig(photon_answers(photon_body({{"Londo", 0.0, 0.0, {}, "Nowhere"}})));
    ASSERT_EQ(cat()->fuzzy_correct("Londo"), std::optional<std::string>{"London"});

    const PlaceLookup result = rig.resolve("Londo", *cat());

    ASSERT_EQ(result.candidates.size(), 1u);
    EXPECT_EQ(result.candidates[0].name, "Londo");
    EXPECT_FALSE(result.corrected_text.has_value());
    EXPECT_FALSE(result.needs_confirmation);
}

// ===========================================================================
// 02 §6.4 / §6.5 — requests, notices, failures, cancellation
// ===========================================================================

/// Captures the full requests (FakeTransport does not record timeouts).
class CapturingTransport final : public http::Transport {
public:
    http::Response send(const http::Request& request, std::stop_token /*stop*/) override
    {
        requests.push_back(request);
        return json_response("[]").response;  // Photon: wrong shape (fails) → Nominatim: nothing
    }
    std::vector<http::Request> requests;
};

TEST(PlaceResolver, Geocoder_requests_are_identified_GETs_with_15_s_timeouts)
{
    const auto transport = std::make_shared<CapturingTransport>();
    PlaceResolver resolver(transport, FileCache::disabled(), std::make_shared<FixedClock>(),
                           RecordingDelay{}.delay_func());

    (void)resolver.resolve("Graz", *cat(), {}, {});

    ASSERT_EQ(transport->requests.size(), 2u);
    const http::Headers expected_headers{{"User-Agent", "WizzairHelper/1.0 (personal desktop flight finder)"},
                                         {"Accept", "application/json"},
                                         {"Accept-Language", "en"}};
    for (const http::Request& request : transport->requests) {
        SCOPED_TRACE(request.url);
        EXPECT_EQ(request.method, "GET");
        EXPECT_EQ(request.headers, expected_headers);
        EXPECT_FALSE(request.body.has_value());
        EXPECT_FALSE(request.content_type.has_value());
        EXPECT_EQ(request.timeout, 15s);
        EXPECT_EQ(request.connect_timeout, 15s);
    }
    EXPECT_EQ(transport->requests[0].url, photon("Graz"));
    EXPECT_EQ(transport->requests[1].url, nominatim("Graz"));
    EXPECT_EQ(geocoding::kUserAgent, "WizzairHelper/1.0 (personal desktop flight finder)");
}

TEST(PlaceResolver, Query_is_the_trimmed_original_text_escaped_per_RFC_3986)
{
    ResolverRig rig(photon_answers(std::string{kNoFeatures}));

    (void)rig.resolve("  São Paulo\t", *cat());

    EXPECT_EQ(rig.transport->urls(), (Codes{photon("S%C3%A3o%20Paulo"), nominatim("S%C3%A3o%20Paulo")}));
    EXPECT_EQ(rig.log.messages(), (Codes{photon_notice("São Paulo"), nominatim_notice("São Paulo")}));
    EXPECT_EQ(geocoding::photon_url("Kraków"), photon("Krak%C3%B3w"));
    EXPECT_EQ(geocoding::nominatim_url("a b~c-d.e_f+g&h=i/j?"), nominatim("a%20b~c-d.e_f%2Bg%26h%3Di%2Fj%3F"));
    EXPECT_EQ(geocoding::photon_url("Dublin"), "https://photon.komoot.io/api/?q=Dublin&limit=6&lang=en&layer=city");
}

TEST(PlaceResolver, Nominatim_is_asked_only_when_Photon_gave_nothing)
{
    {
        SCOPED_TRACE("Photon answered");
        ResolverRig rig(photon_answers(read_fixture("geo_photon_graz.json")));
        const auto result = rig.resolver.geocode("Graz", {}, {});
        EXPECT_EQ(result.found.size(), 3u);
        EXPECT_FALSE(result.failed);
        EXPECT_EQ(rig.transport->urls(), Codes{photon("Graz")});
    }
    {
        SCOPED_TRACE("Photon had only unusable features");
        ResolverRig rig(photon_answers(
            R"({"features":[{"geometry":{"coordinates":[1]},"properties":{"name":"Graz"}},{"properties":{"name":"Graz"}}]})"));
        const auto result = rig.resolver.geocode("Graz", {}, {});
        EXPECT_TRUE(result.found.empty());
        EXPECT_FALSE(result.failed);
        EXPECT_EQ(rig.transport->urls(), (Codes{photon("Graz"), nominatim("Graz")}));
    }
    {
        SCOPED_TRACE("Photon failed, Nominatim answered");
        ResolverRig rig([nominatim = read_fixture("geo_nominatim_graz.json")](const RecordedRequest& request) {
            if (request.url.find("photon") != std::string::npos) {
                throw http::TransportError("offline");
            }
            return json_response(nominatim);
        });
        const auto result = rig.resolver.geocode("Graz", {}, {});
        EXPECT_EQ(result.found, (std::vector<PlaceDto>{kGrazDto}));
        EXPECT_FALSE(result.failed);
    }
}

TEST(PlaceResolver, Service_failures_fall_back_and_count_towards_LookupFailed)
{
    struct Case {
        std::string_view name;
        std::function<ScriptedResponse(const RecordedRequest&)> photon;
    };
    const auto answer = [](std::string body, int status = 200) {
        return [body = std::move(body), status](const RecordedRequest&) { return json_response(body, status); };
    };
    const std::vector<Case> cases{
        {"transport error", offline},
        {"timeout", [](const RecordedRequest&) -> ScriptedResponse { throw http::TimeoutError("no answer within 15 s"); }},
        {"HTTP 500", answer(std::string{kPhBcn}, 500)},
        {"HTTP 404 page", [](const RecordedRequest&) { return wizz::test::html_response("<html></html>", 404); }},
        {"HTTP 301", answer("{}", 301)},
        {"malformed JSON", answer("not json")},
        {"empty body", answer("")},
        {"root not an object", answer("[1,2,3]")},
        {"feature not an object", answer(R"({"features":[5]})")},
        {"geometry not an object", answer(R"({"features":[{"geometry":null,"properties":{"name":"x"}}]})")},
        {"coordinates not numbers",
         answer(R"({"features":[{"geometry":{"coordinates":["a","b"]},"properties":{"name":"x"}}]})")},
        {"feature without properties", answer(R"({"features":[{"geometry":{"coordinates":[1,2]}}]})")},
        {"properties not an object",
         answer(R"({"features":[{"geometry":{"coordinates":[1,2]},"properties":null}]})")},
        {"cancellation that is not the caller's",
         [](const RecordedRequest&) -> ScriptedResponse { throw OperationCancelled{}; }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        for (const bool nominatim_down : {true, false}) {
            ResolverRig rig([&](const RecordedRequest& request) {
                if (request.url.find("photon") != std::string::npos) {
                    return c.photon(request);
                }
                if (nominatim_down) {
                    throw http::TransportError("offline");
                }
                return json_response("[]");
            });
            const PlaceLookup result = rig.resolve("Graz", *cat());
            EXPECT_TRUE(result.candidates.empty());
            EXPECT_EQ(result.lookup_failed, nominatim_down);
            EXPECT_EQ(rig.transport->urls(), (Codes{photon("Graz"), nominatim("Graz")}));
            EXPECT_EQ(rig.log.messages(), (Codes{photon_notice("Graz"), nominatim_notice("Graz")}));
        }
    }
}

TEST(PlaceResolver, Nominatim_failures_are_service_failures_too)
{
    for (const std::string_view body : {"not json"sv, "[5]"sv, R"([{"lat":"1","lon":"2","name":"x"},true])"sv}) {
        SCOPED_TRACE(body);
        ResolverRig rig([&](const RecordedRequest& request) {
            if (request.url.find("photon") != std::string::npos) {
                throw http::TransportError("offline");
            }
            return json_response(std::string{body});
        });
        const auto result = rig.resolver.geocode("Graz", {}, {});
        EXPECT_TRUE(result.found.empty());
        EXPECT_TRUE(result.failed);
    }
    // A root that is not an array is simply "nothing found".
    ResolverRig rig([](const RecordedRequest& request) {
        if (request.url.find("photon") != std::string::npos) {
            throw http::TransportError("offline");
        }
        return json_response(R"({"error":"x"})");
    });
    EXPECT_FALSE(rig.resolver.geocode("Graz", {}, {}).failed);
}

TEST(PlaceResolver, Errors_that_are_not_service_failures_propagate)
{
    {
        SCOPED_TRACE("logic error from the transport");
        ResolverRig rig(photon_responder([](const RecordedRequest&) -> ScriptedResponse {
            throw std::logic_error("must not be called");
        }));
        expect_throws_exactly<std::logic_error>([&] { (void)rig.resolve("Graz", *cat()); }, "must not be called");
        EXPECT_EQ(rig.transport->urls(), Codes{photon("Graz")});
    }
    {
        SCOPED_TRACE("a coordinate that overflows a double (FormatException in .NET)");
        ResolverRig rig(photon_answers(R"({"features":[{"geometry":{"coordinates":[1e400,1]},"properties":{"name":"x"}}]})"));
        expect_throws_exactly<json::FormatError>([&] { (void)rig.resolve("Graz", *cat()); });
        EXPECT_EQ(rig.transport->urls(), Codes{photon("Graz")});
    }
    {
        SCOPED_TRACE("an unexpected error from Nominatim");
        ResolverRig rig([](const RecordedRequest& request) -> ScriptedResponse {
            if (request.url.find("photon") != std::string::npos) {
                return json_response(std::string{kNoFeatures});
            }
            throw std::runtime_error("boom");
        });
        expect_throws_exactly<std::runtime_error>([&] { (void)rig.resolve("Graz", *cat()); }, "boom");
    }
}

TEST(PlaceResolver, After_a_user_cancel_even_a_service_error_propagates_unchanged)
{
    std::stop_source cancel;
    ResolverRig rig(photon_responder([&](const RecordedRequest&) -> ScriptedResponse {
        cancel.request_stop();  // the user cancels while the request is failing
        throw http::TransportError("offline");
    }));

    expect_throws_exactly<http::TransportError>([&] { (void)rig.resolve("Graz", *cat(), cancel.get_token()); },
                                                "offline");
    EXPECT_EQ(rig.transport->urls(), Codes{photon("Graz")}) << "Nominatim is not asked after the cancel";
}

TEST(PlaceResolver, Cancelled_token_stops_geocoding_before_any_request)
{
    std::stop_source cancel;
    cancel.request_stop();
    ResolverRig rig(photon_answers(std::string{kPhBcn}));

    expect_throws_exactly<OperationCancelled>([&] { (void)rig.resolve("Graz", *cat(), cancel.get_token()); });
    expect_throws_exactly<OperationCancelled>([&] { (void)rig.resolver.geocode("Graz", {}, cancel.get_token()); });
    EXPECT_EQ(rig.transport->request_count(), 0u);

    // Offline matches and too-short texts never wait, so they still answer.
    EXPECT_EQ(rig.resolve("London", *cat(), cancel.get_token()).candidates.size(), 1u);
    EXPECT_TRUE(rig.resolve("x", *cat(), cancel.get_token()).candidates.empty());
}

TEST(PlaceResolver, Cancellation_during_a_transfer_propagates)
{
    // Photon's answer arrives as the user cancels; the fake transport then records the
    // Nominatim request and sees the cancelled token.
    std::stop_source cancel;
    const auto transport = std::make_shared<FakeTransport>([&](const RecordedRequest& request) {
        if (request.url.find("photon") != std::string::npos) {
            cancel.request_stop();
            return json_response(std::string{kNoFeatures});  // the answer arrives, then the next step stops
        }
        return json_response("[]");
    });
    PlaceResolver resolver(transport, FileCache::disabled(), std::make_shared<FixedClock>(),
                           RecordingDelay{}.delay_func());

    expect_throws_exactly<OperationCancelled>(
        [&] { (void)resolver.resolve("Graz", *cat(), {}, cancel.get_token()); });
    EXPECT_EQ(transport->urls(), (Codes{photon("Graz"), nominatim("Graz")}));
}

TEST(PlaceResolver, Waiting_for_the_geocoding_lock_is_cancellable)
{
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    ResolverRig rig(photon_responder([&](const RecordedRequest& request) {
        if (request.url.find("q=Graz&") != std::string::npos) {
            entered.set_value();
            released.wait();
        }
        return json_response(std::string{kNoFeatures});
    }));

    std::jthread holder([&] { (void)rig.resolver.resolve("Graz", *cat(), {}, {}); });
    entered.get_future().wait();

    std::stop_source cancel;
    auto waiter = std::async(std::launch::async,
                             [&] { return rig.resolver.resolve("Vienna", *cat(), {}, cancel.get_token()); });
    EXPECT_EQ(waiter.wait_for(100ms), std::future_status::timeout) << "the second lookup waits for the lock";
    cancel.request_stop();
    ASSERT_EQ(waiter.wait_for(10s), std::future_status::ready);
    expect_throws_exactly<OperationCancelled>([&] { (void)waiter.get(); });

    release.set_value();
    holder.join();
    for (const std::string& url : rig.transport->urls()) {
        EXPECT_EQ(url.find("Vienna"), std::string::npos) << url;
    }
}

TEST(PlaceResolver, Geocoding_is_serialised_per_resolver)
{
    std::atomic<int> in_flight{0};
    std::atomic<int> most{0};
    ResolverRig rig([&](const RecordedRequest&) {
        const int now = ++in_flight;
        int seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now)) {
        }
        std::this_thread::sleep_for(5ms);
        --in_flight;
        return json_response(std::string{kNoFeatures});
    });

    {
        std::vector<std::jthread> threads;
        for (const std::string_view text : {"Aaaa"sv, "Bbbb"sv, "Cccc"sv, "Dddd"sv}) {
            threads.emplace_back([&rig, text] { (void)rig.resolver.resolve(text, *cat(), {}, {}); });
        }
    }

    EXPECT_EQ(most.load(), 1);
    EXPECT_EQ(rig.transport->request_count(), 8u);
}

TEST(PlaceResolver, Notices_use_ASCII_quotes_and_the_exact_text)
{
    ResolverRig rig(photon_answers(std::string{kNoFeatures}));

    (void)rig.resolver.geocode("Kraków \u201Cx\u201D", rig.log.sink(), {});

    const std::vector<ClientNotice> notices = rig.log.notices();
    ASSERT_EQ(notices.size(), 2u);
    EXPECT_EQ(notices[0].message, "Looking up \"Kraków \u201Cx\u201D\" (Photon geocoder)");
    EXPECT_EQ(notices[1].message, "Looking up \"Kraków \u201Cx\u201D\" (OpenStreetMap Nominatim)");
    EXPECT_FALSE(notices[0].wait_until_utc.has_value());
    EXPECT_FALSE(notices[1].wait_until_utc.has_value());
}

TEST(PlaceResolver, Constructor_rejects_missing_dependencies)
{
    const auto transport = std::make_shared<FakeTransport>(offline);
    const auto clock = std::make_shared<FixedClock>();
    EXPECT_THROW((void)PlaceResolver(nullptr, FileCache::disabled(), clock), std::invalid_argument);
    EXPECT_THROW((void)PlaceResolver(transport, FileCache::disabled(), nullptr), std::invalid_argument);
    EXPECT_THROW((void)PlaceResolver(transport, FileCache::disabled(), clock, wizz::DelayFunc{}),
                 std::invalid_argument);
}

// ===========================================================================
// 02 §6.4 — Nominatim spacing (injected clock checks, injected delay waits)
// ===========================================================================

TEST(PlaceResolver, Nominatim_calls_start_at_least_1_1_s_apart)
{
    ResolverRig rig(photon_answers(std::string{kNoFeatures}));

    (void)rig.resolver.geocode("Aaaa", {}, {});
    EXPECT_TRUE(rig.delay.delays().empty()) << "the first call never waits";

    (void)rig.resolver.geocode("Bbbb", {}, {});  // the FIXED clock did not move: wait 1.1 s
    rig.clock->advance(400ms);
    (void)rig.resolver.geocode("Cccc", {}, {});  // 0.4 s since the last start: wait 0.7 s
    rig.clock->advance(1100ms);
    (void)rig.resolver.geocode("Dddd", {}, {});  // exactly 1.1 s: no wait

    EXPECT_EQ(rig.delay.delays(), (std::vector<Duration>{Duration{11'000'000}, Duration{7'000'000}}));
    EXPECT_EQ(geocoding::kNominatimSpacing, seconds(1.1));
    EXPECT_EQ(rig.transport->request_count(), 8u);
}

TEST(PlaceResolver, Nominatim_wait_never_exceeds_the_spacing_when_the_clock_goes_back)
{
    ResolverRig rig(photon_answers(std::string{kNoFeatures}));
    (void)rig.resolver.geocode("Aaaa", {}, {});

    rig.clock->set_utc_now(FixedClock::kDefaultUtcNow - std::chrono::hours{1});
    (void)rig.resolver.geocode("Bbbb", {}, {});

    EXPECT_EQ(rig.delay.delays(), std::vector<Duration>{seconds(1.1)});
}

TEST(PlaceResolver, Photon_calls_are_not_spaced)
{
    ResolverRig rig(photon_answers(read_fixture("geo_photon_graz.json")));
    for (const std::string_view text : {"Graz"sv, "Grazac"sv, "Grazzano"sv}) {
        (void)rig.resolver.geocode(text, {}, {});
    }
    EXPECT_TRUE(rig.delay.delays().empty());
    EXPECT_EQ(rig.transport->request_count(), 3u);
}

TEST(PlaceResolver, Nominatim_wait_is_cancellable_and_the_call_is_then_not_sent)
{
    std::stop_source cancel;
    RecordingDelay delay([&](Duration) { cancel.request_stop(); });
    const auto transport = std::make_shared<FakeTransport>(wizz::test::geocoder::photon_answers(std::string{kNoFeatures}));
    PlaceResolver resolver(transport, FileCache::disabled(), std::make_shared<FixedClock>(), delay.delay_func());

    (void)resolver.geocode("Aaaa", {}, cancel.get_token());
    expect_throws_exactly<OperationCancelled>([&] { (void)resolver.geocode("Bbbb", {}, cancel.get_token()); });

    EXPECT_EQ(transport->urls(), (Codes{photon("Aaaa"), nominatim("Aaaa"), photon("Bbbb")}));
    EXPECT_EQ(delay.delays(), std::vector<Duration>{seconds(1.1)});

    // The cancelled call did not take a slot: the next one still waits after the first.
    (void)resolver.geocode("Cccc", {}, {});
    EXPECT_EQ(delay.delays(), (std::vector<Duration>{seconds(1.1), seconds(1.1)}));
}

// ===========================================================================
// 02 §6.6 — ParsePhoton, ParseNominatim, JoinParts, Dedupe, ToPlace
// ===========================================================================

TEST(Geocoding, ParsePhoton_reads_the_fixtures)
{
    EXPECT_EQ(geocoding::parse_photon(read_fixture("geo_photon_graz.json")),
              (std::vector<PlaceDto>{kGrazDto,
                                     {"Grazac", "Grazac, Occitania, France", 43.3115399, 1.4559759, "village"},
                                     {"Grazac", "Grazac, Occitania, France", 43.8429722, 1.6573297, "village"}}));
    // The "fire" boundary London is 2.31 km from the first and is de-duplicated.
    EXPECT_EQ(geocoding::parse_photon(read_fixture("geo_photon_london.json")),
              (std::vector<PlaceDto>{{"London", "London, England, United Kingdom", 51.5074456, -0.1277653, "city"},
                                     {"London", "London, Ontario, Canada", 42.9836747, -81.2496068, "city"}}));
    EXPECT_NEAR(wizz::distance_km({51.5074456, -0.1277653}, {51.4893335, -0.1440551}), 2.31, 0.01);
}

TEST(Geocoding, ParsePhoton_skips_unusable_features)
{
    const std::string body = R"({"features":[)"
                             R"({"properties":{"name":"no geometry"}},)"
                             R"({"geometry":{},"properties":{"name":"no coordinates"}},)"
                             R"({"geometry":{"coordinates":"x"}},)"
                             R"({"geometry":{"coordinates":[1]}},)"
                             R"({"geometry":{"coordinates":[1,2]},"properties":{}},)"
                             R"({"geometry":{"coordinates":[1,2]},"properties":{"name":null}},)"
                             R"({"geometry":{"coordinates":[1,2]},"properties":{"name":5}},)"
                             R"({"geometry":{"coordinates":[1,2]},"properties":{"name":" \t"}},)"
                             R"({"geometry":{"coordinates":[3,4,99]},"properties":{"name":" Kept ","type":"town","state":" kept ","country":"KEPT"}},)"
                             R"({"geometry":{"coordinates":[5,6]},"properties":{"name":"Other","osm_value":5,"type":"village"}},)"
                             R"({"geometry":{"coordinates":[7,8]},"properties":{"name":"Third","osm_value":"city","type":"town"}},)"
                             R"({"geometry":{"coordinates":[9,10]},"properties":{"name":"Fourth"}})"
                             R"(]})";
    EXPECT_EQ(geocoding::parse_photon(body),
              (std::vector<PlaceDto>{{" Kept ", " Kept , KEPT", 4.0, 3.0, "town"},
                                     {"Other", "Other", 6.0, 5.0, "village"},
                                     {"Third", "Third", 8.0, 7.0, "city"},
                                     {"Fourth", "Fourth", 10.0, 9.0, ""}}));
    // " kept " repeats " Kept " ignoring case; "KEPT" differs because parts are not trimmed.
    EXPECT_TRUE(geocoding::parse_photon(R"({"type":"FeatureCollection"})").empty());
    EXPECT_TRUE(geocoding::parse_photon(R"({"features":null})").empty());
    EXPECT_TRUE(geocoding::parse_photon(R"({"features":{"a":1}})").empty());
    EXPECT_TRUE(geocoding::parse_photon(R"({"features":[]})").empty());
}

TEST(Geocoding, ParsePhoton_feature_without_properties_fails_the_whole_response)
{
    const std::string body = R"({"features":[)"
                             R"({"geometry":{"coordinates":[15.4382786,47.0708678]},"properties":{"name":"Graz"}},)"
                             R"({"geometry":{"coordinates":[1,2]}}]})";
    expect_throws_exactly<geocoding::MissingPropertyError>([&] { (void)geocoding::parse_photon(body); },
                                                           "The given key was not present in the dictionary.");

    // A feature skipped for its geometry never reads its properties.
    EXPECT_EQ(geocoding::parse_photon(R"({"features":[{"geometry":{}},{"geometry":{"coordinates":[2,1]},"properties":{"name":"x"}}]})")
                  .size(),
              1u);

    // Through the resolver: Photon counts as failed, so Nominatim is asked.
    ResolverRig rig(photon_answers(body));
    const auto result = rig.resolver.geocode("Graz", {}, {});
    EXPECT_TRUE(result.found.empty());
    EXPECT_FALSE(result.failed) << "Nominatim answered []";
    EXPECT_EQ(rig.transport->urls(), (Codes{photon("Graz"), nominatim("Graz")}));
}

TEST(Geocoding, ParsePhoton_error_kinds)
{
    expect_throws_exactly<json::ParseError>([] { (void)geocoding::parse_photon("not json"); });
    expect_throws_exactly<json::ParseError>([] { (void)geocoding::parse_photon(""); });
    expect_throws_exactly<json::TypeError>([] { (void)geocoding::parse_photon("[1,2,3]"); });
    expect_throws_exactly<json::TypeError>([] { (void)geocoding::parse_photon(R"({"features":[5]})"); });
    expect_throws_exactly<json::TypeError>([] { (void)geocoding::parse_photon(R"({"features":[{"geometry":null}]})"); });
    expect_throws_exactly<json::TypeError>(
        [] { (void)geocoding::parse_photon(R"({"features":[{"geometry":{"coordinates":[1,2]},"properties":[]}]})"); });
    expect_throws_exactly<json::TypeError>([] {
        (void)geocoding::parse_photon(R"({"features":[{"geometry":{"coordinates":[1,"2"]},"properties":{"name":"x"}}]})");
    });
    expect_throws_exactly<json::FormatError>([] {
        (void)geocoding::parse_photon(R"({"features":[{"geometry":{"coordinates":[1,1e400]},"properties":{"name":"x"}}]})");
    });
}

TEST(Geocoding, ParseNominatim_reads_the_fixture)
{
    EXPECT_EQ(geocoding::parse_nominatim(read_fixture("geo_nominatim_graz.json")), std::vector<PlaceDto>{kGrazDto});
}

TEST(Geocoding, ParseNominatim_names_skips_and_kinds)
{
    const std::string body =
        R"([)"
        R"({"lat":" 47.07 ","lon":"+15.4","display_name":" Graz , Styria, Austria","addresstype":"city"},)"
        R"({"lat":"1","lon":"2","name":"  ","display_name":"Fallback, X"},)"
        R"({"lat":"3","lon":"4","name":" Named ","display_name":"Ignored, Y"},)"
        R"({"lat":5,"lon":"6","name":"number lat"},)"
        R"({"lat":"47,07","lon":"6","name":"comma"},)"
        R"({"lat":"7","lon":"","name":"empty lon"},)"
        R"({"lon":"8","name":"no lat"},)"
        R"({"lat":"9","lon":"10"},)"
        R"({"lat":"9","lon":"10","display_name":" , Z"},)"
        R"({"lat":"NaN","lon":"Infinity","name":"Odd"})"
        R"(])";
    const std::vector<PlaceDto> parsed = geocoding::parse_nominatim(body);
    ASSERT_EQ(parsed.size(), 4u);
    EXPECT_EQ(parsed[0], (PlaceDto{"Graz", " Graz , Styria, Austria", 47.07, 15.4, "city"}));
    EXPECT_EQ(parsed[1], (PlaceDto{"Fallback", "Fallback, X", 1.0, 2.0, ""}));
    EXPECT_EQ(parsed[2], (PlaceDto{" Named ", "Ignored, Y", 3.0, 4.0, ""}));
    EXPECT_EQ(parsed[3].name, "Odd");
    EXPECT_TRUE(std::isnan(parsed[3].lat));
    EXPECT_EQ(parsed[3].lon, std::numeric_limits<double>::infinity());

    // Not an array → nothing; an element that is not an object → InvalidOperationException.
    EXPECT_TRUE(geocoding::parse_nominatim(R"({"lat":"1"})").empty());
    EXPECT_TRUE(geocoding::parse_nominatim("null").empty());
    expect_throws_exactly<json::TypeError>([] { (void)geocoding::parse_nominatim("[null]"); });
    expect_throws_exactly<json::ParseError>([] { (void)geocoding::parse_nominatim("[1,]"); });
    // Dedupe applies.
    EXPECT_EQ(geocoding::parse_nominatim(R"([{"lat":"47","lon":"15","name":"Graz"},{"lat":"47.1","lon":"15","name":"GRAZ"}])")
                  .size(),
              1u);
}

TEST(Geocoding, TryParseDouble_matches_dotnet_float_parsing)
{
    // Expectations recorded with double.TryParse(s, NumberStyles.Float, InvariantCulture) on .NET 10.
    constexpr double kInf = std::numeric_limits<double>::infinity();
    const std::vector<std::pair<std::string_view, double>> accepted{
        {" 47.07 ", 47.07},
        {"+47.07", 47.07},
        {"4.707e1", 47.07},
        {".5", 0.5},
        {"5.", 5.0},
        {"Infinity", kInf},
        {"-Infinity", -kInf},
        {"+Infinity", kInf},
        {"infinity", kInf},
        {"INFINITY", kInf},
        {"-INFINITY", -kInf},
        {"+infinity", kInf},
        {"-Infinity ", -kInf},
        {"1E-2", 0.01},
        {"1e400", kInf},
        {"-1e400", -kInf},
        {"1e-400", 0.0},
        {" -5", -5.0},
        {"5 ", 5.0},
        {"\t5\n", 5.0},
        {"5\0"sv, 5.0},
        {"5\0\0"sv, 5.0},
        {"5 \0"sv, 5.0},
        {" 5\0"sv, 5.0},
        {"1e5\0"sv, 1e5},
        {"0.e5", 0.0},
        {".0e5", 0.0},
        {"1.e5", 1e5},
        {"1e+5", 1e5},
        {"1e-05", 1e-5},
        {"4.9406564584124654E-324", 5e-324},
        {"2.4703282292062327e-324", 0.0},
        {"2.4703282292062328e-324", 5e-324},
        {"1.7976931348623157e308", 1.7976931348623157e308},
        {"1.7976931348623158e308", 1.7976931348623157e308},
        {"1.7976931348623159e308", kInf},
        {"00012", 12.0},
        {"1e0000000000000000000000000400", kInf},
        {"1e-99999999999999999999", 0.0},
        {"123456789012345678901234567890", 1.2345678901234568e+29},
        {"0.000000000000000000000000000000000000000000001e-300", 0.0},
        {"0.1", 0.1},
        {"-0", -0.0},
        {"  -0.0e-0  ", -0.0},
        {"\xE2\x80\xA8-Infinity", -kInf},  // U+2028 is trimmed before the named constants
        {"\xE2\x80\xA8+Infinity", kInf},
        {"\xC2\x85Infinity", kInf},  // U+0085
    };
    for (const auto& [text, expected] : accepted) {
        SCOPED_TRACE(std::string{text});
        const std::optional<double> parsed = geocoding::try_parse_double(text);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, expected);
        EXPECT_EQ(std::signbit(*parsed), std::signbit(expected));
    }
    for (const std::string_view nan_text : {"NaN"sv, "nan"sv, "-nan"sv, "+NaN"sv, " NaN "sv, " -NaN "sv,
                                            "\xC2\xA0NaN"sv, "\xE2\x80\xA8NaN\xE3\x80\x80"sv}) {
        SCOPED_TRACE(std::string{nan_text});
        const std::optional<double> parsed = geocoding::try_parse_double(nan_text);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_TRUE(std::isnan(*parsed));
    }
    for (const std::string_view rejected :
         {"47,07"sv, "1,234.5"sv, "0x10"sv, ""sv, " "sv, "."sv, "-"sv, "+"sv, "1e"sv, "1e+"sv, "- 5"sv,
          "\xC2\xA0" "5"sv, "5\xC2\xA0"sv, "NaN\0"sv, "5\0 "sv, "\xE2\x88\x9E"sv, "-\xE2\x88\x9E"sv, "Inf"sv,
          "+-5"sv, "--5"sv, "1.2.3"sv, "\xD9\xA0"sv, "1_000"sv, "+ 5"sv, "5e5.5"sv, "1e\xD9\xA1"sv,
          "\xE2\x80\xA8" "5"sv, " - Infinity"sv, "-\xC2\xA0NaN"sv, "+-NaN"sv, "Infinityx"sv, "NaNa"sv, "+ NaN"sv,
          "\xE2\x80\x8BNaN"sv, "-\xE2\x80\xA8Infinity"sv, "- Infinity"sv, "-Infinity\0"sv, ".e5"sv, "e5"sv,
          "1E+"sv, "1.5E"sv}) {
        SCOPED_TRACE(std::string{rejected});
        EXPECT_FALSE(geocoding::try_parse_double(rejected).has_value());
    }
}

TEST(Geocoding, JoinParts_drops_blank_parts_and_case_insensitive_repeats)
{
    EXPECT_EQ(geocoding::join_parts({"Tbilisi", "Tbilisi", "Georgia"}), "Tbilisi, Georgia");
    EXPECT_EQ(geocoding::join_parts({"Vienna", std::nullopt, "Austria"}), "Vienna, Austria");
    EXPECT_EQ(geocoding::join_parts({"Kraków", "KRAKÓW", " \t", "", "Poland"}), "Kraków, Poland");
    EXPECT_EQ(geocoding::join_parts({" Graz", "Graz", "graz "}), " Graz, Graz, graz ");
    EXPECT_EQ(geocoding::join_parts({std::nullopt, std::nullopt}), "");
}

TEST(Geocoding, Dedupe_drops_same_name_hits_closer_than_30_km)
{
    const GeoPoint origin{47.0708678, 15.4382786};
    const GeoPoint at_29_9 = north_of(origin, 29.9);
    const GeoPoint at_30_1 = north_of(origin, 30.1);
    ASSERT_LT(wizz::distance_km(origin, at_29_9), 30.0);
    ASSERT_GT(wizz::distance_km(origin, at_30_1), 30.0);
    EXPECT_NEAR(wizz::distance_km(origin, at_29_9), 29.9, 1e-9);
    EXPECT_NEAR(wizz::distance_km(origin, at_30_1), 30.1, 1e-9);

    const PlaceDto first{"Graz", "first", origin.latitude, origin.longitude, ""};
    const PlaceDto near_twin{"GRÁZ", "near", at_29_9.latitude, at_29_9.longitude, ""};
    const PlaceDto far_twin{"graz", "far", at_30_1.latitude, at_30_1.longitude, ""};
    const PlaceDto other_name{"Grazac", "other", at_29_9.latitude, at_29_9.longitude, ""};

    EXPECT_EQ(geocoding::dedupe(std::vector<PlaceDto>{first, near_twin}), std::vector<PlaceDto>{first});
    EXPECT_EQ(geocoding::dedupe(std::vector<PlaceDto>{first, far_twin}), (std::vector<PlaceDto>{first, far_twin}));
    EXPECT_EQ(geocoding::dedupe(std::vector<PlaceDto>{first, other_name}), (std::vector<PlaceDto>{first, other_name}));
    // Only kept hits are compared against: the far twin survives, then hides its own neighbour.
    const PlaceDto beyond{"Graz", "beyond", north_of(origin, 50.0).latitude, origin.longitude, ""};
    EXPECT_EQ(geocoding::dedupe(std::vector<PlaceDto>{first, far_twin, beyond}),
              (std::vector<PlaceDto>{first, far_twin}));
}

TEST(Geocoding, ToPlace_and_WithPinned)
{
    expect_place(geocoding::to_place(kGrazDto), "Graz", "Graz, Styria, Austria", {47.0708678, 15.4382786},
                 PlaceSource::geocoder, {});

    // "London" prefix-matches LGW and LTN (RealAirports order): both lie within 150 km.
    const std::vector<PlaceDto> found{{"London", "London, England, United Kingdom", 51.5074456, -0.1277653, "city"},
                                      {"London", "London, Ontario, Canada", 42.9836747, -81.2496068, "city"}};
    const std::vector<Place> places = geocoding::with_pinned(found, *cat(), "London");
    ASSERT_EQ(places.size(), 2u);
    expect_place(places[0], "London", "London, England, United Kingdom", {51.5074456, -0.1277653},
                 PlaceSource::geocoder, {"LGW", "LTN"});
    EXPECT_TRUE(places[1].pinned_airports.empty());
    EXPECT_TRUE(geocoding::with_pinned(found, *cat(), "Graz")[0].pinned_airports.empty());
}

TEST(Geocoding, Service_failure_classification)
{
    std::stop_source live;
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto check = [&](const std::exception& error, bool expected) {
        SCOPED_TRACE(typeid(error).name());
        EXPECT_EQ(geocoding::is_service_failure(error, live.get_token()), expected);
        EXPECT_FALSE(geocoding::is_service_failure(error, cancelled.get_token()));
    };
    check(http::TransportError("offline"), true);
    check(http::TimeoutError("timeout"), true);
    check(json::ParseError("bad"), true);
    check(json::TypeError("shape"), true);
    check(geocoding::MissingPropertyError{}, true);
    check(OperationCancelled{}, true);
    check(json::FormatError("1e400"), false);
    check(wizz::FormatError("format"), false);
    check(std::logic_error("logic"), false);
    check(std::runtime_error("runtime"), false);
}

// ===========================================================================
// 02 §6.9 — the geocode.json memo (TTL, format, D-23)
// ===========================================================================

TEST(GeocodeCache, Parse_reads_PascalCase_records_in_file_order)
{
    const auto entries = geocoding::parse_geocode_cache(
        R"({"graz":[{"Name":"Graz","Detail":"Graz, Styria, Austria","Lat":47.0708678,"Lon":15.4382786,"Kind":"city"}],)"
        R"("empty":[],"x":[{}],"y":[{"name":"lower","LAT":5,"Lat":1.5,"Extra":[1,2],"Kind":null,"Detail":null,"Name":"a","Name":"b"}]})");
    ASSERT_EQ(entries.size(), 4u);
    EXPECT_EQ(entries[0], (GeocodeCacheEntry{"graz", {kGrazDto}}));
    EXPECT_EQ(entries[1], (GeocodeCacheEntry{"empty", {}}));
    EXPECT_EQ(entries[2], (GeocodeCacheEntry{"x", {PlaceDto{}}}));
    EXPECT_EQ(entries[3], (GeocodeCacheEntry{"y", {PlaceDto{"b", "", 1.5, 0.0, ""}}}));
}

TEST(GeocodeCache, Duplicate_keys_keep_the_first_position_and_the_last_value)
{
    const auto entries =
        geocoding::parse_geocode_cache(R"({"a":[{"Name":"1"}],"b":[],"a":[{"Name":"2"}]})");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0], (GeocodeCacheEntry{"a", {PlaceDto{"2", "", 0, 0, ""}}}));
    EXPECT_EQ(entries[1].key, "b");
}

TEST(GeocodeCache, D_23_null_or_non_array_values_are_misses)
{
    const auto entries = geocoding::parse_geocode_cache(
        R"({"a":null,"b":5,"c":"x","d":{"Name":"Graz"},"e":[null],"f":[{"Name":"x"},null],"g":[{"Name":"y"}]})");
    ASSERT_EQ(entries.size(), 7u);
    for (std::size_t i = 0; i < 6; ++i) {
        SCOPED_TRACE(entries[i].key);
        EXPECT_TRUE(entries[i].hits.empty());
    }
    EXPECT_EQ(entries[6], (GeocodeCacheEntry{"g", {PlaceDto{"y", "", 0, 0, ""}}}));
}

TEST(GeocodeCache, Corrupt_content_gives_an_empty_memo)
{
    for (const std::string_view text :
         {"not json"sv, ""sv, "null"sv, "[]"sv, "5"sv, R"("x")"sv, "{}"sv, R"({"a":[{"Name":5}]})"sv,
          R"({"a":[{"Detail":true}]})"sv, R"({"a":[{"Lat":"47.0"}]})"sv, R"({"a":[{"Lat":null}]})"sv,
          R"({"a":[{"Lon":1e400}]})"sv, R"({"a":[5]})"sv, R"({"a":[[]]})"sv, R"({"a":[null,5]})"sv,
          R"({"ok":[{"Name":"x"}],"a":[true]})"sv, R"({"a":[{"Name":"1","Name":2}]})"sv}) {
        SCOPED_TRACE(text);
        EXPECT_TRUE(geocoding::parse_geocode_cache(text).empty());
    }
}

TEST(GeocodeCache, Serialize_writes_compact_PascalCase_JSON_like_dotnet)
{
    const std::vector<GeocodeCacheEntry> entries{
        {"graz", {kGrazDto}},
        {"krakow", {{"Kraków", "Kraków \u2013 \"x\"", 50.0619474, -19.9368564, ""}}},
        {"none", {}},
    };
    EXPECT_EQ(geocoding::serialize_geocode_cache(entries),
              R"({"graz":[{"Name":"Graz","Detail":"Graz, Styria, Austria","Lat":47.0708678,"Lon":15.4382786,"Kind":"city"}],)"
              "\"krakow\":[{\"Name\":\"Krak\\u00F3w\",\"Detail\":\"Krak\\u00F3w \\u2013 \\u0022x\\u0022\",\"Lat\":50.0619474,"
              "\"Lon\":-19.9368564,\"Kind\":\"\"}],\"none\":[]}");
    EXPECT_EQ(geocoding::parse_geocode_cache(geocoding::serialize_geocode_cache(entries)), entries);
    EXPECT_EQ(geocoding::serialize_geocode_cache({}), "{}");
}

TEST(GeocodeCache, Entries_with_non_finite_coordinates_are_left_out_of_the_file)
{
    const std::vector<GeocodeCacheEntry> entries{
        {"odd", {{"Odd", "", std::numeric_limits<double>::quiet_NaN(), 1.0, ""}}},
        {"graz", {kGrazDto}},
    };
    EXPECT_EQ(geocoding::parse_geocode_cache(geocoding::serialize_geocode_cache(entries)),
              (std::vector<GeocodeCacheEntry>{{"graz", {kGrazDto}}}));
}

/// A resolver whose cache is a fresh temporary directory, FIXED clock.
struct CachedRig {
    explicit CachedRig(wizz::test::Responder responder) : rig(std::move(responder), directory.cache()) {}

    TempDirectory directory;
    ResolverRig rig;

    /// Writes geocode.json with a last-write time `age` before the FIXED now.
    void write_cache(std::string_view content, Duration age = std::chrono::hours{1}) const
    {
        write_bytes(directory.geocode_file(), content);
        set_last_write_utc(directory.geocode_file(), rig.clock->utc_now() - age);
    }
    [[nodiscard]] std::vector<GeocodeCacheEntry> cache_entries() const
    {
        return geocoding::parse_geocode_cache(read_bytes(directory.geocode_file()));
    }
};

[[nodiscard]] wizz::test::Responder graz_photon()
{
    return photon_answers(read_fixture("geo_photon_graz.json"));
}

TEST(GeocodeCache, Fresh_file_answers_without_network_and_exactly_90_days_is_fresh)
{
    CachedRig cached(photon_responder([](const RecordedRequest&) -> ScriptedResponse {
        throw std::logic_error("must not be called");
    }));
    cached.write_cache(cache_file({{"graz", {kGrazDto}}}), std::chrono::days{90});

    const PlaceLookup result = cached.rig.resolve("Graz", *cat());

    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "Graz", "Graz, Styria, Austria", {47.0708678, 15.4382786},
                 PlaceSource::geocoder, {});
    EXPECT_EQ(cached.rig.transport->request_count(), 0u);
    EXPECT_TRUE(cached.rig.log.notices().empty());
    EXPECT_EQ(geocoding::kCacheMaxAge, std::chrono::days{90});
}

TEST(GeocodeCache, A_file_older_than_90_days_is_ignored_and_then_replaced)
{
    CachedRig cached(graz_photon());
    cached.write_cache(cache_file({{"tbilisi", {{"Tbilisi", "Tbilisi, Georgia", 41.69, 44.80, "city"}}},
                                   {"graz", {kGrazDto}}}),
                       Duration{std::chrono::days{90}} + Duration{1});

    const PlaceLookup result = cached.rig.resolve("Graz", *cat());

    EXPECT_EQ(result.candidates.size(), 3u);
    EXPECT_EQ(cached.rig.transport->urls(), Codes{photon("Graz")});
    // The next write replaces the stale file with only the new entry.
    const auto entries = cached.cache_entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].key, "graz");
    EXPECT_EQ(entries[0].hits.size(), 3u);
}

TEST(GeocodeCache, Whole_memo_is_rewritten_atomically_after_a_non_empty_lookup)
{
    CachedRig cached(graz_photon());
    const GeocodeCacheEntry tbilisi{"tbilisi", {{"Tbilisi", "Tbilisi, Georgia", 41.6934591, 44.8014495, "city"}}};
    cached.write_cache(cache_file({tbilisi, {"vienna", {}}}));

    (void)cached.rig.resolve(" Graz ", *cat());

    const std::vector<GeocodeCacheEntry> expected{
        tbilisi, {"vienna", {}}, {"graz", geocoding::parse_photon(read_fixture("geo_photon_graz.json"))}};
    const std::string written = read_bytes(cached.directory.geocode_file());
    EXPECT_EQ(written, geocoding::serialize_geocode_cache(expected)) << "UTF-8, no BOM, whole memo in order";
    EXPECT_FALSE(std::filesystem::exists(cached.directory.path() / "geocode.json.tmp"));
}

TEST(GeocodeCache, Empty_results_are_never_written)
{
    CachedRig cached(photon_answers(std::string{kNoFeatures}));

    (void)cached.rig.resolve("Graz", *cat());

    EXPECT_FALSE(std::filesystem::exists(cached.directory.geocode_file()));
}

TEST(GeocodeCache, Memo_is_loaded_on_the_first_geocode_and_kept_for_the_process)
{
    CachedRig cached(photon_responder([](const RecordedRequest&) -> ScriptedResponse {
        throw std::logic_error("must not be called");
    }));
    cached.write_cache(cache_file({{"graz", {{"Graz", "A", 47.0, 15.0, ""}}}}));

    // An offline match does not load the memo...
    EXPECT_EQ(cached.rig.resolve("London", *cat()).candidates.size(), 1u);
    cached.write_cache(cache_file({{"graz", {{"Graz", "B", 47.0, 15.0, ""}}}}));
    // ...the first geocode does...
    EXPECT_EQ(cached.rig.resolve("Graz", *cat()).candidates.at(0).description, "B");
    // ...and it is never reloaded.
    cached.write_cache(cache_file({{"graz", {{"Graz", "C", 47.0, 15.0, ""}}}}));
    EXPECT_EQ(cached.rig.resolve("Graz", *cat()).candidates.at(0).description, "B");
    EXPECT_EQ(cached.rig.transport->request_count(), 0u);
}

TEST(GeocodeCache, Corrupt_file_gives_an_empty_memo_and_is_replaced)
{
    for (const std::string_view content : {"not json"sv, "null"sv, "[]"sv, R"({"graz":[{"Lat":"x"}]})"sv}) {
        SCOPED_TRACE(content);
        CachedRig cached(graz_photon());
        cached.write_cache(content);

        EXPECT_EQ(cached.rig.resolve("Graz", *cat()).candidates.size(), 3u);

        EXPECT_EQ(cached.rig.transport->urls(), Codes{photon("Graz")});
        const auto entries = cached.cache_entries();
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].key, "graz");
    }
}

TEST(GeocodeCache, D_23_null_value_is_a_miss_and_other_keys_keep_working)
{
    for (const std::string_view value : {"null"sv, R"({"Name":"Graz"})"sv, "5"sv, "[null]"sv}) {
        SCOPED_TRACE(value);
        CachedRig cached(graz_photon());
        cached.write_cache(std::string{R"({"graz":)"} + std::string{value} +
                           R"(,"vienna":[{"Name":"Vienna","Detail":"Vienna, Austria","Lat":48.21,"Lon":16.37,"Kind":"city"}]})");

        const PlaceLookup vienna = cached.rig.resolve("Vienna", *cat());
        ASSERT_EQ(vienna.candidates.size(), 1u);
        EXPECT_EQ(vienna.candidates[0].description, "Vienna, Austria");
        EXPECT_EQ(cached.rig.transport->request_count(), 0u);

        const PlaceLookup graz = cached.rig.resolve("Graz", *cat());
        EXPECT_EQ(graz.candidates.size(), 3u);
        EXPECT_EQ(cached.rig.transport->urls(), Codes{photon("Graz")});

        // The entry is replaced in place.
        const auto entries = cached.cache_entries();
        ASSERT_EQ(entries.size(), 2u);
        EXPECT_EQ(entries[0].key, "graz");
        EXPECT_EQ(entries[0].hits.size(), 3u);
        EXPECT_EQ(entries[1], (GeocodeCacheEntry{"vienna", {kViennaDto}}));
    }
}

TEST(GeocodeCache, Property_names_are_case_sensitive)
{
    CachedRig cached(photon_responder([](const RecordedRequest&) -> ScriptedResponse {
        throw std::logic_error("must not be called");
    }));
    cached.write_cache(
        R"({"graz":[{"name":"Graz","Detail":"Graz, Styria, Austria","Lat":47.0708678,"Lon":15.4382786,"Kind":"city"}]})");

    const PlaceLookup result = cached.rig.resolve("Graz", *cat());

    // The record reads Name = "", so the hit is not exact and the user must confirm.
    ASSERT_EQ(result.candidates.size(), 1u);
    expect_place(result.candidates[0], "", "Graz, Styria, Austria", {47.0708678, 15.4382786},
                 PlaceSource::geocoder, {});
    EXPECT_TRUE(result.needs_confirmation);
    EXPECT_EQ(cached.rig.transport->request_count(), 0u);
}

TEST(GeocodeCache, Typed_variants_share_one_entry)
{
    CachedRig cached(photon_answers(photon_body({{"Kraków", 50.06, 19.94, "Lesser Poland", "Poland"}})));

    const auto first = cached.rig.resolver.geocode("Kraków", {}, {});
    const auto second = cached.rig.resolver.geocode(" KRAKOW ", {}, {});

    EXPECT_EQ(first.found, second.found);
    EXPECT_EQ(cached.rig.transport->urls(), Codes{photon("Krak%C3%B3w")});
    const auto entries = cached.cache_entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].key, "krakow");
}

TEST(GeocodeCache, Memo_hit_with_an_empty_list_is_asked_again)
{
    CachedRig cached(graz_photon());
    cached.write_cache(R"({"graz":[]})");

    EXPECT_EQ(cached.rig.resolve("Graz", *cat()).candidates.size(), 3u);
    EXPECT_EQ(cached.rig.transport->urls(), Codes{photon("Graz")});
}

} // namespace
