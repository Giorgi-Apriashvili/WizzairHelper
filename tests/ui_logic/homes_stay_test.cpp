// HomesStay (DECISIONS D-72): the destination, stay dates and adults of the
// "Homes on Airbnb" action for each grid's rows; the radius choices (D-73).

#include "wizz/ui/homes_stay.hpp"

#include "ui_samples.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>

namespace wizz::test::ui {
namespace {

using wizz::ui::HomesRadius;
using wizz::ui::HomesStay;
using wizz::ui::StayLeg;

const AirportPtr kBud = airport("BUD", "Budapest");
const AirportPtr kLtn = airport("LTN", "London Luton");

[[nodiscard]] FlightResultPtr flight(AirportPtr from, AirportPtr to, Date date, int adults = 1)
{
    FlightResult copy = *hand_fare(std::move(from), std::move(to), date, 10, 0, "50", "EUR", "50");
    copy.adults = adults;
    return std::make_shared<const FlightResult>(std::move(copy));
}

TEST(HomesStay, D_72_An_outbound_row_stays_the_minimum_nights_from_its_date_at_its_arrival)
{
    const std::optional<HomesStay> stay =
        wizz::ui::stay_for_flight(*flight(kBud, kLtn, day(12, 4), 2), StayLeg::outbound, 3, kToday);
    ASSERT_TRUE(stay.has_value());
    EXPECT_EQ(stay->destination, kLtn);
    EXPECT_EQ(stay->check_in, day(12, 4));
    EXPECT_EQ(stay->check_out, day(12, 7));
    EXPECT_EQ(stay->adults, 2);
}

TEST(HomesStay, D_72_A_return_row_stays_the_minimum_nights_before_its_date_at_its_departure)
{
    const std::optional<HomesStay> stay =
        wizz::ui::stay_for_flight(*flight(kLtn, kBud, day(12, 10)), StayLeg::inbound, 4, kToday);
    ASSERT_TRUE(stay.has_value());
    EXPECT_EQ(stay->destination, kLtn);
    EXPECT_EQ(stay->check_in, day(12, 6));
    EXPECT_EQ(stay->check_out, day(12, 10));
}

TEST(HomesStay, D_72_At_least_one_night)
{
    for (const int nights : {0, -5, 1}) {
        const auto out = wizz::ui::stay_for_flight(*flight(kBud, kLtn, day(12, 4)), StayLeg::outbound, nights, kToday);
        ASSERT_TRUE(out.has_value()) << nights;
        EXPECT_EQ(out->check_out, day(12, 5)) << nights;
        const auto back = wizz::ui::stay_for_flight(*flight(kLtn, kBud, day(12, 4)), StayLeg::inbound, nights, kToday);
        ASSERT_TRUE(back.has_value()) << nights;
        EXPECT_EQ(back->check_in, day(12, 3)) << nights;
    }
}

TEST(HomesStay, D_72_A_return_stay_never_starts_before_today)
{
    // kToday is 2 Nov: a return on 4 Nov with 5 nights starts today.
    const auto clamped = wizz::ui::stay_for_flight(*flight(kLtn, kBud, day(11, 4)), StayLeg::inbound, 5, kToday);
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(clamped->check_in, kToday);
    EXPECT_EQ(clamped->check_out, day(11, 4));
    // A return flight today (or, defensively, earlier) leaves no night.
    EXPECT_FALSE(wizz::ui::stay_for_flight(*flight(kLtn, kBud, kToday), StayLeg::inbound, 2, kToday).has_value());
    EXPECT_FALSE(
        wizz::ui::stay_for_flight(*flight(kLtn, kBud, kToday.add_days(-1)), StayLeg::inbound, 2, kToday).has_value());
    // An outbound flight today is a stay from today.
    EXPECT_TRUE(wizz::ui::stay_for_flight(*flight(kBud, kLtn, kToday), StayLeg::outbound, 2, kToday).has_value());
}

TEST(HomesStay, D_72_Adults_are_at_least_one)
{
    const auto stay = wizz::ui::stay_for_flight(*flight(kBud, kLtn, day(12, 4), 0), StayLeg::outbound, 2, kToday);
    ASSERT_TRUE(stay.has_value());
    EXPECT_EQ(stay->adults, 1);
}

TEST(HomesStay, D_72_A_row_without_its_station_has_no_stay)
{
    FlightResult no_airports = *flight(kBud, kLtn, day(12, 4));
    no_airports.to_airport = nullptr;
    EXPECT_FALSE(wizz::ui::stay_for_flight(no_airports, StayLeg::outbound, 2, kToday).has_value());
    EXPECT_TRUE(wizz::ui::stay_for_flight(no_airports, StayLeg::inbound, 2, kToday).has_value())
        << "a Return row needs only its departure station";
    no_airports.from_airport = nullptr;
    EXPECT_FALSE(wizz::ui::stay_for_flight(no_airports, StayLeg::inbound, 2, kToday).has_value());
}

TEST(HomesStay, D_72_A_round_trip_stays_from_the_outbound_to_the_return)
{
    RoundTrip trip{.outbound = flight(kBud, kLtn, day(12, 4), 3), .inbound = flight(kLtn, kBud, day(12, 9), 3),
                   .nights = 5};
    const std::optional<HomesStay> stay = wizz::ui::stay_for_trip(trip);
    ASSERT_TRUE(stay.has_value());
    EXPECT_EQ(*stay, (HomesStay{.destination = kLtn, .check_in = day(12, 4), .check_out = day(12, 9), .adults = 3}));

    // A same-day return still stays one night.
    trip.inbound = flight(kLtn, kBud, day(12, 4));
    ASSERT_TRUE(wizz::ui::stay_for_trip(trip).has_value());
    EXPECT_EQ(wizz::ui::stay_for_trip(trip)->check_out, day(12, 5));

    trip.inbound = nullptr;
    EXPECT_FALSE(wizz::ui::stay_for_trip(trip).has_value());
    EXPECT_FALSE(wizz::ui::stay_for_trip(RoundTrip{}).has_value());
}

TEST(HomesStay, D_73_The_radius_choices)
{
    EXPECT_EQ(wizz::ui::radius_km(HomesRadius::two_km), 2.0);
    EXPECT_EQ(wizz::ui::radius_km(HomesRadius::five_km), 5.0);
    EXPECT_EQ(wizz::ui::radius_km(HomesRadius::ten_km), 10.0);
    EXPECT_EQ(wizz::ui::radius_km(HomesRadius::twenty_five_km), 25.0);
    EXPECT_EQ(wizz::ui::kHomesRadii.front(), HomesRadius::two_km);
    EXPECT_EQ(wizz::ui::kHomesRadii.back(), HomesRadius::twenty_five_km);
}

} // namespace
} // namespace wizz::test::ui
