#pragma once
// HomesStay (DECISIONS D-72): what the "Homes on Airbnb" action of a result row
// searches — the destination station, the stay dates and the adults — and the
// distances of settings.json `HomesWithin` (D-73). Pure.
//
// Check-in is the outbound flight's (local) departure date: a night flight lands
// the next day, and the user moves the dates on Airbnb if needed.

#include "wizz/core/domain.hpp"
#include "wizz/core/time.hpp"

#include <array>
#include <optional>

namespace wizz::ui {

/// The grid a flight row is in: the destination is where an Outbound flight lands and
/// where a Return flight leaves from.
enum class StayLeg { outbound, inbound };

struct HomesStay {
    AirportPtr destination;  ///< never null
    Date check_in;
    Date check_out;  ///< after check_in
    int adults = 1;  ///< at least 1

    friend bool operator==(const HomesStay&, const HomesStay&) = default;
};

/// An Outbound row: its arrival station, check-in on the flight's date, check-out
/// max(1, min_nights) nights later. A Return row: its departure station, check-out on the
/// flight's date, check-in max(1, min_nights) nights earlier but not before `today`;
/// nullopt when that leaves no night (a return flight today). nullopt without the station,
/// and for a flight before `today` (results kept open past midnight).
[[nodiscard]] std::optional<HomesStay> stay_for_flight(const FlightResult& flight, StayLeg leg, int min_nights,
                                                       Date today);

/// A round trip: the outbound's arrival station, check-in on the outbound's date,
/// check-out on the return's date and at least one night later (a same-day return
/// stays one night). nullopt without both flights or the station, and when the outbound
/// flies before `today`.
[[nodiscard]] std::optional<HomesStay> stay_for_trip(const RoundTrip& trip, Date today);

/// settings.json `HomesWithin` (D-73): how far around the city centre the search reaches.
enum class HomesRadius { two_km, five_km, ten_km, twenty_five_km };
/// The choices, nearest first.
inline constexpr std::array<HomesRadius, 4> kHomesRadii{HomesRadius::two_km, HomesRadius::five_km,
                                                        HomesRadius::ten_km, HomesRadius::twenty_five_km};
/// 2, 5, 10 or 25.
[[nodiscard]] double radius_km(HomesRadius radius) noexcept;

} // namespace wizz::ui
