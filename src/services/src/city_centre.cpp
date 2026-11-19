// FindCityCentre (DECISIONS D-72).

#include "wizz/services/city_centre.hpp"

#include "wizz/core/city_names.hpp"
#include "wizz/core/text.hpp"

namespace wizz {

CityCentreLookup find_city_centre(PlaceResolver& places, const Airport& airport, std::stop_token stop)
{
    CityCentreLookup result;
    result.place_name = city_names::clean_name(airport.name);
    if (result.place_name.empty()) {
        result.place_name = std::string{text::trim(airport.name)};
    }
    if (airport.location == GeoPoint{}) {
        return result;  // no position to check the hits against
    }
    for (const std::string& candidate : city_names::candidates(airport.name)) {
        const PlaceResolver::GeocodeResult found = places.geocode(candidate, NoticeSink{}, stop);
        for (const PlaceResolver::PlaceDto& hit : found.found) {
            const GeoPoint location{.latitude = hit.lat, .longitude = hit.lon};
            if (distance_km(airport.location, location) <= kCityCentreMaxDistanceKm &&
                city_names::names_city(hit.name, candidate)) {
                result.centre = CityCentre{.name = hit.name, .location = location};
                return result;
            }
        }
        if (found.failed) {
            result.lookup_failed = true;  // offline: the shorter names would fail too
            return result;
        }
    }
    return result;
}

} // namespace wizz
