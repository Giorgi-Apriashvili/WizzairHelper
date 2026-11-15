#pragma once
// Shared inputs of the pricing and result-view tests: CAT and RATES (05 §0.3),
// the timetable rows of 05 §2.4 and the inline BTS body B1 (05 §2.7) built by
// hand as DayFares (the timetable parser is tested on its own), the request of
// the 03 §11 worked example, and fare builder A (05 §1.7). Locale en-US.

#include "wizz/core/catalog.hpp"
#include "wizz/core/decimal.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/fx_rates.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/pricing.hpp"
#include "wizz/core/time.hpp"

#include "support/fixtures.hpp"

#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wizz::test::samples {

/// CAT: the catalog of fixtures/map_subset.json under en-US, built once.
[[nodiscard]] inline const AirportCatalogPtr& cat()
{
    static const AirportCatalogPtr catalog =
        AirportCatalog::from_map_json(read_fixture("map_subset.json"), Locale::en_us());
    return catalog;
}

/// RATES: ParseFrankfurterV2(fx_frankfurter_v2.json), built once.
[[nodiscard]] inline const FxRatesPtr& rates()
{
    static const FxRatesPtr parsed =
        std::make_shared<const FxRates>(fx_rates::parse_frankfurter_v2(read_fixture("fx_frankfurter_v2.json")));
    return parsed;
}

/// A decimal from its JSON lexeme, scale kept ("30790.0").
[[nodiscard]] inline Decimal dec(std::string_view lexeme)
{
    const auto parsed = Decimal::from_json_number(lexeme);
    if (!parsed) {
        throw std::logic_error("bad decimal literal in a test");
    }
    return *parsed;
}

[[nodiscard]] constexpr Date day(unsigned month, unsigned day_of_month, int year = 2026) noexcept
{
    return Date::from_ymd(year, month, day_of_month);
}

/// CAT.MatchPlace(text); throws when the fixture map has no such place.
[[nodiscard]] inline Place place(std::string_view text)
{
    std::optional<Place> found = cat()->match_place(text);
    if (!found) {
        throw std::logic_error("fixture place missing");
    }
    return *std::move(found);
}

/// The catalog's station `code`; throws when missing.
[[nodiscard]] inline AirportPtr station(std::string_view code)
{
    AirportPtr airport = cat()->find(code);
    if (!airport) {
        throw std::logic_error("fixture station missing");
    }
    return airport;
}

using HourMinute = std::pair<int, int>;

/// One DayFare with the cheapest departure at `cheapest` and the given departures of that day.
[[nodiscard]] inline DayFare day_fare(std::string from, std::string to, Date date, HourMinute cheapest,
                                      std::initializer_list<HourMinute> departures, std::string_view amount,
                                      std::string currency, FareStatus status = FareStatus::available,
                                      bool has_mac_flight = false)
{
    DayFare fare;
    fare.from = std::move(from);
    fare.to = std::move(to);
    fare.date = date;
    fare.cheapest_departure = make_local(date, cheapest.first, cheapest.second);
    for (const HourMinute& departure : departures) {
        fare.departures.push_back(make_local(date, departure.first, departure.second));
    }
    fare.price_per_person = Money{dec(amount), std::move(currency)};
    fare.status = status;
    fare.has_mac_flight = has_mac_flight;
    return fare;
}

/// timetable_v2_bud_ltn.json (05 §2.4), the 11 rows in response order, HUF.
[[nodiscard]] inline std::vector<DayFare> bud_ltn_rows()
{
    return {
        day_fare("BUD", "LTN", day(11, 10), {6, 0}, {{6, 0}, {17, 30}}, "30790.0", "HUF", FareStatus::available, false),
        day_fare("BUD", "LTN", day(11, 11), {6, 0}, {{6, 0}, {8, 30}, {17, 30}}, "21590.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LTN", day(11, 12), {6, 0}, {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, "21590.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LTN", day(11, 13), {12, 50}, {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, "21590.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LTN", day(11, 14), {6, 0}, {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, "19290.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LGW", day(11, 15), {8, 30}, {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, "34690.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LTN", day(11, 16), {6, 0}, {{6, 0}, {8, 30}, {17, 30}}, "25390.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LTN", day(11, 17), {6, 0}, {{6, 0}, {17, 30}}, "21590.0", "HUF", FareStatus::available, false),
        day_fare("BUD", "LTN", day(11, 18), {6, 0}, {{6, 0}, {8, 30}, {17, 30}}, "19290.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LTN", day(11, 19), {12, 50}, {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, "19290.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LTN", day(11, 20), {6, 0}, {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, "19290.0", "HUF",
                 FareStatus::available, true),
    };
}

/// B1 (05 §2.7): BTS→LTN 2026-11-12 07:15, 14.99 EUR.
[[nodiscard]] inline DayFare b1_row()
{
    return day_fare("BTS", "LTN", day(11, 12), {7, 15}, {{7, 15}}, "14.99", "EUR");
}

/// timetable_checkprice.json (05 §2.4), the 4 rows in response order, HUF.
[[nodiscard]] inline std::vector<DayFare> checkprice_rows()
{
    return {
        day_fare("BUD", "LTN", day(11, 2), {17, 30}, {{17, 30}}, "0.0", "HUF", FareStatus::check_on_site, false),
        day_fare("BUD", "LTN", day(11, 3), {17, 30}, {{6, 0}, {8, 30}, {17, 30}}, "44290.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LGW", day(11, 4), {8, 30}, {{6, 0}, {8, 30}, {17, 30}}, "38490.0", "HUF",
                 FareStatus::available, true),
        day_fare("BUD", "LTN", day(11, 5), {6, 0}, {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, "30790.0", "HUF",
                 FareStatus::available, true),
    };
}

/// The 03 §11 / CT-FSS-01 request: Budapest r200 → London r30, 10–20 Nov 2026, 2 adults, EUR.
[[nodiscard]] inline LegSearchRequest budapest_to_london()
{
    LegSearchRequest request;
    request.origin = place("Budapest");
    request.origin_radius_km = 200;
    request.destination = place("London");
    request.destination_radius_km = 30;
    request.from = day(11, 10);
    request.to = day(11, 20);
    request.adults = 2;
    request.currency = "EUR";
    return request;
}

/// The plan's destination airports of that request (03 §11 Plan step 2: the pins LGW and LTN).
[[nodiscard]] inline pricing::CodeSet london_destination_iatas()
{
    return pricing::CodeSet{"LGW", "LTN"};
}

/// The 12 results of 03 §11 in insertion order (the BUD response, then the BTS one),
/// each built with BuildResult(CAT, request, RATES, fare, missing, destIatas).
[[nodiscard]] inline std::vector<FlightResultPtr> section11_flights(pricing::OrderedCodeSet* missing = nullptr)
{
    const LegSearchRequest request = budapest_to_london();
    const pricing::CodeSet destinations = london_destination_iatas();
    std::vector<FlightResultPtr> flights;
    for (const DayFare& fare : bud_ltn_rows()) {
        flights.push_back(pricing::build_result(*cat(), request, *rates(), fare, missing, &destinations));
    }
    flights.push_back(pricing::build_result(*cat(), request, *rates(), b1_row(), missing, &destinations));
    return flights;
}

/// Fare builder A (05 §1.7, `RoundTripTests.Fare`): Origin = CAT.MatchPlace(from),
/// From = To = date, EUR, 1 adult, anywhere; the fare departs at 06:00 for `eur` EUR.
[[nodiscard]] inline FlightResultPtr builder_a(std::string_view from, std::string_view to, Date date,
                                               std::string_view eur)
{
    LegSearchRequest request;
    request.origin = place(from);
    request.from = date;
    request.to = date;
    request.currency = "EUR";
    DayFare fare;
    fare.from = std::string{from};
    fare.to = std::string{to};
    fare.date = date;
    fare.cheapest_departure = make_local(date, 6, 0);
    fare.price_per_person = Money{dec(eur), "EUR"};
    fare.status = FareStatus::available;
    return pricing::build_result(*cat(), request, *rates(), fare);
}

} // namespace wizz::test::samples
