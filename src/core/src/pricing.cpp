// Pricing: BuildResult and Reprice (03 §6.1–§6.3, 02 §2.11).

#include "wizz/core/pricing.hpp"

#include "wizz/core/booking_links.hpp"
#include "wizz/core/catalog.hpp"
#include "wizz/core/currency_format.hpp"
#include "wizz/core/fx_rates.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/result_views.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace wizz::pricing {

namespace {

/// Reprice applied in place (03 §6.2): convert, round per person, × adults.
void apply_prices(FlightResult& flight, const FxRates& rates, std::string_view currency)
{
    std::string target = text::to_upper_invariant(currency);
    std::optional<Decimal> per_person;
    if (flight.is_available()) {
        const Money& price = flight.fare.price_per_person;
        if (const std::optional<Decimal> converted = fx_rates::convert(rates, price.amount, price.currency, target)) {
            per_person = converted->round(currency_format::decimals(target), Rounding::away_from_zero);
        }
    }
    flight.target_currency = std::move(target);
    flight.price_per_person_converted = per_person;
    flight.total_converted = per_person ? std::optional<Decimal>{*per_person * flight.adults} : std::nullopt;
}

[[nodiscard]] AirportPtr find_or_stub(const AirportCatalog& catalog, const std::string& code)
{
    if (AirportPtr airport = catalog.find(code)) {
        return airport;
    }
    return unknown_airport(code);
}

} // namespace

CodeSet iata_set(std::span<const AreaAirport> airports)
{
    CodeSet codes;
    for (const AreaAirport& entry : airports) {
        codes.insert(entry.airport->iata);
    }
    return codes;
}

bool OrderedCodeSet::add(std::string_view code)
{
    if (!seen_.emplace(code).second) {
        return false;
    }
    items_.emplace_back(code);
    return true;
}

AirportPtr unknown_airport(std::string_view iata)
{
    Airport stub;
    stub.iata = std::string{iata};
    stub.name = std::string{iata};
    return std::make_shared<const Airport>(std::move(stub));
}

FlightResultPtr build_result(const AirportCatalog& catalog, const LegSearchRequest& request, const FxRates& rates,
                             const DayFare& fare, OrderedCodeSet* missing_currencies, const CodeSet* destination_iatas)
{
    FlightResult result;
    result.fare = fare;
    result.from_airport = find_or_stub(catalog, fare.from);
    result.to_airport = find_or_stub(catalog, fare.to);
    result.adults = std::max(1, request.adults);
    result.from_distance_km = distance_km(request.origin.location, result.from_airport->location);
    if (request.destination) {
        result.to_distance_km = distance_km(request.destination->location, result.to_airport->location);
    }
    result.outside_destination_radius =
        request.destination.has_value() && destination_iatas != nullptr && !destination_iatas->contains(fare.to);
    result.booking_url = booking_links::one_way(fare.from, fare.to, fare.date, result.adults);
    apply_prices(result, rates, request.currency);
    if (missing_currencies != nullptr && result.is_available() && !result.price_per_person_converted &&
        fx_rates::supports(rates, request.currency)) {
        missing_currencies->add(fare.price_per_person.currency);
    }
    return std::make_shared<const FlightResult>(std::move(result));
}

FlightResultPtr reprice(const FlightResult& flight, const FxRates& rates, std::string_view currency)
{
    FlightResult copy = flight;
    apply_prices(copy, rates, currency);
    return std::make_shared<const FlightResult>(std::move(copy));
}

LegSearchResultPtr reprice(const LegSearchResult& result, std::string_view currency)
{
    if (!result.rates) {
        throw std::invalid_argument("A leg search result without exchange rates cannot be re-priced.");
    }
    std::vector<FlightResultPtr> repriced;
    repriced.reserve(result.flights.size());
    for (const FlightResultPtr& flight : result.flights) {
        repriced.push_back(reprice(*flight, *result.rates, currency));
    }
    LegSearchResult copy = result;
    copy.request.currency = text::to_upper_invariant(currency);
    copy.flights = result_views::sort_by_price(repriced);
    return std::make_shared<const LegSearchResult>(std::move(copy));
}

} // namespace wizz::pricing
