#include "wizz/ui/strings.hpp"

#include "wizz/core/datetime_parse.hpp"

#include <algorithm>
#include <format>

namespace wizz::ui::strings {

std::string crash_message(std::string_view exception_message)
{
    return std::format("Something went wrong:\n\n{}\n\nDetails were written to %LOCALAPPDATA%\\WizzairHelper\\errors.log.",
                       exception_message);
}

std::string errors_log_entry(LocalDateTime now, std::string_view exception_text)
{
    return std::format("[{}] {}\r\n\r\n", format_iso_seconds(now), exception_text);
}

bool is_listed_currency(std::string_view code) noexcept
{
    return std::ranges::find(kCurrencies, code) != kCurrencies.end();
}

std::string tab_title(std::string_view base, std::size_t count)
{
    return count > 0 ? std::format("{} ({})", base, count) : std::string{base};
}

std::string fetch_begin(std::string_view label, std::size_t requests, std::string_view duration_text)
{
    return std::format("{}: starting ({} requests, about {})…", label, requests, duration_text);
}

std::string retry_countdown(std::string_view seconds_text)
{
    return std::format("⏳ retrying in {} s", seconds_text);
}

std::string waiting_for_server(std::string_view seconds_text)
{
    return std::format("waiting for server… {} s", seconds_text);
}

std::string map_ready(std::size_t real_airports)
{
    return std::format("Ready — {} Wizz Air airports loaded. Choose your trip and press “Search outbound”.",
                       real_airports);
}

std::string map_load_failed(std::string_view message)
{
    return std::format("Could not load the Wizz Air route map: {}", message);
}

std::string map_unreachable(std::string_view message)
{
    return std::format("Could not reach wizzair.com (offline?): {}", message);
}

std::string place_lookup_failed(std::string_view message)
{
    return std::format("Place lookup failed: {}", message);
}

std::string place_not_found(std::string_view trimmed_text)
{
    return std::format("Could not find “{}”. Check the spelling, or type a nearby city or airport.", trimmed_text);
}

std::string not_an_airport_code(std::string_view upper_text)
{
    return std::format("“{}” is not a Wizz Air airport code, so it was looked up as a place name.", upper_text);
}

std::string which_place(std::string_view trimmed_text)
{
    return std::format("which place is “{}”?", trimmed_text);
}

std::string interpreted(std::string_view typed, std::string_view corrected)
{
    return std::format("Interpreted “{}” as “{}”.", typed, corrected);
}

std::string chooser_title(std::string_view role)
{
    return std::format("Which {}?", role);
}

std::string chooser_intro(std::string_view typed, std::string_view role, std::size_t candidates,
                          const std::optional<std::string>& hint)
{
    std::string intro = candidates == 1
                            ? std::format("Did you mean this place for “{}”?", typed)
                            : std::format("Did you mean one of these places for “{}”? Pick the {}:", typed, role);
    if (hint) {
        return *hint + "\r\n" + intro;
    }
    return intro;
}

std::string request_cap(std::size_t requests, std::int64_t minutes)
{
    return std::format("This search would need {} requests to Wizz Air (about {} min). That is more than the 500 this "
                       "app allows, to stay polite to the site. Use a smaller radius, fewer dates, or a specific "
                       "destination.",
                       requests, minutes);
}

std::string big_search(std::size_t requests, std::string_view duration_text)
{
    return std::format("This search needs {} requests to Wizz Air (about {}, longer if Wizz Air slows us down).\n\n"
                       "Tip: a smaller radius or a specific destination makes it much faster.\n\nContinue?",
                       requests, duration_text);
}

std::string searching_routes(std::int64_t routes, std::size_t airports)
{
    return std::format("Searching {} route(s) from {} airport(s)…", routes, airports);
}

std::string fares_so_far(std::size_t fares)
{
    return std::format("{} fares so far…", fares);
}

std::string leg_cancelled_log(std::string_view label)
{
    return std::format("{}: cancelled.", label);
}

std::string leg_error_log(std::string_view label, std::string_view message)
{
    return std::format("{} ERROR: {}", label, message);
}

std::string notice_log(std::string_view label, std::string_view detail)
{
    return std::format("  {}: {}", label, detail);
}

std::string no_combination(std::int64_t min_nights, std::int64_t max_nights)
{
    return std::format("No combination with a stay of {}–{} nights. Change the nights or the dates.", min_nights,
                       max_nights);
}

std::string open_jaw(std::string_view out_to, std::string_view ret_from)
{
    return std::format("open jaw: land {}, fly back from {}", out_to, ret_from);
}

std::string back_to(std::string_view ret_to, std::string_view out_from)
{
    return std::format("back to {} (left from {})", ret_to, out_from);
}

std::string note_no_rate(std::string_view fare_currency)
{
    return std::format("no exchange rate for {}", fare_currency);
}

std::string note_outside_radius(std::string_view airport_name)
{
    return std::format("lands at {}, outside your destination radius (cheapest airport of its group that day)",
                       airport_name);
}

std::string note_total_for(int adults)
{
    return std::format("total for {} adults", adults);
}

std::string browser_error(std::string_view message, std::string_view url)
{
    return std::format("Could not open the browser:\n{}\n\n{}", message, url);
}

std::string route_map_log(std::size_t real_airports, const std::optional<std::string>& api_url)
{
    if (api_url) {
        return std::format("Route map: {} airports, API {}.", real_airports, *api_url);
    }
    return std::format("Route map: {} airports (from the local cache).", real_airports);
}

std::string map_error_log(std::string_view message)
{
    return std::format("ERROR loading route map: {}", message);
}

std::string return_from_pick_log(std::string_view airport_display, std::string_view from_text, std::string_view to_text)
{
    return std::format("Return will start from {} (picked in the outbound results), {} – {}.", airport_display,
                       from_text, to_text);
}

std::string screenshot_failed(std::string_view name, std::string_view message)
{
    return std::format("screenshot {} failed: {}", name, message);
}

} // namespace wizz::ui::strings
