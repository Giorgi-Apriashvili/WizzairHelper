#pragma once
// FindCityCentre (DECISIONS D-72): the centre of the city a destination station
// stands for, for the "Homes on Airbnb" search. Wizz Air's map knows only the
// airport's position (and for nine city groups a pseudo station), so the city's
// names (`city_names::candidates`) are geocoded, most specific first, and the
// first hit that names the city within kCityCentreMaxDistanceKm of the airport is
// its centre. Blocking: runs on a worker.

#include "wizz/core/domain.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/services/place_resolver.hpp"

#include <optional>
#include <stop_token>
#include <string>

namespace wizz {

/// The farthest a city's centre may lie from its airport ("Paris Beauvais" is 85 km
/// from Paris, "Frankfurt Hahn" about 105 km from Frankfurt); the place resolver pins
/// geocoder hits to stations over the same distance (02 §6.2 S4).
inline constexpr double kCityCentreMaxDistanceKm = 150.0;

/// A city and its centre, as the geocoder names and places it.
struct CityCentre {
    std::string name;
    GeoPoint location;
};

struct CityCentreLookup {
    /// The city's centre, when a candidate named it near the airport.
    std::optional<CityCentre> centre;
    /// The station's cleaned name (`city_names::clean_name`, the raw name when that is
    /// empty): what Airbnb is asked to find when there is no centre (islands, regions).
    std::string place_name;
    /// True when the geocoders could not be reached (both failed for a candidate); the
    /// candidates after it were not tried.
    bool lookup_failed = false;
};

/// Geocodes `airport`'s candidates in order through `places` (its geocode.json memo and
/// 90-day cache, its lock; no notices) and returns the first hit that lies at most
/// kCityCentreMaxDistanceKm from the airport and `city_names::names_city` the candidate.
/// Stops at the first candidate whose lookup failed. Sends nothing for a station at
/// (0, 0), which the map gives for a missing position. Throws OperationCancelled when
/// `stop` fires; other errors that are not geocoder service failures propagate.
[[nodiscard]] CityCentreLookup find_city_centre(PlaceResolver& places, const Airport& airport, std::stop_token stop);

} // namespace wizz
