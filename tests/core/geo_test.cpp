// Geo.DistanceKm and Geo.Centroid (02 §3, 05 CT-GEO-01, 05 §2.3 distance tables).

#include "wizz/core/geo.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {

using wizz::GeoPoint;
using wizz::distance_km;

// Station coordinates of fixtures/map_subset.json after parsing (05 §2.3).
constexpr GeoPoint kTia{41.415, 19.72083333333333};
constexpr GeoPoint kBud{47.43694444444444, 19.255555555555556};
constexpr GeoPoint kDeb{47.55, 21.7};
constexpr GeoPoint kAho{40.63027777777778, 8.289722222222222};
constexpr GeoPoint kNap{40.88611111111111, 14.291666666666666};
constexpr GeoPoint kKtw{50.483333333333334, 19.083333333333332};
constexpr GeoPoint kKrk{50.07777777777778, 19.784722222222225};
constexpr GeoPoint kBts{48.166666666666664, 17.216666666666665};
constexpr GeoPoint kBcn{41.29694444444444, 2.0783333333333336};
constexpr GeoPoint kLon{51.51, -0.11805555555555555};
constexpr GeoPoint kLtn{51.874722222222225, -0.3683333333333333};
constexpr GeoPoint kLgw{51.14805555555556, -0.19027777777777777};

/// xUnit `Assert.Equal(expected, actual, places)`: both sides rounded with Math.Round
/// (half to even) to `places` decimals. The spec tables are given that way.
[[nodiscard]] double round_places(double value, int places)
{
    const double scale = std::pow(10.0, places);
    return std::nearbyint(value * scale) / scale;
}

TEST(Geo, CT_GEO_01_Haversine_matches_known_airport_distances)
{
    // [A]
    const double bud_bts = distance_km(kBud, kBts);
    const double ltn_lgw = distance_km(kLtn, kLgw);
    const double quarter_turn = distance_km(GeoPoint{0, 0}, GeoPoint{0, 180});
    const double same_point = distance_km(GeoPoint{47, 19}, GeoPoint{47, 19});
    EXPECT_GE(bud_bts, 170.0);
    EXPECT_LE(bud_bts, 175.0);
    EXPECT_GE(ltn_lgw, 80.0);
    EXPECT_LE(ltn_lgw, 83.0);
    EXPECT_GE(quarter_turn, 20014.0);
    EXPECT_LE(quarter_turn, 20016.0);
    EXPECT_EQ(round_places(same_point, 6), 0.0);
    // [D] exact values
    EXPECT_DOUBLE_EQ(bud_bts, 172.54389037013857);
    EXPECT_DOUBLE_EQ(ltn_lgw, 81.73582262738209);
    EXPECT_DOUBLE_EQ(quarter_turn, 20015.114442035923);
    EXPECT_EQ(same_point, 0.0);
}

TEST(Geo, Distances_from_BUD_table_05_2_3)
{
    struct Row {
        const char* iata;
        GeoPoint location;
        double expected_km;
        int places;
    };
    const std::array rows{
        Row{"BUD", kBud, 0.0, 4},       Row{"BTS", kBts, 172.5439, 4}, Row{"DEB", kDeb, 184.0772, 4},
        Row{"KRK", kKrk, 296.2, 1},     Row{"KTW", kKtw, 339.0, 1},    Row{"TIA", kTia, 670.6, 1},
        Row{"NAP", kNap, 828.7, 1},     Row{"AHO", kAho, 1156.2, 1},   Row{"LGW", kLgw, 1464.4, 1},
        Row{"LTN", kLtn, 1490.8, 1},    Row{"BCN", kBcn, 1522.1, 1},
    };
    for (const Row& row : rows) {
        EXPECT_DOUBLE_EQ(round_places(distance_km(kBud, row.location), row.places), row.expected_km) << row.iata;
    }
}

TEST(Geo, Distances_between_group_members_table_05_2_3)
{
    EXPECT_DOUBLE_EQ(round_places(distance_km(kLon, kLgw), 4), 40.5581);
    EXPECT_DOUBLE_EQ(round_places(distance_km(kLon, kLtn), 4), 44.0719);
    EXPECT_DOUBLE_EQ(round_places(distance_km(kKtw, kKrk), 4), 67.2118);
    EXPECT_DOUBLE_EQ(round_places(distance_km(kLtn, kLgw), 4), 81.7358);
    // 05 §1.7 Leg("London", 0): LGW 40.56 km, LTN 44.07 km; P-18: ToDistanceKm 81.74.
    EXPECT_DOUBLE_EQ(round_places(distance_km(kLon, kLgw), 2), 40.56);
    EXPECT_DOUBLE_EQ(round_places(distance_km(kLon, kLtn), 2), 44.07);
    EXPECT_DOUBLE_EQ(round_places(distance_km(kLtn, kLgw), 2), 81.74);
}

TEST(Geo, Reference_values_02_3_2_and_worked_examples_02_9)
{
    // 02 §3.2: London UK geocoder hit – LON pseudo station; Barcelona city – BCN.
    EXPECT_DOUBLE_EQ(round_places(distance_km(GeoPoint{51.5074456, -0.1277653}, kLon), 2), 0.73);
    EXPECT_DOUBLE_EQ(round_places(distance_km(GeoPoint{41.3825802, 2.177073}, kBcn), 2), 12.59);
    // 02 §9 #5: "Barcelona, Catalonia, Spain" (41.3826, 2.1771) is 12.6 km from BCN.
    EXPECT_DOUBLE_EQ(round_places(distance_km(GeoPoint{41.3826, 2.1771}, kBcn), 1), 12.6);
    // 02 §3.2 / §9 #7: Frankfurt city – HHN (49.9486, 7.2639 as printed in 02 §9) is 102.89 km.
    EXPECT_DOUBLE_EQ(round_places(distance_km(GeoPoint{50.1106444, 8.6820917}, GeoPoint{49.9486, 7.2639}), 2), 102.89);
    // 02 §9 #8: Vienna AT – BTS 62.8 km, BUD 231.7 km. The spec prints the geocoder hit
    // rounded to 3 decimals (48.208, 16.373), about 100 m off, so only ±0.1 km can agree.
    EXPECT_NEAR(distance_km(GeoPoint{48.208, 16.373}, kBts), 62.8, 0.1);
    EXPECT_NEAR(distance_km(GeoPoint{48.208, 16.373}, kBud), 231.7, 0.1);
}

TEST(Geo, CT_RTR_04_Hand_built_LTN_to_EVN_distance)
{
    // 05 CT-RTR-04 [D]: LTN (51.87, −0.37) – EVN (40.15, 44.40) is 3627.2645 km.
    EXPECT_DOUBLE_EQ(round_places(distance_km(GeoPoint{51.87, -0.37}, GeoPoint{40.15, 44.40}), 4), 3627.2645);
}

TEST(Geo, Distance_is_symmetric_and_zero_for_identical_points)
{
    const std::array points{kTia, kBud, kDeb, kAho, kNap, kKtw, kKrk, kBts, kBcn, kLon, kLtn, kLgw};
    for (const GeoPoint& a : points) {
        EXPECT_EQ(distance_km(a, a), 0.0);
        for (const GeoPoint& b : points) {
            EXPECT_EQ(distance_km(a, b), distance_km(b, a));
        }
    }
}

TEST(Geo, Antipodal_points_do_not_produce_nan)
{
    const double half_circumference = std::numbers::pi * wizz::kEarthRadiusKm;
    EXPECT_NEAR(distance_km(GeoPoint{90, 0}, GeoPoint{-90, 0}), half_circumference, 1e-6);
    EXPECT_NEAR(distance_km(GeoPoint{10, 20}, GeoPoint{-10, -160}), half_circumference, 1e-3);
}

TEST(Geo, Default_point_is_zero_zero_not_missing)
{
    constexpr GeoPoint origin{};
    EXPECT_EQ(origin, (GeoPoint{0.0, 0.0}));
    EXPECT_EQ(wizz::kEarthRadiusKm, 6371.0088);
}

TEST(Geo, Centroid_of_no_points_throws_No_points_given)
{
    try {
        (void)wizz::centroid({});
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_STREQ(e.what(), "No points given.");
    }
}

TEST(Geo, Centroid_is_the_mean_of_each_coordinate)
{
    const std::array one{kBud};
    EXPECT_EQ(wizz::centroid(one), kBud);

    // SPQ has no pseudo station, so a Katowice / Kraków city place sits at the centroid (02 §3.3).
    const std::array spq{kKtw, kKrk};
    const GeoPoint spq_centre = wizz::centroid(spq);
    EXPECT_DOUBLE_EQ(spq_centre.latitude, (50.483333333333334 + 50.07777777777778) / 2);
    EXPECT_DOUBLE_EQ(spq_centre.longitude, (19.083333333333332 + 19.784722222222225) / 2);

    // Sequential sums in input order, divided by the count (Enumerable.Average).
    const std::array london{kLon, kLtn, kLgw};
    const GeoPoint london_centre = wizz::centroid(london);
    EXPECT_EQ(london_centre.latitude, (kLon.latitude + kLtn.latitude + kLgw.latitude) / 3);
    EXPECT_EQ(london_centre.longitude, (kLon.longitude + kLtn.longitude + kLgw.longitude) / 3);
}

TEST(Geo, Centroid_does_not_wrap_the_antimeridian)
{
    const std::array points{GeoPoint{0, 179}, GeoPoint{0, -179}};
    EXPECT_EQ(wizz::centroid(points), (GeoPoint{0, 0}));
}

} // namespace
