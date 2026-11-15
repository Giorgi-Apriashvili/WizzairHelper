// ResultViews (03 §8).

#include "wizz/core/result_views.hpp"

#include <algorithm>
#include <compare>
#include <functional>
#include <set>
#include <string>

namespace wizz::result_views {

namespace {

/// 03 §8 key 2: `TotalConverted ?? DECIMAL_MAX`.
[[nodiscard]] Decimal total_or_max(const FlightResult& flight)
{
    return flight.total_converted.value_or(Decimal::max_value());
}

/// The SortByPrice key comparison; equal keys compare false (stable sort keeps input order).
[[nodiscard]] bool price_less(const FlightResultPtr& a, const FlightResultPtr& b)
{
    const int rank_a = a->is_available() ? 0 : 1;
    const int rank_b = b->is_available() ? 0 : 1;
    if (rank_a != rank_b) {
        return rank_a < rank_b;
    }
    if (const std::weak_ordering order = total_or_max(*a) <=> total_or_max(*b); order != 0) {
        return order < 0;
    }
    if (a->date() != b->date()) {
        return a->date() < b->date();
    }
    return a->fare.cheapest_departure < b->fare.cheapest_departure;
}

} // namespace

std::vector<FlightResultPtr> sort_by_price(std::span<const FlightResultPtr> flights)
{
    std::vector<FlightResultPtr> sorted(flights.begin(), flights.end());
    std::ranges::stable_sort(sorted, price_less);
    return sorted;
}

std::vector<FlightResultPtr> cheapest_per_day(std::span<const FlightResultPtr> flights)
{
    std::vector<FlightResultPtr> firsts;
    std::set<Date> seen;
    for (const FlightResultPtr& flight : sort_by_price(flights)) {
        if (seen.insert(flight->date()).second) {
            firsts.push_back(flight);
        }
    }
    std::vector<FlightResultPtr> result = sort_by_price(firsts);
    std::ranges::stable_sort(result, std::ranges::less{}, [](const FlightResultPtr& flight) { return flight->date(); });
    return result;
}

std::vector<FlightResultPtr> cheapest_per_destination(std::span<const FlightResultPtr> flights)
{
    std::vector<FlightResultPtr> firsts;
    std::set<std::string, std::less<>> seen; // ordinal, case-sensitive group keys
    for (const FlightResultPtr& flight : sort_by_price(flights)) {
        if (seen.insert(flight->to_airport->group_key()).second) {
            firsts.push_back(flight);
        }
    }
    return sort_by_price(firsts);
}

} // namespace wizz::result_views
