// HomesStay (DECISIONS D-72).

#include "wizz/ui/homes_stay.hpp"

#include <algorithm>

namespace wizz::ui {

std::optional<HomesStay> stay_for_flight(const FlightResult& flight, StayLeg leg, int min_nights, Date today)
{
    const int nights = std::max(1, min_nights);
    if (leg == StayLeg::outbound) {
        if (!flight.to_airport) {
            return std::nullopt;
        }
        return HomesStay{.destination = flight.to_airport,
                         .check_in = flight.date(),
                         .check_out = flight.date().add_days(nights),
                         .adults = std::max(1, flight.adults)};
    }
    if (!flight.from_airport) {
        return std::nullopt;
    }
    const Date check_out = flight.date();
    const Date check_in = std::max(check_out.add_days(-nights), today);
    if (check_in >= check_out) {
        return std::nullopt;
    }
    return HomesStay{.destination = flight.from_airport,
                     .check_in = check_in,
                     .check_out = check_out,
                     .adults = std::max(1, flight.adults)};
}

std::optional<HomesStay> stay_for_trip(const RoundTrip& trip)
{
    if (!trip.outbound || !trip.inbound || !trip.outbound->to_airport) {
        return std::nullopt;
    }
    const Date check_in = trip.outbound->date();
    return HomesStay{.destination = trip.outbound->to_airport,
                     .check_in = check_in,
                     .check_out = std::max(trip.inbound->date(), check_in.add_days(1)),
                     .adults = std::max(1, trip.outbound->adults)};
}

double radius_km(HomesRadius radius) noexcept
{
    switch (radius) {
    case HomesRadius::two_km:
        return 2.0;
    case HomesRadius::five_km:
        return 5.0;
    case HomesRadius::ten_km:
        return 10.0;
    case HomesRadius::twenty_five_km:
        return 25.0;
    }
    return 5.0;
}

} // namespace wizz::ui
