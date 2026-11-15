#pragma once
// Geographic points and the great-circle distance every radius rule depends on
// (02 §2.2, §3). The haversine is reproduced operation for operation because
// thresholds such as "≤ radius", "< 30 km" and "≤ 150 km" depend on the last bit.

#include <span>

namespace wizz {

/// Decimal degrees, latitude north-positive, longitude east-positive. Nothing
/// validates the range. The default (0, 0) is a real value, not "missing" (02 §2.2).
struct GeoPoint {
    double latitude = 0.0;
    double longitude = 0.0;

    friend constexpr bool operator==(const GeoPoint&, const GeoPoint&) = default;
};

/// IUGG mean Earth radius (02 §3.1).
inline constexpr double kEarthRadiusKm = 6371.0088;

/// Haversine distance in km (02 §3.2), evaluated exactly as
/// `Rad(d) = d * PI / 180`,
/// `h = sin(dLat/2)*sin(dLat/2) + cos(Rad(a.lat))*cos(Rad(b.lat))*sin(dLon/2)*sin(dLon/2)`,
/// `6371.0088 * 2 * atan2(sqrt(h), sqrt(1 - h))`. `h` is not clamped.
[[nodiscard]] double distance_km(GeoPoint a, GeoPoint b) noexcept;

/// Arithmetic mean of the latitudes and, separately, of the longitudes, summed in
/// input order (02 §3.3). No antimeridian handling. Throws
/// std::invalid_argument("No points given.") for an empty span.
[[nodiscard]] GeoPoint centroid(std::span<const GeoPoint> points);

} // namespace wizz
