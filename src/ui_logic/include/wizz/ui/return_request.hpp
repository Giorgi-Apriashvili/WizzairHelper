#pragma once
// PrepareAsync / ResolveAsync / PlanReturnFromAsync decisions without a window
// (01 §11.4-§11.6; 05 U-02): the two early validation texts, which places to
// resolve, the outbound and return LegSearchRequest (incl. the 50 km cap for a
// return that starts at a single airport, the right-click override and the
// explicit outbound pick), the chooser decision with its hint, the
// "Interpreted …" log lines and the Fail(message) outcome.
//
// Choice (05 U-02 vs 01 §11.4, C-08): currency, adults and fare type come from
// the current inputs, as 01 §11.4 builds them; an unchanged panel therefore
// repeats the outbound's values. Plain functions, safe from any thread.

#include "wizz/core/catalog.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/core/time.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace wizz::ui {

/// The input-panel values a search reads (01 §11.4). Radii keep typed fractions;
/// adults are already truncated toward zero (01 §4.4).
struct SearchInputs {
    double from_radius_km = 250;
    double to_radius_km = 100;
    int adults = 1;
    PriceType price_type = PriceType::regular;
    std::string currency{"EUR"};
};

/// A return that starts at a single airport (override, or "To" empty with a pick)
/// searches at most this far around it (01 §11.4).
inline constexpr double kSingleAirportReturnRadiusKm = 50;

/// `_fare` combo index → price type: 1 = Wizz Discount Club, else Regular.
[[nodiscard]] constexpr PriceType price_type_for_fare_index(int index) noexcept
{
    return index == 1 ? PriceType::wizz_discount_club : PriceType::regular;
}

/// The two early checks, in order, shown as MessageBoxes even in autotest [WART]:
/// "The end date is before the start date." when `date_to < date_from`, then
/// "Type where you start from (a city or airport)." when `from_text` is blank
/// (white space only). nullopt when both pass.
[[nodiscard]] std::optional<std::string_view> validate_search(Date date_from, Date date_to, std::string_view from_text);

/// Whether PrepareAsync resolves "To": it is not blank and this is not a right-click
/// return (a typed "To" wins over an outbound pick; the override skips "To" entirely).
[[nodiscard]] bool should_resolve_destination(std::string_view to_text, bool is_return, bool has_override) noexcept;

/// The outbound request: Origin = home with the From radius, Destination = the
/// resolved "To" (nullopt = anywhere) with the To radius, the outbound dates and the
/// current adults / fare type / currency, Label "Outbound".
[[nodiscard]] LegSearchRequest outbound_request(const Place& home, std::optional<Place> destination,
                                                const SearchInputs& inputs, Date from, Date to);

/// `ExplicitOutboundDestination`: MatchPlace(pick.ToAirport.Iata) when the user picked
/// an outbound fare, else nullopt ("LTN" → "London Luton (LTN), United Kingdom" pinned [LTN]).
[[nodiscard]] std::optional<Place> explicit_outbound_destination(const FlightResultPtr& pick,
                                                                 const AirportCatalog& catalog);

/// The return request (U-02): origin = override ?? destination ?? explicit pick; with
/// none of them the error text depends on whether the outbound grid holds fares. The
/// origin radius is min(toRadius, 50) for an override or an empty "To", else toRadius;
/// Destination = home with the From radius; the return dates; Label "Return".
[[nodiscard]] std::expected<LegSearchRequest, std::string>
return_request(const Place& home, const std::optional<Place>& destination, const std::optional<Place>& override_origin,
               const std::optional<Place>& explicit_destination, bool outbound_has_results, const SearchInputs& inputs,
               Date from, Date to);

/// The right-click override (01 §11.6): MatchPlace(a.Iata) for the outbound's arrival
/// airport a, else Place{a.Name, a.Display, a.Location, WizzAirport, pinned [a.Iata]}.
[[nodiscard]] Place return_origin_for(const AirportCatalog& catalog, const FlightResult& outbound);

/// The right-click Log line "Return will start from {a.Display} (picked in the outbound
/// results), {retFrom:dd MMM} – {retTo:dd MMM}.".
[[nodiscard]] std::string plan_return_log(const FlightResult& outbound, Date ret_from, Date ret_to,
                                          const Locale& locale);

// ---------------------------------------------------------------------------
// ResolveAsync (01 §11.5)
// ---------------------------------------------------------------------------

/// What PrepareAsync does with a resolver answer.
enum class ResolveAction {
    fail,   ///< no candidates: Fail(error) with E5 (lookup failed) or E6 (not found)
    accept, ///< use candidates[0] (no confirmation needed, or autotest)
    ask,    ///< show the place chooser
};

struct ResolveDecision {
    ResolveAction action = ResolveAction::fail;
    std::string trimmed;                ///< text.Trim()
    std::string error;                  ///< fail: E5 / E6
    std::optional<std::string> log;     ///< accept: "Interpreted “{t}” as “{corrected}”." when corrected
    std::optional<std::string> hint;    ///< ask: E7 for a 3-letter non-code, else nullopt
};

/// The chooser hint (E7): when the trimmed text is exactly 3 UTF-16 units, all letters
/// (char.IsLetter per unit) and not a catalog code, "“{UPPER}” is not a Wizz Air
/// airport code, so it was looked up as a place name."
[[nodiscard]] std::optional<std::string> chooser_hint(std::string_view trimmed, const AirportCatalog& catalog);

/// The ResolveAsync branch for `typed` and the resolver's `lookup`.
[[nodiscard]] ResolveDecision decide_resolution(std::string_view typed, const PlaceLookup& lookup, bool autotest,
                                                const AirportCatalog& catalog);

/// After the chooser: the "Interpreted …" line when the user took the corrected first
/// candidate (`chosen_index` 0) and the resolver corrected the text; nullopt otherwise
/// (also for a cancelled chooser, `chosen_index` nullopt).
[[nodiscard]] std::optional<std::string> interpreted_after_choice(std::string_view trimmed, const PlaceLookup& lookup,
                                                                  std::optional<std::size_t> chosen_index);

/// The report for a resolver notice: Stage "Finding places", Detail = message,
/// StepStartedUtc = now (so "waiting for server…" appears after 3 s).
[[nodiscard]] SearchProgress finding_places_progress(const ClientNotice& notice, UtcInstant now);
/// The direct report before the chooser opens: Stage "Waiting for your choice",
/// Detail "which place is “{t}”?", WaitingForUser (marquee, no wait note; 07 §4).
[[nodiscard]] SearchProgress waiting_for_choice_progress(std::string_view trimmed);

/// `Fail(message)` of PrepareAsync: an empty message (chooser cancelled) ends green with
/// "Search not started." and nothing else (UI-10); any other message ends red, is logged
/// and shown in a MessageBox unless autotest.
struct FailOutcome {
    std::string busy_text;
    bool success = false;
    bool log = false;
    bool show_error_box = false;

    friend bool operator==(const FailOutcome&, const FailOutcome&) = default;
};
[[nodiscard]] FailOutcome prepare_failure(std::string_view message, bool autotest);

} // namespace wizz::ui
