#pragma once
// Shared inputs of the UI-logic tests (01; 05 §7.2). They build real results
// with the core planner and pricing over the fixture map (CAT) and RATES, so
// the texts under test come from the same values the app would show. Locale
// en-US, TODAY = 2026-11-02 (05 §1.1, §1.11).

#include "wizz/core/catalog.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/fx_rates.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/planner.hpp"
#include "wizz/core/pricing.hpp"
#include "wizz/core/result_views.hpp"
#include "wizz/core/time.hpp"

#include "core/fare_samples.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wizz::test::ui {

using samples::cat;
using samples::day;
using samples::dec;
using samples::place;
using samples::rates;
using samples::station;

inline constexpr Date kToday = Date::from_ymd(2026, 11, 2);

[[nodiscard]] inline const Locale& en() { return *Locale::en_us(); }

/// A LegSearchResult whose plan is SearchPlanner.Plan(CAT, request, TODAY).
[[nodiscard]] inline LegSearchResult make_leg(LegSearchRequest request, std::vector<FlightResultPtr> flights,
                                              int requests_completed, Duration elapsed,
                                              std::vector<std::string> warnings = {})
{
    LegSearchResult result;
    result.plan = planner::plan(*cat(), request, kToday);
    result.request = std::move(request);
    result.flights = std::move(flights);
    result.warnings = std::move(warnings);
    result.rates = rates();
    result.requests_completed = requests_completed;
    result.elapsed = elapsed;
    return result;
}

/// The CT-FSS-01 / 03 §11 outbound leg: Budapest r200 → London r30, 10–20 Nov, 2 adults,
/// EUR, label "Outbound"; 12 fares in SortByPrice order, 3 of 3 requests, 1 route not served.
[[nodiscard]] inline LegSearchResult section11_outbound(Duration elapsed = std::chrono::seconds{2})
{
    LegSearchRequest request = samples::budapest_to_london();
    request.label = "Outbound";
    LegSearchResult result =
        make_leg(std::move(request), result_views::sort_by_price(samples::section11_flights()), 3, elapsed);
    result.routes_not_served = 1;
    return result;
}

/// One fare of the 03 §11 return leg (London r30 → Budapest r200, 12–20 Nov, 2 adults, EUR).
[[nodiscard]] inline LegSearchRequest section11_return_request()
{
    LegSearchRequest request;
    request.origin = place("London");
    request.origin_radius_km = 30;
    request.destination = place("Budapest");
    request.destination_radius_km = 200;
    request.from = day(11, 12);
    request.to = day(11, 20);
    request.adults = 2;
    request.currency = "EUR";
    request.label = "Return";
    return request;
}

/// The 03 §11 return leg with its synthetic GBP fares (SortByPrice order).
[[nodiscard]] inline LegSearchResult section11_return()
{
    const LegSearchRequest request = section11_return_request();
    std::vector<FlightResultPtr> flights;
    for (DayFare fare : {samples::day_fare("LTN", "BUD", day(11, 17), {21, 35}, {{21, 35}}, "17.99", "GBP"),
                         samples::day_fare("LTN", "BTS", day(11, 15), {12, 0}, {{12, 0}}, "19.99", "GBP"),
                         samples::day_fare("LTN", "BUD", day(11, 14), {10, 5}, {{10, 5}}, "24.99", "GBP"),
                         samples::day_fare("LGW", "BUD", day(11, 16), {14, 0}, {{14, 0}}, "29.99", "GBP")}) {
        flights.push_back(pricing::build_result(*cat(), request, *rates(), fare));
    }
    return make_leg(request, result_views::sort_by_price(flights), 4, std::chrono::seconds{3});
}

/// A FlightResult built field by field (for texts that need airports outside the fixture map).
[[nodiscard]] inline FlightResultPtr hand_fare(AirportPtr from, AirportPtr to, Date date, int hour, int minute,
                                               std::string_view amount, std::string currency,
                                               std::optional<std::string_view> total)
{
    FlightResult flight;
    flight.fare.from = from->iata;
    flight.fare.to = to->iata;
    flight.fare.date = date;
    flight.fare.cheapest_departure = make_local(date, hour, minute);
    flight.fare.departures = {flight.fare.cheapest_departure};
    flight.fare.price_per_person = Money{dec(amount), std::move(currency)};
    flight.from_airport = std::move(from);
    flight.to_airport = std::move(to);
    flight.adults = 1;
    flight.target_currency = "EUR";
    if (total) {
        flight.price_per_person_converted = dec(*total);
        flight.total_converted = dec(*total);
    }
    flight.booking_url = "https://www.wizzair.com/en-gb/booking/select-flight/" + flight.fare.from + "/" +
                         flight.fare.to + "/" + date.iso() + "/null/1/0/0/null";
    return std::make_shared<const FlightResult>(std::move(flight));
}

/// An Airport with just a code and a name (hand-built texts).
[[nodiscard]] inline AirportPtr airport(std::string iata, std::string name)
{
    Airport a;
    a.iata = std::move(iata);
    a.name = std::move(name);
    return std::make_shared<const Airport>(std::move(a));
}

} // namespace wizz::test::ui
