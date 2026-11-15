#pragma once
// ResultViews (03 §8): the grid orderings of a leg's flights. Every function
// returns a new list and sorts stably (LINQ OrderBy/ThenBy), so remaining ties
// keep the input order — the first-insertion order of the (From, To, Date) key
// in a search, the previous order after a re-price (03 §5.5, §6.3).
// Inputs must hold non-null results with non-null airports (as build_result makes them).

#include "wizz/core/domain.hpp"

#include <span>
#include <vector>

namespace wizz::result_views {

/// SortByPrice: stable sort by (available ? 0 : 1, TotalConverted ?? DECIMAL_MAX,
/// Date, Fare.CheapestDeparture). CheckOnSite rows come last; available rows
/// without a converted price follow every converted one (07 §2: never ranked as
/// zero-priced tickets).
[[nodiscard]] std::vector<FlightResultPtr> sort_by_price(std::span<const FlightResultPtr> flights);

/// CheapestPerDay: the first row of each date in SortByPrice order (one row per
/// day, also when that day has only CheckOnSite or unconverted rows), returned
/// in ascending date order.
[[nodiscard]] std::vector<FlightResultPtr> cheapest_per_day(std::span<const FlightResultPtr> flights);

/// CheapestPerDestination: the first row of each arrival group
/// (`ToAirport.GroupKey`, ordinal case-sensitive equality) in SortByPrice order,
/// returned cheapest first. LTN and LGW share the LON group.
[[nodiscard]] std::vector<FlightResultPtr> cheapest_per_destination(std::span<const FlightResultPtr> flights);

} // namespace wizz::result_views
