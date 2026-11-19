#pragma once
// AirbnbLinks (DECISIONS D-72): Airbnb's own search page for the homes around a
// destination, opened in the user's browser like the booking links. The app never
// calls Airbnb. Airbnb documents no search URL; these are the parameters its site
// uses (checked by hand, D-72), so a change on Airbnb's side only loosens the
// filter: the page still opens. Dates are invariant `yyyy-MM-dd`, adults at least 1.

#include "wizz/core/geo.hpp"
#include "wizz/core/time.hpp"

#include <string>
#include <string_view>

namespace wizz::airbnb_links {

/// Airbnb's search page for homes.
inline constexpr std::string_view kSearchBase = "https://www.airbnb.com/s/homes";
/// Kilometres in one degree of latitude (and of longitude at the equator).
inline constexpr double kKmPerDegree = 111.32;

/// The map area of a search: its north-east and south-west corners.
struct MapBox {
    GeoPoint north_east;
    GeoPoint south_west;

    friend constexpr bool operator==(const MapBox&, const MapBox&) = default;
};

/// The area reaching `radius_km` north, south, east and west of `centre`:
/// ± radius / kKmPerDegree degrees of latitude and ± radius / (kKmPerDegree ·
/// cos latitude) of longitude (cos taken at least 0.01 near the poles).
[[nodiscard]] MapBox box_around(GeoPoint centre, double radius_km) noexcept;

/// `{kSearchBase}?checkin={in}&checkout={out}&adults={n}&ne_lat=…&ne_lng=…&sw_lat=…&sw_lng=…&search_by_map=true`,
/// the coordinates with five decimals (about a metre).
[[nodiscard]] std::string homes_in_box(const MapBox& box, Date check_in, Date check_out, int adults);

/// `{kSearchBase}?query={place, escaped}&checkin={in}&checkout={out}&adults={n}`: Airbnb
/// finds the place and chooses the area itself (islands and regions, D-72).
[[nodiscard]] std::string homes_by_name(std::string_view place, Date check_in, Date check_out, int adults);

/// "{name}, {country}"; just the name when the country is empty or equal to it
/// (ignoring ASCII case: "Malta").
[[nodiscard]] std::string place_query(std::string_view name, std::string_view country);

} // namespace wizz::airbnb_links
