// RoundTripCombiner: MatchLegs, SameDayFeasible, Combine (03 §10, 07 §3, D-12).

#include "wizz/core/round_trips.hpp"

#include "wizz/core/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace wizz::round_trips {

namespace {

using text::ordinal_ignore_case_compare;
using text::ordinal_ignore_case_equals;

/// 03 §9 `Base`.
constexpr std::string_view kBookingBase = "https://www.wizzair.com/en-gb/booking/select-flight";

/// .NET TimeSpan.TicksPerHour.
constexpr double kTicksPerHour = 36'000'000'000.0;

/// 03 §9 `BookingLinks.Return(from, to, outDate, returnDate, adults)`:
/// `{Base}/{upper(from)}/{upper(to)}/{outDate:yyyy-MM-dd}/{returnDate:yyyy-MM-dd}/{max(1,adults)}/0/0/null`.
/// A private copy: booking_links.hpp is a separate module; both must produce the
/// same text (CT-LNK-01 pins the format there, CT-RT-01 here).
[[nodiscard]] std::string return_link(std::string_view from, std::string_view to, Date out_date, Date return_date,
                                      int adults)
{
    return std::format("{}/{}/{}/{}/{}/{}/0/0/null", kBookingBase, text::to_upper_invariant(from),
                       text::to_upper_invariant(to), out_date.iso(), return_date.iso(), std::max(1, adults));
}

[[nodiscard]] const Airport& airport_or_throw(const AirportPtr& airport, std::string_view role)
{
    if (!airport) {
        throw std::invalid_argument(std::format("Round trip: the {} airport is missing.", role));
    }
    return *airport;
}

[[nodiscard]] LegMatch problem(std::string text)
{
    return LegMatch{.outbound = {}, .inbound = {}, .problem = std::move(text)};
}

/// Eligible for pairing (03 §10.4 filter plus the null guards of the header).
[[nodiscard]] bool is_eligible(const FlightResultPtr& flight) noexcept
{
    return flight && flight->from_airport && flight->to_airport && flight->is_available() &&
           flight->total_converted.has_value();
}

/// One admitted candidate. The pointers address elements of the eligible lists,
/// which stay unmodified (and so in place) while `combine` runs.
struct Candidate {
    const FlightResultPtr* outbound = nullptr;
    const FlightResultPtr* inbound = nullptr;
    Decimal total;
    std::int64_t nights = 0;
    std::uint64_t sequence = 0;
};

/// The 07 §3 ranking key as a three-way comparison; never equivalent for two
/// different candidates thanks to the generation sequence.
[[nodiscard]] std::strong_ordering rank_order(const Candidate& a, const Candidate& b) noexcept
{
    if (const std::weak_ordering c = a.total <=> b.total; c != 0) {
        return c < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    const FlightResult& ao = **a.outbound;
    const FlightResult& bo = **b.outbound;
    const FlightResult& ar = **a.inbound;
    const FlightResult& br = **b.inbound;
    if (const auto c = ao.date() <=> bo.date(); c != 0) {
        return c;
    }
    if (const auto c = a.nights <=> b.nights; c != 0) {
        return c;
    }
    if (const auto c = ao.fare.cheapest_departure <=> bo.fare.cheapest_departure; c != 0) {
        return c;
    }
    if (const auto c = ar.fare.cheapest_departure <=> br.fare.cheapest_departure; c != 0) {
        return c;
    }
    const std::array<std::string_view, 4> codes_a{ao.from_airport->iata, ao.to_airport->iata,
                                                  ar.from_airport->iata, ar.to_airport->iata};
    const std::array<std::string_view, 4> codes_b{bo.from_airport->iata, bo.to_airport->iata,
                                                  br.from_airport->iata, br.to_airport->iata};
    for (std::size_t i = 0; i < codes_a.size(); ++i) {
        if (const int c = ordinal_ignore_case_compare(codes_a[i], codes_b[i]); c != 0) {
            return c < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
    }
    return a.sequence <=> b.sequence;
}

struct RanksBefore {
    [[nodiscard]] bool operator()(const Candidate& a, const Candidate& b) const noexcept
    {
        return rank_order(a, b) < 0;
    }
};

/// 03 §10.4 "bounded MAX-heap on the full trip key": the front is the worst
/// admitted candidate. A candidate enters while there is room, or replaces the
/// worst one when it ranks strictly before it.
class BoundedTripHeap {
public:
    explicit BoundedTripHeap(std::size_t capacity) noexcept : capacity_(capacity) {}

    void offer(const Candidate& candidate)
    {
        if (entries_.size() < capacity_) {
            entries_.push_back(candidate);
            std::ranges::push_heap(entries_, RanksBefore{});
        } else if (!entries_.empty() && RanksBefore{}(candidate, entries_.front())) {
            std::ranges::pop_heap(entries_, RanksBefore{});
            entries_.back() = candidate;
            std::ranges::push_heap(entries_, RanksBefore{});
        }
    }

    /// The admitted candidates, best first.
    [[nodiscard]] std::vector<Candidate> take_sorted() &&
    {
        std::ranges::sort_heap(entries_, RanksBefore{});
        return std::move(entries_);
    }

private:
    std::size_t capacity_;
    std::vector<Candidate> entries_;
};

} // namespace

RoundTrip make_round_trip(FlightResultPtr outbound, FlightResultPtr inbound)
{
    if (!outbound || !inbound) {
        throw std::invalid_argument("Round trip: a flight is missing.");
    }
    const Airport& out_from = airport_or_throw(outbound->from_airport, "outbound departure");
    const Airport& out_to = airport_or_throw(outbound->to_airport, "outbound arrival");
    const Airport& ret_from = airport_or_throw(inbound->from_airport, "return departure");
    const Airport& ret_to = airport_or_throw(inbound->to_airport, "return arrival");

    RoundTrip trip;
    trip.nights = static_cast<int>(days_between(inbound->date(), outbound->date()));
    if (outbound->total_converted && inbound->total_converted) {
        trip.total = *outbound->total_converted + *inbound->total_converted;
    }
    trip.same_home_airport = ordinal_ignore_case_equals(out_from.iata, ret_to.iata);
    trip.same_away_airport = ordinal_ignore_case_equals(out_to.iata, ret_from.iata);
    if (trip.same_home_airport && trip.same_away_airport) {
        trip.combined_booking_url =
            return_link(out_from.iata, out_to.iata, outbound->date(), inbound->date(), outbound->adults);
    }
    trip.outbound = std::move(outbound);
    trip.inbound = std::move(inbound);
    return trip;
}

bool away_matches(const Airport& airport, std::span<const AreaAirport> area) noexcept
{
    return std::ranges::any_of(area, [&airport](const AreaAirport& entry) {
        if (!entry.airport) {
            return false;
        }
        const Airport& x = *entry.airport;
        return ordinal_ignore_case_equals(x.iata, airport.iata) ||
               (airport.mac && x.mac && airport.has_city_group && ordinal_ignore_case_equals(*x.mac, *airport.mac));
    });
}

LegMatch match_legs(const LegSearchResult& outbound, const LegSearchResult& inbound)
{
    const LegSearchRequest& out_request = outbound.request;
    const LegSearchRequest& ret_request = inbound.request;
    if (!ordinal_ignore_case_equals(out_request.currency, ret_request.currency)) {
        return problem(std::format("Outbound prices are in {} but return prices in {}. Run both with the same currency.",
                                   out_request.currency, ret_request.currency));
    }
    if (out_request.adults != ret_request.adults) {
        return problem(std::format(
            "Outbound was searched for {} adult(s) but the return for {}. Search both legs with the same number.",
            out_request.adults, ret_request.adults));
    }
    if (out_request.price_type != ret_request.price_type) {
        return problem("Outbound and return were searched with different fare types (regular vs Discount Club). "
                       "Search both the same way.");
    }

    const std::span<const AreaAirport> away = inbound.plan.origin_airports;
    std::unordered_set<std::string_view, text::OrdinalIgnoreCaseHash, text::OrdinalIgnoreCaseEqual> home;
    for (const AreaAirport& entry : outbound.plan.origin_airports) {
        if (entry.airport) {
            home.insert(entry.airport->iata);
        }
    }

    LegMatch match;
    for (const FlightResultPtr& flight : outbound.flights) {
        if (flight && flight->to_airport && away_matches(*flight->to_airport, away)) {
            match.outbound.push_back(flight);
        }
    }
    for (const FlightResultPtr& flight : inbound.flights) {
        if (flight && flight->to_airport && home.contains(flight->to_airport->iata)) {
            match.inbound.push_back(flight);
        }
    }

    if (!outbound.flights.empty() && match.outbound.empty()) {
        std::string codes;
        for (const AreaAirport& entry : away) {
            if (entry.airport) {
                if (!codes.empty()) {
                    codes += ", ";
                }
                codes += entry.airport->iata;
            }
        }
        return problem(std::format("The return was searched from {}, but none of the outbound fares land there. "
                                   "Search the return from your destination.",
                                   codes));
    }
    if (!inbound.flights.empty() && match.inbound.empty()) {
        return problem("None of the return fares lands at an airport near where the outbound starts. "
                       "Search the return back to your home area.");
    }
    return match;
}

SameDayRequirement same_day_requirement(GeoPoint from, GeoPoint to) noexcept
{
    SameDayRequirement r;
    r.distance_km = distance_km(from, to);
    r.flight_hours = r.distance_km / 750.0 + 0.5;
    // Math.Round(double) is banker's rounding: std::nearbyint in the default
    // rounding mode, never std::round (half away from zero) — 07 §2, CT-RTR-04.
    r.zone_offset_hours = std::nearbyint((to.longitude - from.longitude) / 15.0);
    const double sum = r.flight_hours + r.zone_offset_hours + 3.0;
    r.required_hours = std::max(2.0, sum);
    const double ticks = r.required_hours * kTicksPerHour;
    // TimeSpan.FromHours truncates the double tick count toward zero.
    r.required = std::isfinite(ticks) && std::abs(ticks) < 9.0e18 ? Duration{static_cast<std::int64_t>(ticks)}
                                                                    : Duration::max();
    return r;
}

bool same_day_feasible(const FlightResult& outbound, const FlightResult& inbound)
{
    const Airport& from = airport_or_throw(outbound.from_airport, "outbound departure");
    const Airport& to = airport_or_throw(outbound.to_airport, "outbound arrival");
    const Duration gap = inbound.fare.cheapest_departure - outbound.fare.cheapest_departure;
    return gap >= same_day_requirement(from.location, to.location).required;
}

std::vector<RoundTrip> combine(std::span<const FlightResultPtr> outbound, std::span<const FlightResultPtr> inbound,
                               int min_nights, int max_nights, int limit, bool require_same_home,
                               bool require_same_away)
{
    const std::int64_t min_n = std::max(0, min_nights);
    const std::int64_t max_n = max_nights;
    if (limit <= 0 || max_n < min_n) {
        return {};
    }

    std::vector<FlightResultPtr> outs;
    for (const FlightResultPtr& flight : outbound) {
        if (is_eligible(flight)) {
            outs.push_back(flight);
        }
    }
    // Returns grouped by day number; each day stable-sorted by total, cheapest first.
    std::map<std::int64_t, std::vector<FlightResultPtr>> returns_by_day;
    for (const FlightResultPtr& flight : inbound) {
        if (is_eligible(flight)) {
            returns_by_day[flight->date().day_number()].push_back(flight);
        }
    }
    for (auto& [day, flights] : returns_by_day) {
        std::ranges::stable_sort(flights, std::ranges::less{},
                                 [](const FlightResultPtr& f) -> const Decimal& { return *f->total_converted; });
    }

    BoundedTripHeap heap{static_cast<std::size_t>(limit)};
    std::uint64_t sequence = 0;
    for (const FlightResultPtr& o : outs) {
        const std::int64_t out_day = o->date().day_number();
        std::optional<Duration> same_day_required;  // computed once per outbound, on demand
        // The day-ordered map walk visits n = min..max ascending, skipping empty days.
        const auto first = returns_by_day.lower_bound(out_day + min_n);
        const auto last = returns_by_day.upper_bound(out_day + max_n);
        for (auto day = first; day != last; ++day) {
            const std::int64_t nights = day->first - out_day;
            for (const FlightResultPtr& r : day->second) {
                if (nights == 0) {
                    if (!same_day_required) {
                        same_day_required =
                            same_day_requirement(o->from_airport->location, o->to_airport->location).required;
                    }
                    if (r->fare.cheapest_departure - o->fare.cheapest_departure < *same_day_required) {
                        continue;
                    }
                }
                if (require_same_home && !ordinal_ignore_case_equals(o->from_airport->iata, r->to_airport->iata)) {
                    continue;
                }
                if (require_same_away && !ordinal_ignore_case_equals(o->to_airport->iata, r->from_airport->iata)) {
                    continue;
                }
                heap.offer(Candidate{.outbound = &o,
                                     .inbound = &r,
                                     .total = *o->total_converted + *r->total_converted,
                                     .nights = nights,
                                     .sequence = sequence++});
            }
        }
    }

    const std::vector<Candidate> ranked = std::move(heap).take_sorted();
    std::vector<RoundTrip> trips;
    trips.reserve(ranked.size());
    for (const Candidate& candidate : ranked) {
        trips.push_back(make_round_trip(*candidate.outbound, *candidate.inbound));
    }
    return trips;
}

} // namespace wizz::round_trips
