// AirbnbLinks (DECISIONS D-72).

#include "wizz/core/airbnb_links.hpp"

#include "wizz/core/http.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace wizz::airbnb_links {

namespace {

constexpr double kMinCosine = 0.01;

[[nodiscard]] std::string stay_parameters(Date check_in, Date check_out, int adults)
{
    return std::format("checkin={}&checkout={}&adults={}", check_in.iso(), check_out.iso(), std::max(1, adults));
}

} // namespace

MapBox box_around(GeoPoint centre, double radius_km) noexcept
{
    const double latitude_span = radius_km / kKmPerDegree;
    const double cosine = std::max(kMinCosine, std::cos(centre.latitude * std::numbers::pi / 180.0));
    const double longitude_span = radius_km / (kKmPerDegree * cosine);
    return MapBox{
        .north_east = {.latitude = centre.latitude + latitude_span, .longitude = centre.longitude + longitude_span},
        .south_west = {.latitude = centre.latitude - latitude_span, .longitude = centre.longitude - longitude_span},
    };
}

std::string homes_in_box(const MapBox& box, Date check_in, Date check_out, int adults)
{
    return std::format("{}?{}&ne_lat={:.5f}&ne_lng={:.5f}&sw_lat={:.5f}&sw_lng={:.5f}&search_by_map=true",
                       kSearchBase, stay_parameters(check_in, check_out, adults), box.north_east.latitude,
                       box.north_east.longitude, box.south_west.latitude, box.south_west.longitude);
}

std::string homes_by_name(std::string_view place, Date check_in, Date check_out, int adults)
{
    return std::format("{}?query={}&{}", kSearchBase, http::escape_data_string(place),
                       stay_parameters(check_in, check_out, adults));
}

std::string place_query(std::string_view name, std::string_view country)
{
    const std::string_view trimmed_name = text::trim(name);
    const std::string_view trimmed_country = text::trim(country);
    if (trimmed_country.empty() || text::ordinal_ignore_case_equals(trimmed_name, trimmed_country)) {
        return std::string{trimmed_name};
    }
    return std::format("{}, {}", trimmed_name, trimmed_country);
}

} // namespace wizz::airbnb_links
