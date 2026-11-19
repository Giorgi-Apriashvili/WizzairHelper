// CityNames (DECISIONS D-72): the city names tried for a station, and which
// geocoder hits name that city; pinned on the frozen live route map of 2026-11-02.

#include "wizz/core/city_names.hpp"

#include "wizz/core/catalog.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/text.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

namespace city_names = wizz::city_names;
using Strings = std::vector<std::string>;

TEST(CityNames, D_72_Clean_name_drops_brackets_second_names_and_line_breaks)
{
    EXPECT_EQ(city_names::clean_name("Catania (Sicily)"), "Catania");
    EXPECT_EQ(city_names::clean_name("Gran Canaria (Canary Islands)\n"), "Gran Canaria");
    EXPECT_EQ(city_names::clean_name("Athens\n"), "Athens");
    EXPECT_EQ(city_names::clean_name("  Warsaw \t Chopin\r\n"), "Warsaw Chopin");
    EXPECT_EQ(city_names::clean_name("Memmingen / Munich West"), "Memmingen");
    EXPECT_EQ(city_names::clean_name("Karlsruhe/Baden-Baden"), "Karlsruhe");
    EXPECT_EQ(city_names::clean_name("Palanga – Klaipeda"), "Palanga");
    EXPECT_EQ(city_names::clean_name("Palanga - Klaipeda"), "Palanga");
    EXPECT_EQ(city_names::clean_name("Palanga — Klaipeda"), "Palanga");
    EXPECT_EQ(city_names::clean_name("Cluj-Napoca"), "Cluj-Napoca") << "a hyphen inside a word stays";
    EXPECT_EQ(city_names::clean_name("Tenerife Norte (Canary Islands)"), "Tenerife Norte");
    EXPECT_EQ(city_names::clean_name("Rome (All (Airports))"), "Rome") << "nested brackets";
    EXPECT_EQ(city_names::clean_name("Rome (unclosed"), "Rome");
    EXPECT_EQ(city_names::clean_name("(Sicily)"), "");
    EXPECT_EQ(city_names::clean_name(" \n "), "");
    EXPECT_EQ(city_names::clean_name("/ Munich"), "");
}

TEST(CityNames, D_72_Candidates_go_from_the_most_specific_name_to_the_first_word)
{
    EXPECT_EQ(city_names::candidates("London Luton"), (Strings{"London Luton", "London"}));
    EXPECT_EQ(city_names::candidates("Barcelona El Prat"), (Strings{"Barcelona El Prat", "Barcelona El", "Barcelona"}));
    EXPECT_EQ(city_names::candidates("Santiago de Compostela\n"),
              (Strings{"Santiago de Compostela", "Santiago de", "Santiago"}));
    EXPECT_EQ(city_names::candidates("Oslo Sandefjord Torp"), (Strings{"Oslo Sandefjord Torp", "Oslo Sandefjord", "Oslo"}));
    EXPECT_EQ(city_names::candidates("Ab Cd"), (Strings{"Ab Cd"})) << "\"Ab\" is shorter than 3 units";
    EXPECT_EQ(city_names::candidates("Ab"), (Strings{}));
    EXPECT_EQ(city_names::candidates("Abu Dhabi Int Airport"), (Strings{"Abu Dhabi Int", "Abu Dhabi", "Abu"}))
        << "at most three words";
    EXPECT_EQ(city_names::candidates("Basel-Mulhouse-Freiburg"), (Strings{"Basel-Mulhouse-Freiburg", "Basel"}));
    EXPECT_EQ(city_names::candidates("Tel-Aviv"), (Strings{"Tel-Aviv", "Tel"}));
    EXPECT_EQ(city_names::candidates("Budapest"), (Strings{"Budapest"}));
    EXPECT_EQ(city_names::candidates("Niš"), (Strings{"Niš"})) << "3 UTF-16 units, 4 UTF-8 bytes";
    EXPECT_EQ(city_names::candidates("Catania (Sicily)"), (Strings{"Catania"}));
    EXPECT_EQ(city_names::candidates("Memmingen / Munich West"), (Strings{"Memmingen"}));
    EXPECT_EQ(city_names::candidates("Up-Down"), (Strings{"Up-Down"})) << "\"Up\" is too short";
    EXPECT_EQ(city_names::candidates("Rio-Rio Rio"), (Strings{"Rio-Rio Rio", "Rio-Rio", "Rio"})) << "no repeats";
    EXPECT_EQ(city_names::candidates(""), (Strings{}));
    EXPECT_EQ(city_names::candidates("(Sicily)"), (Strings{}));
}

TEST(CityNames, D_72_A_hit_names_the_city_when_its_key_starts_with_the_candidate_as_whole_words)
{
    EXPECT_TRUE(city_names::names_city("London", "London"));
    EXPECT_TRUE(city_names::names_city("LONDON", "london")) << "case";
    EXPECT_TRUE(city_names::names_city("Frankfurt am Main", "Frankfurt"));
    EXPECT_TRUE(city_names::names_city("Tel Aviv-Yafo", "Tel-Aviv")) << "hyphens read as spaces";
    EXPECT_TRUE(city_names::names_city("Târgu Mureș", "Targu Mures")) << "accents";
    EXPECT_TRUE(city_names::names_city("Kraków", "Krakow"));
    EXPECT_TRUE(city_names::names_city("Tromsø", "Tromso")) << "ø is o in the key";
    EXPECT_TRUE(city_names::names_city("Sharm el-Sheikh", "Sharm El Sheikh"));
    EXPECT_TRUE(city_names::names_city("Castellón de la Plana", "Castellon"));
    EXPECT_TRUE(city_names::names_city("Palma, Balearic Islands", "Palma")) << "a comma is a boundary";

    EXPECT_FALSE(city_names::names_city("Granadilla de Abona", "Gran")) << "not a whole word";
    EXPECT_FALSE(city_names::names_city("El Prat de Llobregat", "Barcelona El Prat"));
    EXPECT_FALSE(city_names::names_city("Brandenburg an der Havel", "Berlin Brandenburg"));
    EXPECT_FALSE(city_names::names_city("Luton", "London Luton"));
    EXPECT_FALSE(city_names::names_city("Lond", "London")) << "shorter than the candidate";
    EXPECT_FALSE(city_names::names_city("London", "")) << "an empty candidate names nothing";
    EXPECT_FALSE(city_names::names_city("London", " - ")) << "nor does one that is only a hyphen";
    EXPECT_FALSE(city_names::names_city("Budapest2", "Budapest")) << "a digit continues the word";
}

// --- the frozen live map -------------------------------------------------------------

[[nodiscard]] const wizz::AirportCatalog& live()
{
    static const wizz::AirportCatalogPtr catalog =
        wizz::AirportCatalog::from_map_json(wizz::test::read_live_cache("map.json"), wizz::Locale::en_us());
    return *catalog;
}

TEST(CityNames, D_72_Every_real_station_of_the_live_map_has_clean_candidates)
{
    std::size_t stations = 0;
    for (const wizz::AirportPtr& airport : live().real_airports()) {
        ++stations;
        const Strings found = city_names::candidates(airport->name);
        ASSERT_FALSE(found.empty()) << airport->iata;
        EXPECT_EQ(found.front().find_first_of("()/\n\r\t"), std::string::npos) << airport->iata;
        for (const std::string& candidate : found) {
            EXPECT_GE(wizz::text::utf16_length(candidate), city_names::kMinLength) << airport->iata;
            EXPECT_EQ(wizz::text::trim(candidate), candidate) << airport->iata;
            EXPECT_TRUE(city_names::names_city(candidate, candidate)) << airport->iata << " " << candidate;
        }
    }
    EXPECT_GT(stations, 150u);
}

TEST(CityNames, D_72_Candidates_of_typical_live_stations)
{
    const std::vector<std::pair<std::string, Strings>> expected{
        {"LTN", {"London Luton", "London"}},
        {"BUD", {"Budapest"}},
        {"BCN", {"Barcelona El Prat", "Barcelona El", "Barcelona"}},
        {"CTA", {"Catania"}},
        {"FMM", {"Memmingen"}},
        {"TLV", {"Tel-Aviv", "Tel"}},
        {"BSL", {"Basel-Mulhouse-Freiburg", "Basel"}},
        {"LPA", {"Gran Canaria", "Gran"}},
        {"KUT", {"Kutaisi"}},
        {"TRF", {"Oslo Sandefjord Torp", "Oslo Sandefjord", "Oslo"}},
    };
    for (const auto& [code, names] : expected) {
        const wizz::AirportPtr airport = live().find(code);
        ASSERT_NE(airport, nullptr) << code;
        EXPECT_EQ(city_names::candidates(airport->name), names) << code << " \"" << airport->name << "\"";
    }
}

} // namespace
