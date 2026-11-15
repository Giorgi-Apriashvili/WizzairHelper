// SearchPlanner building blocks (03 §4.3.2, §4.3.5, §4.3.7, §4.3.10, §4.4, §5.9).

#include "wizz/core/planner.hpp"

#include "planner_detail.hpp"

#include "wizz/core/geo.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wizz::planner {

namespace {

[[nodiscard]] std::string_view role_text(AreaRole role) noexcept
{
    return role == AreaRole::start ? "start" : "destination";
}

/// The radius AirportsFor searches with: .NET `Math.Max(0, r)`, which keeps NaN.
[[nodiscard]] double search_radius(double radius_km) noexcept
{
    return std::isnan(radius_km) ? radius_km : std::max(0.0, radius_km);
}

} // namespace

// ---------------------------------------------------------------------------
// 03 §4.3.10 Normalize / Min
// ---------------------------------------------------------------------------

NextDeparture normalize_next_departure(std::optional<LocalDateTime> next) noexcept
{
    if (!next) {
        return NextDeparture::unknown();
    }
    if (date_of(*next).year() < kSuspendedBeforeYear) {
        return NextDeparture::suspended();
    }
    return NextDeparture::at(*next);
}

NextDeparture merge_next_departures(NextDeparture a, NextDeparture b) noexcept
{
    if (a.is_unknown() || b.is_unknown()) {
        return NextDeparture::unknown();
    }
    if (a.is_suspended()) {
        return b;  // suspended = +infinity
    }
    if (b.is_suspended()) {
        return a;
    }
    return *b.time() < *a.time() ? b : a;
}

// ---------------------------------------------------------------------------
// 03 §4.3.2 AirportsFor / GroupMembersOrSelf
// ---------------------------------------------------------------------------

std::vector<AirportPtr> group_members_or_self(const AirportCatalog& catalog, std::string_view code)
{
    AirportPtr airport = catalog.find(code);
    if (!airport) {
        return {};
    }
    if (airport->is_fake_station) {
        return catalog.group_members(airport->iata);
    }
    return {std::move(airport)};
}

std::vector<AreaAirport> airports_for(const AirportCatalog& catalog, const Place& place, double radius_km)
{
    std::vector<AreaAirport> result = catalog.airports_near(place.location, search_radius(radius_km));
    for (const std::string& code : place.pinned_airports) {
        for (AirportPtr& airport : group_members_or_self(catalog, code)) {
            const bool present = std::ranges::any_of(
                result, [&](const AreaAirport& entry) { return entry.airport->iata == airport->iata; });  // ordinal
            if (!present) {
                const double distance = distance_km(place.location, airport->location);
                result.push_back(AreaAirport{std::move(airport), distance});
            }
        }
    }
    std::ranges::stable_sort(result, std::ranges::less{}, &AreaAirport::distance_km);
    return result;
}

// ---------------------------------------------------------------------------
// 03 §4.3.5 NoAirportNote
// ---------------------------------------------------------------------------

std::string no_airport_note(const AirportCatalog& catalog, const Place& place, double radius_km, AreaRole role,
                            const Locale& locale)
{
    const std::vector<AreaAirport> everything =
        catalog.airports_near(place.location, std::numeric_limits<double>::max());  // 02: double.MaxValue
    if (everything.empty()) {
        return "No Wizz Air airport near the " + std::string{role_text(role)} + " (" + place.name + ").";
    }
    const AreaAirport& nearest = everything.front();
    std::string note = "No Wizz Air airport within " + detail::format_km(radius_km, locale) + " km of " + place.name +
                       ". The nearest is " + nearest.airport->display() + ", " +
                       detail::format_km(nearest.distance_km, locale) + " km away.";
    if (nearest.distance_km <= kRadiusHintMaxKm) {
        note += " Set the " + std::string{role_text(role)} + " radius to at least " +
                detail::format_km(std::ceil(nearest.distance_km / 10) * 10, locale) + " km.";
    }
    return note;
}

// ---------------------------------------------------------------------------
// 03 §4.3.7 ListsBack
// ---------------------------------------------------------------------------

bool lists_back(const AirportCatalog& catalog, const Airport& target, const Airport& origin)
{
    // The origin's city code counts only when its mac names a pseudo station.
    std::optional<std::string_view> city_code;
    if (origin.mac) {
        if (const AirportPtr city = catalog.find(*origin.mac); city && city->is_fake_station) {
            city_code = *origin.mac;
        }
    }
    const auto lists = [&](const Airport& station) {
        return std::ranges::any_of(station.connections, [&](const RouteConnection& connection) {
            return text::ordinal_ignore_case_equals(connection.iata, origin.iata) ||
                   (city_code && text::ordinal_ignore_case_equals(connection.iata, *city_code));
        });
    };

    if (lists(target)) {
        return true;
    }
    if (target.is_fake_station) {
        return std::ranges::any_of(catalog.group_members(target.iata),
                                   [&](const AirportPtr& member) { return lists(*member); });
    }
    if (target.mac) {
        if (const AirportPtr fake = catalog.find(*target.mac); fake && fake->is_fake_station && lists(*fake)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 03 §4.4 ReachableNames, GroupName; 03 §5.9 GroupLabel; Display
// ---------------------------------------------------------------------------

std::vector<std::string> reachable_names(const AirportCatalog& catalog, std::span<const AirportPtr> origins,
                                         std::span<const std::string> exclude, const Locale& locale)
{
    using NameSet = std::set<std::string, CollationLess>;  // SortedSet(StringComparer.CurrentCulture)
    const detail::CodeSet excluded(exclude.begin(), exclude.end());
    detail::OrderedCodeMap<std::optional<NameSet>> by_group;

    for (const AirportPtr& origin : origins) {
        for (const RouteConnection& connection : origin->connections) {
            const AirportPtr target = catalog.find(connection.iata);
            if (!target || excluded.contains(target->iata) || !lists_back(catalog, *target, *origin)) {
                continue;
            }
            std::optional<NameSet>& names = by_group[target->group_key()];
            if (!names) {
                names.emplace(CollationLess{locale});
            }
            // Culture-equal strings collapse; the first spelling stays.
            names->insert(target->is_fake_station ? AirportCatalog::city_name_of_fake(target->name) : target->name);
        }
    }

    std::vector<std::string> result;
    result.reserve(by_group.size());
    for (const auto& [group, names] : by_group.entries()) {
        const std::string* shortest = nullptr;
        std::size_t shortest_length = 0;
        for (const std::string& name : *names) {
            const std::size_t length = text::utf16_length(name);  // .NET string.Length
            if (!shortest || length < shortest_length) {
                shortest = &name;
                shortest_length = length;
            }
        }
        const bool all_start_with_shortest = std::ranges::all_of(*names, [&](const std::string& name) {
            return locale.starts_with(name, *shortest, CollationStrength::secondary);
        });
        if (all_start_with_shortest) {
            result.push_back(*shortest);
        } else {
            const std::vector<std::string> all(names->begin(), names->end());
            result.push_back(detail::join(all, " / "));
        }
    }
    std::ranges::stable_sort(result, CollationLess{locale});
    return result;
}

std::string group_name(const AirportCatalog& catalog, std::string_view group_key)
{
    const AirportPtr airport = catalog.find(group_key);
    if (airport && airport->is_fake_station) {
        return AirportCatalog::city_name_of_fake(airport->name);
    }
    const std::vector<AirportPtr> members = catalog.group_members(group_key);
    if (members.empty()) {
        return airport ? airport->name : std::string{group_key};
    }
    if (members.size() == 1) {
        return members.front()->name;
    }
    std::vector<std::string> names;
    for (const AirportPtr& member : members) {
        if (std::ranges::find(names, member->name) == names.end()) {  // distinct, ordinal, first kept
            names.push_back(member->name);
        }
    }
    return detail::join(names, " / ");
}

std::string group_label(const AirportCatalog& catalog, std::string_view group_key)
{
    std::string name = group_name(catalog, group_key);
    if (const AirportPtr airport = catalog.find(group_key); airport && airport->is_fake_station) {
        return name + " (" + std::string{group_key} + ")";
    }
    const std::vector<AirportPtr> members = catalog.group_members(group_key);
    if (members.size() == 1) {
        return name + " (" + members.front()->iata + ")";
    }
    return name;
}

std::string display(const AirportCatalog& catalog, std::string_view iata)
{
    const AirportPtr airport = catalog.find(iata);
    return airport ? airport->display() : std::string{iata};
}

} // namespace wizz::planner
