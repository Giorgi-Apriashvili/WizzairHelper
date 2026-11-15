#include "wizz/ui/return_request.hpp"

#include "wizz/ui/strings.hpp"

#include "wizz/core/text.hpp"

#include <algorithm>
#include <utility>

namespace wizz::ui {

std::optional<std::string_view> validate_search(Date date_from, Date date_to, std::string_view from_text)
{
    if (date_to < date_from) {
        return strings::kEndBeforeStart;
    }
    if (text::is_null_or_white_space(from_text)) {
        return strings::kTypeStart;
    }
    return std::nullopt;
}

bool should_resolve_destination(std::string_view to_text, bool is_return, bool has_override) noexcept
{
    return !text::is_null_or_white_space(to_text) && !(is_return && has_override);
}

LegSearchRequest outbound_request(const Place& home, std::optional<Place> destination, const SearchInputs& inputs,
                                  Date from, Date to)
{
    LegSearchRequest request;
    request.origin = home;
    request.origin_radius_km = inputs.from_radius_km;
    request.destination = std::move(destination);
    request.destination_radius_km = inputs.to_radius_km;
    request.from = from;
    request.to = to;
    request.adults = inputs.adults;
    request.price_type = inputs.price_type;
    request.currency = inputs.currency;
    request.label = std::string{strings::kLabelOutbound};
    return request;
}

std::optional<Place> explicit_outbound_destination(const FlightResultPtr& pick, const AirportCatalog& catalog)
{
    if (!pick || !pick->to_airport) {
        return std::nullopt;
    }
    return catalog.match_place(pick->to_airport->iata);
}

std::expected<LegSearchRequest, std::string> return_request(const Place& home, const std::optional<Place>& destination,
                                                            const std::optional<Place>& override_origin,
                                                            const std::optional<Place>& explicit_destination,
                                                            bool outbound_has_results, const SearchInputs& inputs,
                                                            Date from, Date to)
{
    const std::optional<Place>& origin = override_origin       ? override_origin
                                         : destination         ? destination
                                                               : explicit_destination;
    if (!origin) {
        return std::unexpected(std::string{outbound_has_results ? strings::kReturnNeedsPick
                                                                : strings::kReturnNeedsDestination});
    }
    LegSearchRequest request;
    request.origin = *origin;
    request.origin_radius_km = (override_origin || !destination)
                                   ? std::min(inputs.to_radius_km, kSingleAirportReturnRadiusKm)
                                   : inputs.to_radius_km;
    request.destination = home;
    request.destination_radius_km = inputs.from_radius_km;
    request.from = from;
    request.to = to;
    request.adults = inputs.adults;
    request.price_type = inputs.price_type;
    request.currency = inputs.currency;
    request.label = std::string{strings::kLabelReturn};
    return request;
}

Place return_origin_for(const AirportCatalog& catalog, const FlightResult& outbound)
{
    const Airport& airport = *outbound.to_airport;
    if (std::optional<Place> matched = catalog.match_place(airport.iata)) {
        return *std::move(matched);
    }
    Place place;
    place.name = airport.name;
    place.description = airport.display();
    place.location = airport.location;
    place.source = PlaceSource::wizz_airport;
    place.pinned_airports = {airport.iata};
    return place;
}

std::string plan_return_log(const FlightResult& outbound, Date ret_from, Date ret_to, const Locale& locale)
{
    const std::string display = outbound.to_airport ? outbound.to_airport->display() : outbound.fare.to;
    return strings::return_from_pick_log(display, locale.format_date(ret_from, "dd MMM"),
                                         locale.format_date(ret_to, "dd MMM"));
}

std::optional<std::string> chooser_hint(std::string_view trimmed, const AirportCatalog& catalog)
{
    const std::u16string units = text::utf8_to_utf16(trimmed);
    if (units.size() != 3) {
        return std::nullopt;
    }
    const bool all_letters =
        std::ranges::all_of(units, [](char16_t unit) { return text::is_letter(static_cast<char32_t>(unit)); });
    if (!all_letters || catalog.find(trimmed)) {
        return std::nullopt;
    }
    return strings::not_an_airport_code(text::to_upper_invariant(trimmed));
}

ResolveDecision decide_resolution(std::string_view typed, const PlaceLookup& lookup, bool autotest,
                                  const AirportCatalog& catalog)
{
    ResolveDecision decision;
    decision.trimmed = std::string{text::trim(typed)};
    if (lookup.candidates.empty()) {
        decision.action = ResolveAction::fail;
        decision.error =
            lookup.lookup_failed ? std::string{strings::kLookupOffline} : strings::place_not_found(decision.trimmed);
        return decision;
    }
    if (autotest || !lookup.needs_confirmation) {
        decision.action = ResolveAction::accept;
        if (lookup.corrected_text) {
            decision.log = strings::interpreted(decision.trimmed, *lookup.corrected_text);
        }
        return decision;
    }
    decision.action = ResolveAction::ask;
    decision.hint = chooser_hint(decision.trimmed, catalog);
    return decision;
}

std::optional<std::string> interpreted_after_choice(std::string_view trimmed, const PlaceLookup& lookup,
                                                    std::optional<std::size_t> chosen_index)
{
    if (!chosen_index || *chosen_index != 0 || !lookup.corrected_text) {
        return std::nullopt;
    }
    return strings::interpreted(trimmed, *lookup.corrected_text);
}

SearchProgress finding_places_progress(const ClientNotice& notice, UtcInstant now)
{
    SearchProgress progress;
    progress.stage = std::string{strings::kStageFindingPlaces};
    progress.detail = notice.message;
    progress.step_started_utc = now;
    return progress;
}

SearchProgress waiting_for_choice_progress(std::string_view trimmed)
{
    SearchProgress progress;
    progress.stage = std::string{strings::kStageWaitingForChoice};
    progress.detail = strings::which_place(trimmed);
    progress.waiting_for_user = true;
    return progress;
}

FailOutcome prepare_failure(std::string_view message, bool autotest)
{
    if (message.empty()) {
        return FailOutcome{.busy_text = std::string{strings::kSearchNotStarted},
                           .success = true,
                           .log = false,
                           .show_error_box = false};
    }
    return FailOutcome{.busy_text = std::string{message}, .success = false, .log = true, .show_error_box = !autotest};
}

} // namespace wizz::ui
