// ResultViews (03 §8).

#include "wizz/core/result_views.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>

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

// The first row of a group in SortByPrice order is the group's smallest row under
// price_less, the earliest in the input among equal ones (the sort is stable). One
// pass finds it without sorting every flight: a grouped grid view regroups all
// fares on every streamed flush (01 §8.6), so this runs on the UI thread (R-03).

std::vector<FlightResultPtr> cheapest_per_day(std::span<const FlightResultPtr> flights)
{
    std::map<Date, FlightResultPtr> firsts;  // ascending date order
    for (const FlightResultPtr& flight : flights) {
        const auto [found, added] = firsts.try_emplace(flight->date(), flight);
        if (!added && price_less(flight, found->second)) {
            found->second = flight;
        }
    }
    std::vector<FlightResultPtr> result;
    result.reserve(firsts.size());
    for (auto& [date, flight] : firsts) {
        result.push_back(std::move(flight));
    }
    return result;
}

std::vector<FlightResultPtr> cheapest_per_destination(std::span<const FlightResultPtr> flights)
{
    struct First {
        std::size_t position = 0;  ///< in the input: equal rows of different groups keep the input order
        FlightResultPtr flight;
    };
    std::map<std::string, First, std::less<>> firsts;  // ordinal, case-sensitive group keys
    for (std::size_t position = 0; position < flights.size(); ++position) {
        const FlightResultPtr& flight = flights[position];
        const auto [found, added] =
            firsts.try_emplace(flight->to_airport->group_key(), First{.position = position, .flight = flight});
        if (!added && price_less(flight, found->second.flight)) {
            found->second = First{.position = position, .flight = flight};
        }
    }
    std::vector<First> ordered;
    ordered.reserve(firsts.size());
    for (auto& [key, first] : firsts) {
        ordered.push_back(std::move(first));
    }
    std::ranges::sort(ordered, [](const First& a, const First& b) {
        if (price_less(a.flight, b.flight)) {
            return true;
        }
        return !price_less(b.flight, a.flight) && a.position < b.position;
    });
    std::vector<FlightResultPtr> result;
    result.reserve(ordered.size());
    for (First& first : ordered) {
        result.push_back(std::move(first.flight));
    }
    return result;
}

} // namespace wizz::result_views
