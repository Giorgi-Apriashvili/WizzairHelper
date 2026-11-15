#pragma once
// RoundTripCombiner (03 §10, 07 §3, DECISIONS D-12): pairs the fares of an
// outbound and a return leg into round trips.
//
//   match_legs        03 §10.2  keep only legs that belong together; refuse
//                               incompatible searches with the 01 §11.8 texts
//   same_day_feasible 03 §10.3  0-night trips: the return must leave after the
//                               outbound has landed (zone-aware estimate)
//   combine           03 §10.4  every pairing within the night window, ranked by
//                               the full 07 §3 key through a bounded max-heap
//   make_round_trip   03 §10.1  nights, lifted total, same home/away airport and
//                               the combined booking link
//
// Everything here is pure and thread-safe. Airport codes compare ordinally and
// case-insensitively (03 §0 `ci`, 07 §1). FlightResults are shared, never copied:
// a RoundTrip holds the very FlightResultPtr instances it was built from.

#include "wizz/core/domain.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/time.hpp"

#include <span>
#include <vector>

namespace wizz::round_trips {

/// The limit the UI always passes to `combine` (03 §10.4, 01 §11.8).
inline constexpr int kDefaultLimit = 500;

/// 03 §10.1 RoundTrip(outbound, inbound):
/// - `nights` = inbound date − outbound date (days, may be negative for odd input);
/// - `total` = outbound.total_converted + inbound.total_converted, nullopt when
///   either is nullopt (lifted decimal add; std::overflow_error like .NET);
/// - `same_home_airport` = outbound from IATA == inbound to IATA (ci);
/// - `same_away_airport` = outbound to IATA == inbound from IATA (ci);
/// - `combined_booking_url` (both flags true only) = the 03 §9 `Return` link
///   `…/select-flight/{OUT FROM}/{OUT TO}/{out date}/{return date}/{max(1, outbound adults)}/0/0/null`.
/// Throws std::invalid_argument when a flight or one of its airports is null.
[[nodiscard]] RoundTrip make_round_trip(FlightResultPtr outbound, FlightResultPtr inbound);

/// 03 §10.2 AwayMatches(a, area): some area airport has a's IATA (ci), or `a`
/// belongs to a real multi-airport city (`has_city_group`) and shares its mac
/// with an area airport (ci). Regional groups such as SPQ never match; null
/// area entries are ignored.
[[nodiscard]] bool away_matches(const Airport& airport, std::span<const AreaAirport> area) noexcept;

/// 03 §10.2 MatchLegs. Checks in this order, the first failure wins (problem
/// texts byte for byte as in 01 §11.8, request values printed raw):
/// 1. currency differs (ci) → "Outbound prices are in {out} but return prices in {ret}. Run both with the same currency."
/// 2. adults differ (raw)  → "Outbound was searched for {n} adult(s) but the return for {m}. Search both legs with the same number."
/// 3. fare type differs     → "Outbound and return were searched with different fare types (regular vs Discount Club). Search both the same way."
/// Then keeps outbound fares landing in the return's origin area (`away_matches`)
/// and return fares landing exactly (ci) at one of the outbound's origin
/// airports, both in leg order, and reports
/// 4. outbound fares but none kept → "The return was searched from {IATA, IATA…}, but none of the outbound fares land there. Search the return from your destination."
/// 5. return fares but none kept   → "None of the return fares lands at an airport near where the outbound starts. Search the return back to your home area."
/// A problem comes with empty lists. A leg without flights is no problem.
/// Flights with a null pointer or a null destination airport never match.
[[nodiscard]] LegMatch match_legs(const LegSearchResult& outbound, const LegSearchResult& inbound);

/// The intermediate values of the 03 §10.3 same-day rule for one outbound route.
struct SameDayRequirement {
    double distance_km = 0;        ///< distance_km(from, to) (02 §3.2 haversine)
    double flight_hours = 0;       ///< distance_km / 750 + 0.5
    double zone_offset_hours = 0;  ///< (to.lon − from.lon) / 15 rounded half to even (Math.Round, std::nearbyint)
    double required_hours = 0;     ///< max(2, flight_hours + zone_offset_hours + 3)
    /// `TimeSpan.FromHours(required_hours)`: ticks = required_hours × 36·10⁹
    /// truncated toward zero; Duration::max() if that is not finite.
    Duration required{};
};

/// The 03 §10.3 requirement for an outbound flight from `from` to `to`.
/// Examples (05 CT-RTR-04/07): BUD→LTN 1490.8137 km, 2.48775 h, zone −1,
/// required 4.48775 h; LTN→EVN 3627.2645 km, 5.33635 h, zone 3, required 11.33635 h.
[[nodiscard]] SameDayRequirement same_day_requirement(GeoPoint from, GeoPoint to) noexcept;

/// 03 §10.3 SameDayFeasible(o, r): `r.cheapest_departure − o.cheapest_departure
/// >= same_day_requirement(o.from, o.to).required`, a naive wall-clock difference
/// (no time-zone conversion; only the outbound's airports are used; exactly the
/// requirement counts as feasible). Throws std::invalid_argument when an
/// airport of `o` is null.
[[nodiscard]] bool same_day_feasible(const FlightResult& outbound, const FlightResult& inbound);

/// 03 §10.4 Combine with the deterministic 07 §3 ranking (D-12).
/// - `limit <= 0` or `max_nights < max(0, min_nights)` → empty; a negative
///   `min_nights` counts as 0.
/// - Eligible flights: non-null, both airports set, available, total_converted set.
/// - For each eligible outbound (input order), for each night count n in
///   [min, max] ascending, for each eligible return dated outbound date + n
///   (that day's returns stable-sorted by total, cheapest first): skip n == 0
///   pairs that fail `same_day_feasible`, then pairs failing a requested
///   same-home / same-away flag; every other pair is a candidate, numbered in
///   this generation order.
/// - Ranking key, ascending: total, outbound date, nights, outbound cheapest
///   departure, return cheapest departure, outbound from / to IATA, return
///   from / to IATA (ci), generation number. The key is a strict total order,
///   used for admission and eviction in a max-heap of capacity `limit` and for
///   the final sort, so the result with any limit N is exactly the first N
///   trips of the uncapped result.
[[nodiscard]] std::vector<RoundTrip> combine(std::span<const FlightResultPtr> outbound,
                                             std::span<const FlightResultPtr> inbound, int min_nights, int max_nights,
                                             int limit = kDefaultLimit, bool require_same_home = false,
                                             bool require_same_away = false);

} // namespace wizz::round_trips
