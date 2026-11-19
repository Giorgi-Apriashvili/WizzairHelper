// AirbnbLinks (DECISIONS D-72): the map box around a city centre and both search
// URLs, byte for byte.

#include "wizz/core/airbnb_links.hpp"

#include "wizz/core/geo.hpp"
#include "wizz/core/time.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using wizz::Date;
using wizz::GeoPoint;
namespace links = wizz::airbnb_links;

constexpr Date day(unsigned month, unsigned day_of_month, int year = 2026) noexcept
{
    return Date::from_ymd(year, month, day_of_month);
}

TEST(AirbnbLinks, D_72_The_box_reaches_the_radius_in_every_direction)
{
    // At the equator one degree is kKmPerDegree in both directions.
    EXPECT_EQ(links::box_around(GeoPoint{0.0, 10.0}, links::kKmPerDegree),
              (links::MapBox{.north_east = {1.0, 11.0}, .south_west = {-1.0, 9.0}}));

    // Further north a degree of longitude is shorter, so the box is wider in degrees,
    // and each side is the radius away from the centre (within 0.5 %).
    const GeoPoint london{51.5074, -0.1278};
    const links::MapBox box = links::box_around(london, 5.0);
    EXPECT_NEAR(wizz::distance_km(london, GeoPoint{box.north_east.latitude, london.longitude}), 5.0, 0.025);
    EXPECT_NEAR(wizz::distance_km(london, GeoPoint{box.south_west.latitude, london.longitude}), 5.0, 0.025);
    EXPECT_NEAR(wizz::distance_km(london, GeoPoint{london.latitude, box.north_east.longitude}), 5.0, 0.025);
    EXPECT_NEAR(wizz::distance_km(london, GeoPoint{london.latitude, box.south_west.longitude}), 5.0, 0.025);
    EXPECT_GT(box.north_east.longitude - london.longitude, box.north_east.latitude - london.latitude);
}

TEST(AirbnbLinks, D_72_Homes_in_a_box)
{
    EXPECT_EQ(links::homes_in_box(links::box_around(GeoPoint{51.5074, -0.1278}, 5.0), day(12, 4), day(12, 8), 2),
              "https://www.airbnb.com/s/homes?checkin=2026-12-04&checkout=2026-12-08&adults=2"
              "&ne_lat=51.55232&ne_lng=-0.05564&sw_lat=51.46248&sw_lng=-0.19996&search_by_map=true");
    EXPECT_EQ(links::homes_in_box(links::box_around(GeoPoint{47.4979, 19.0402}, 10.0), day(12, 30), day(1, 2, 2027), 1),
              "https://www.airbnb.com/s/homes?checkin=2026-12-30&checkout=2027-01-02&adults=1"
              "&ne_lat=47.58773&ne_lng=19.17316&sw_lat=47.40807&sw_lng=18.90724&search_by_map=true");
}

TEST(AirbnbLinks, D_72_Homes_by_name_escape_the_place)
{
    EXPECT_EQ(links::homes_by_name("Santorini, Greece", day(6, 1), day(6, 5), 3),
              "https://www.airbnb.com/s/homes?query=Santorini%2C%20Greece&checkin=2026-06-01&checkout=2026-06-05"
              "&adults=3");
    EXPECT_EQ(links::homes_by_name("Gran Canaria & Málaga", day(6, 1), day(6, 2), 1),
              "https://www.airbnb.com/s/homes?query=Gran%20Canaria%20%26%20M%C3%A1laga&checkin=2026-06-01"
              "&checkout=2026-06-02&adults=1");
}

TEST(AirbnbLinks, D_72_Adults_are_at_least_one)
{
    EXPECT_TRUE(links::homes_by_name("Malta", day(6, 1), day(6, 2), 0).ends_with("&adults=1"));
    EXPECT_TRUE(links::homes_by_name("Malta", day(6, 1), day(6, 2), -2).ends_with("&adults=1"));
    EXPECT_TRUE(
        links::homes_in_box(links::box_around(GeoPoint{}, 1.0), day(6, 1), day(6, 2), 0).contains("&adults=1&"));
    EXPECT_TRUE(links::homes_by_name("Malta", day(6, 1), day(6, 2), 9).ends_with("&adults=9"));
}

TEST(AirbnbLinks, D_72_Place_query_names_the_country_unless_it_is_the_place)
{
    EXPECT_EQ(links::place_query("Santorini", "Greece"), "Santorini, Greece");
    EXPECT_EQ(links::place_query(" Gran Canaria\n", " Spain "), "Gran Canaria, Spain");
    EXPECT_EQ(links::place_query("Malta", "Malta"), "Malta");
    EXPECT_EQ(links::place_query("MALTA", "malta"), "MALTA");
    EXPECT_EQ(links::place_query("Asturias", ""), "Asturias");
}

} // namespace
