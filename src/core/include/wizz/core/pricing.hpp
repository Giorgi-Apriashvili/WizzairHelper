#pragma once
// Pricing (03 §6.1–§6.3, 02 §2.11): turns a timetable row into a FlightResult
// (airports, distances, booking link, converted price) and re-prices finished
// results into another display currency without any network access.
//
// Money rule (03 §6.2, 07 §2): convert through EUR in exact decimal arithmetic,
// round the per-person amount to the display currency's decimals
// (currency_format::decimals) with halves away from zero, then multiply by the
// adults. The total is never rounded again.

#include "wizz/core/domain.hpp"
#include "wizz/core/text.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace wizz {
class AirportCatalog; // wizz/core/catalog.hpp
} // namespace wizz

namespace wizz::pricing {

/// A case-insensitive set of codes (`set_ci` of 03 §5.3): IATA codes or currencies,
/// compared OrdinalIgnoreCase. Supports heterogeneous `contains(std::string_view)`.
using CodeSet = std::unordered_set<std::string, text::OrdinalIgnoreCaseHash, text::OrdinalIgnoreCaseEqual>;

/// `set_ci(plan.DestinationAirports iatas)` (03 §5.3 `destIatas`): the codes of `airports`.
/// Entries must have a non-null airport.
[[nodiscard]] CodeSet iata_set(std::span<const AreaAirport> airports);

/// `insertion_ordered_set_ci<string>` of 03 §5.3 (`missingCurrencies`): codes in
/// first-seen order; a code equal to an earlier one (OrdinalIgnoreCase) is not
/// added again and the first spelling is kept.
class OrderedCodeSet {
public:
    /// Adds `code` unless an equal code is present. Returns true if it was added.
    bool add(std::string_view code);

    [[nodiscard]] bool contains(std::string_view code) const { return seen_.contains(code); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    /// The codes in insertion order.
    [[nodiscard]] const std::vector<std::string>& items() const noexcept { return items_; }
    [[nodiscard]] std::vector<std::string>::const_iterator begin() const noexcept { return items_.begin(); }
    [[nodiscard]] std::vector<std::string>::const_iterator end() const noexcept { return items_.end(); }

private:
    std::vector<std::string> items_;
    CodeSet seen_;
};

/// `Unknown(iata)` of 03 §6.1 / 02 §2.11: the stub for a code the catalog does
/// not know — Iata = Name = `iata` as given, empty country and currency fields,
/// Location (0, 0), no Mac, not a pseudo station, no connections, no city group.
[[nodiscard]] AirportPtr unknown_airport(std::string_view iata);

/// BuildResult (03 §6.1):
/// - FromAirport / ToAirport = catalog.find(fare.from / fare.to), else `unknown_airport`;
/// - FromDistanceKm from `request.origin.location`; ToDistanceKm from the
///   destination's location, nullopt for "anywhere";
/// - OutsideDestinationRadius = a destination is set, `destination_iatas` is
///   given and does not contain fare.to (OrdinalIgnoreCase);
/// - Adults = max(1, request.adults); TargetCurrency = upper(request.currency);
/// - BookingUrl = booking_links::one_way(fare.from, fare.to, fare.date, adults);
/// - prices as `reprice` (03 §6.2).
/// When the fare is available, it got no converted price and the target currency
/// is supported, the fare's own currency is added to `missing_currencies` (when
/// given) — also an empty code (03 §5.8 item 5).
[[nodiscard]] FlightResultPtr build_result(const AirportCatalog& catalog, const LegSearchRequest& request,
                                           const FxRates& rates, const DayFare& fare,
                                           OrderedCodeSet* missing_currencies = nullptr,
                                           const CodeSet* destination_iatas = nullptr);

/// Reprice(FlightResult) (03 §6.2): a copy of `flight` with TargetCurrency =
/// upper(currency); for an available fare PricePerPersonConverted =
/// round(fx_rates::convert(amount, fare currency, currency), decimals(currency),
/// away from zero) or nullopt when not convertible; TotalConverted = per person ×
/// `flight.adults` (exact). CheckOnSite fares get no price. A fare already in the
/// target currency is converted exactly but still rounded (19290 HUF → 19290).
[[nodiscard]] FlightResultPtr reprice(const FlightResult& flight, const FxRates& rates, std::string_view currency);

/// Reprice(LegSearchResult) (03 §6.3, CT-SRCH-07): no network. Request.currency
/// becomes upper(currency); Flights = SortByPrice of every flight re-priced with
/// `result.rates` (the previous order breaks remaining ties). Plan, Warnings (not
/// recomputed), Rates, counters, Cancelled, AbortReason and Elapsed are unchanged.
/// The flights must be non-null. Throws std::invalid_argument when `result.rates` is null.
[[nodiscard]] LegSearchResultPtr reprice(const LegSearchResult& result, std::string_view currency);

} // namespace wizz::pricing
