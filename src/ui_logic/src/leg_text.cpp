#include "wizz/ui/leg_text.hpp"

#include "wizz/ui/strings.hpp"

#include "wizz/core/currency_format.hpp"
#include "wizz/core/format.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace wizz::ui {

namespace {

constexpr double kSecondsPerRequest = 1.3;
constexpr double kTicksPerSecond = 10'000'000.0;
constexpr double kTicksPerMinute = 600'000'000.0;

[[nodiscard]] std::string km_label(double km, const Locale& locale)
{
    return format_label(km, 0, locale.number_symbols());
}

[[nodiscard]] std::string_view iata_of(const AirportPtr& airport, std::string_view fallback) noexcept
{
    return airport ? std::string_view{airport->iata} : fallback;
}

[[nodiscard]] std::string_view price_type_name(PriceType type) noexcept
{
    return type == PriceType::wizz_discount_club ? "WizzDiscountClub" : "Regular";
}

/// "{IATA} {km:0} km" joined ", " (ShowResolved) or "{IATA} ({km:0} km)" (LogPlan).
[[nodiscard]] std::string join_airports(std::span<const AreaAirport> airports, bool parenthesised, const Locale& locale)
{
    std::string text;
    for (const AreaAirport& area : airports) {
        if (!text.empty()) {
            text += ", ";
        }
        const std::string_view iata = area.airport ? std::string_view{area.airport->iata} : std::string_view{};
        text += parenthesised ? std::format("{} ({} km)", iata, km_label(area.distance_km, locale))
                              : std::format("{} {} km", iata, km_label(area.distance_km, locale));
    }
    return text;
}

} // namespace

RequestBudget request_budget(std::size_t requests, bool autotest) noexcept
{
    if (requests > kMaxRequests) {
        return RequestBudget::refuse;
    }
    if (requests > kConfirmAboveRequests && !autotest) {
        return RequestBudget::confirm;
    }
    return RequestBudget::proceed;
}

Duration request_estimate(std::size_t requests) noexcept
{
    // TimeSpan.FromSeconds(double): ticks = seconds * TicksPerSecond, truncated toward zero.
    const double seconds = static_cast<double>(requests) * kSecondsPerRequest;
    return Duration{static_cast<Duration::rep>(seconds * kTicksPerSecond)};
}

std::int64_t estimate_minutes_ceiling(Duration estimate) noexcept
{
    return static_cast<std::int64_t>(std::ceil(static_cast<double>(estimate.count()) / kTicksPerMinute));
}

std::string fetch_begin_text(std::string_view label, std::size_t requests)
{
    return strings::fetch_begin(label, requests, format_duration(request_estimate(requests)));
}

std::string request_cap_text(std::size_t requests)
{
    return strings::request_cap(requests, estimate_minutes_ceiling(request_estimate(requests)));
}

std::string big_search_text(std::size_t requests)
{
    return strings::big_search(requests, format_duration(request_estimate(requests)));
}

std::string nothing_to_search_text(const SearchPlan& plan)
{
    return plan.notes.empty() ? std::string{strings::kNothingToSearch} : plan.notes.front();
}

std::string searching_text(const SearchPlan& plan)
{
    return strings::searching_routes(plan.routes_considered, plan.origin_airports.size());
}

std::string money_text(const Money& money, const Locale& locale)
{
    return currency_format::format(money.amount, money.currency, locale.number_symbols());
}

std::string price_text(const FlightResult& flight, const Locale& locale)
{
    if (flight.total_converted) {
        return currency_format::format(*flight.total_converted, flight.target_currency, locale.number_symbols());
    }
    return money_text(flight.fare.price_per_person, locale);
}

std::string summary(const LegSearchResult& result, const Locale& locale)
{
    if (result.flights.empty() || !result.flights.front()) {
        if (result.abort_reason) {
            return std::string{strings::kSummaryNoFaresPrefix} + *result.abort_reason;
        }
        if (result.cancelled) {
            return std::string{strings::kSummaryCancelledBeforeFares};
        }
        if (result.routes_not_served > 0 && result.routes_not_served >= result.plan.routes_considered) {
            return std::string{strings::kSummaryRoutesNotServed};
        }
        const auto note = std::ranges::find_if(result.plan.notes, [](const std::string& text) {
            return !text.starts_with(strings::kDatesBeforePrefix);
        });
        return note != result.plan.notes.end() ? *note : std::string{strings::kSummaryNoFlights};
    }
    const FlightResult& cheapest = *result.flights.front();
    std::string text = std::format("{} fares · cheapest {}: {} → {} on {}", result.flights.size(),
                                   price_text(cheapest, locale), iata_of(cheapest.from_airport, cheapest.fare.from),
                                   iata_of(cheapest.to_airport, cheapest.fare.to),
                                   locale.format_date(cheapest.date(), "ddd dd MMM"));
    if (result.cancelled) {
        text += strings::kSummaryCancelledEarly;
    }
    if (result.abort_reason) {
        text += strings::kSummaryStoppedEarly;
    }
    return text;
}

std::vector<std::string> extra_warnings(const LegSearchResult& result)
{
    std::vector<std::string> extra;
    for (const std::string& warning : result.warnings) {
        if (std::ranges::find(result.plan.notes, warning) == result.plan.notes.end()) {
            extra.push_back(warning);
        }
    }
    return extra;
}

std::string status_line(const LegSearchResult& result)
{
    const std::string_view verb = result.cancelled      ? strings::kVerbCancelled
                                  : result.abort_reason ? strings::kVerbStoppedEarly
                                                        : strings::kVerbDone;
    std::string text = std::format("{} {}: {} fares from {} of {} requests in {}.", result.request.label, verb,
                                   result.flights.size(), result.requests_completed, result.plan.requests.size(),
                                   format_duration(result.elapsed));
    const std::size_t extra = extra_warnings(result).size() + result.plan.notes.size();
    if (result.abort_reason) {
        text += " " + *result.abort_reason;
    } else if (extra > 0) {
        text += std::format(" {} note(s) in the Log tab.", extra);
    }
    return text;
}

bool status_success(const LegSearchResult& result) noexcept
{
    const bool ok = !result.abort_reason && !result.cancelled;
    return ok || !result.flights.empty();
}

std::string show_resolved(const LegSearchRequest& request, const SearchPlan& plan, const Locale& locale)
{
    const auto airports = [&](std::span<const AreaAirport> list) -> std::string {
        if (plan.same_area) {
            return std::string{strings::kResolvedSameArea};
        }
        if (list.empty()) {
            return std::string{strings::kResolvedNoneInRange};
        }
        return join_airports(list, false, locale);
    };
    std::string text = std::format("{}: from {} → airports within {} km: {}.   ", request.label,
                                   request.origin.description, km_label(request.origin_radius_km, locale),
                                   airports(plan.origin_airports));
    if (!request.destination) {
        text += strings::kResolvedAnywhere;
    } else {
        text += std::format("To {} → airports within {} km: {}.", request.destination->description,
                            km_label(request.destination_radius_km, locale), airports(plan.destination_airports));
    }
    return text;
}

std::string log_entry(LocalDateTime now, std::string_view line, const Locale& locale)
{
    return std::format("[{}] {}\r\n", locale.format_time(now, "HH:mm:ss"), line);
}

std::vector<std::string> log_plan_lines(const LegSearchRequest& request, const SearchPlan& plan, const Locale& locale)
{
    std::vector<std::string> lines;
    std::string first = std::format("{}: {} (+{} km) → {}", request.label, request.origin.description,
                                    km_label(request.origin_radius_km, locale),
                                    request.destination ? request.destination->description : std::string{"anywhere"});
    if (request.destination) {
        first += std::format(" (+{} km)", km_label(request.destination_radius_km, locale));
    }
    first += std::format(", {} … {}, {} adult(s), {}, {}", plan.effective_from.iso(), plan.effective_to.iso(),
                         request.adults, price_type_name(request.price_type), request.currency);
    lines.push_back(std::move(first));

    if (plan.same_area) {
        lines.emplace_back(strings::kSameAreaLogLine);
    } else {
        lines.push_back("  origin airports: " + join_airports(plan.origin_airports, true, locale));
        if (request.destination) {
            lines.push_back("  destination airports: " + join_airports(plan.destination_airports, true, locale));
        }
        lines.push_back(std::format("  {} route(s), {} request(s)", plan.routes_considered, plan.requests.size()));
    }
    for (const std::string& note : plan.notes) {
        lines.push_back("  note: " + note);
    }
    return lines;
}

std::optional<std::string> reused_fares_line(const LegSearchResult& result)
{
    if (result.requests_from_cache + result.requests_partly_cached <= 0) {
        return std::nullopt;
    }
    return strings::reused_fares_log(strings::reused_age(result.oldest_reused_age.value_or(Duration::zero())),
                                     result.requests_from_cache, result.plan.requests.size(),
                                     result.requests_partly_cached);
}

std::vector<std::string> log_result_lines(const LegSearchResult& result, const Locale& locale)
{
    std::vector<std::string> lines;
    std::string first = std::format("{}: {} fares, {}/{} requests, {} route(s) not served, {}, FX ", result.request.label,
                                    result.flights.size(), result.requests_completed, result.plan.requests.size(),
                                    result.routes_not_served, format_duration(result.elapsed));
    if (result.rates) {
        first += std::format("{} {}", result.rates->source, result.rates->date.iso());
        if (result.rates->is_approximate) {
            first += " (approximate)";
        }
    }
    lines.push_back(std::move(first));
    if (std::optional<std::string> reused = reused_fares_line(result)) {
        lines.push_back(*std::move(reused));
    }
    for (const std::string& warning : extra_warnings(result)) {
        lines.push_back("  ! " + warning);
    }
    std::size_t written = 0;
    for (const FlightResultPtr& flight : result.flights) {
        if (written == kLoggedFares) {
            break;
        }
        if (!flight) {
            continue;
        }
        lines.push_back(std::format("    {} {}  {}→{}  {}  = {}", locale.format_date(flight->date(), "ddd dd MMM"),
                                    locale.format_time(flight->fare.cheapest_departure, "HH:mm"),
                                    iata_of(flight->from_airport, flight->fare.from),
                                    iata_of(flight->to_airport, flight->fare.to),
                                    money_text(flight->fare.price_per_person, locale), price_text(*flight, locale)));
        ++written;
    }
    return lines;
}

std::optional<std::string> NoticeLog::line_for(const SearchProgress& progress)
{
    const bool interesting = progress.waiting_until_utc.has_value() ||
                             progress.detail.find("API version") != std::string::npos ||
                             progress.detail.find("cached") != std::string::npos;
    if (!interesting || !logged_.insert(progress.detail).second) {
        return std::nullopt;
    }
    return strings::notice_log(label_, progress.detail);
}

std::string round_trip_summary(std::span<const RoundTrip> trips, int min_nights, int max_nights,
                               std::string_view currency, const Locale& locale)
{
    if (trips.empty()) {
        return strings::no_combination(min_nights, max_nights);
    }
    const RoundTrip& best = trips.front();
    const std::string total =
        best.total ? currency_format::format(*best.total, currency, locale.number_symbols()) : std::string{};
    const FlightResult& out = *best.outbound;
    const FlightResult& ret = *best.inbound;
    return std::format("{} combinations · cheapest {}: {}→{} {}, back {}→{} {} ({} nights)", trips.size(), total,
                       iata_of(out.from_airport, out.fare.from), iata_of(out.to_airport, out.fare.to),
                       locale.format_date(out.date(), "ddd dd MMM"), iata_of(ret.from_airport, ret.fare.from),
                       iata_of(ret.to_airport, ret.fare.to), locale.format_date(ret.date(), "ddd dd MMM"), best.nights);
}

} // namespace wizz::ui
