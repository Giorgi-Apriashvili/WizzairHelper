#include "wizz/core/geo.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace wizz {

namespace {

/// Degrees to radians: multiply first, then divide (Math.PI / 180.0 would round differently).
[[nodiscard]] double to_radians(double degrees) noexcept
{
    return degrees * std::numbers::pi / 180.0;
}

} // namespace

double distance_km(GeoPoint a, GeoPoint b) noexcept
{
    const double d_lat = to_radians(b.latitude - a.latitude);
    const double d_lon = to_radians(b.longitude - a.longitude);
    const double h = std::sin(d_lat / 2) * std::sin(d_lat / 2) +
                     std::cos(to_radians(a.latitude)) * std::cos(to_radians(b.latitude)) * std::sin(d_lon / 2) *
                         std::sin(d_lon / 2);
    return kEarthRadiusKm * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}

GeoPoint centroid(std::span<const GeoPoint> points)
{
    if (points.empty()) {
        throw std::invalid_argument("No points given.");
    }
    // Enumerable.Average: sequential double sums divided by the count.
    double latitude_sum = 0.0;
    double longitude_sum = 0.0;
    for (const GeoPoint& point : points) {
        latitude_sum += point.latitude;
        longitude_sum += point.longitude;
    }
    const auto count = static_cast<double>(points.size());
    return GeoPoint{latitude_sum / count, longitude_sum / count};
}

} // namespace wizz
