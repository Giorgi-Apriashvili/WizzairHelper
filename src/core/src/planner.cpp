// SearchPlanner.Plan (03 §4.2), step for step. Comments name the pseudocode
// steps; note texts are 03 §4.4 / 01 §18.5 A1–A16.

#include "wizz/core/planner.hpp"

#include "planner_detail.hpp"

#include "wizz/core/date_chunker.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wizz::planner {

namespace {

using detail::CodeSet;
using detail::format_km;
using detail::join;
using detail::OrderedCodeMap;

/// How many entries the I3, I4, I6 and B7 notes list (03 §4.4).
constexpr std::size_t kListedEntries = 3;
/// How many origin codes `originList` names (03 §4.4).
constexpr std::size_t kOriginListLength = 4;
/// How many names the B8 note lists before " and N more" (03 §4.4).
constexpr std::size_t kReachableNamesShown = 12;

/// One arrival group of one origin (03 §4.2 step 8: `Route{QueryTo, Next, QueryIsDest}`).
struct Route {
    std::string query_to;
    NextDeparture next;
    bool query_is_dest = false;
};

[[nodiscard]] CodeSet iata_set(const std::vector<AreaAirport>& airports)
{
    CodeSet codes;
    for (const AreaAirport& entry : airports) {
        codes.insert(entry.airport->iata);
    }
    return codes;
}

[[nodiscard]] bool contains_ordinal(const std::vector<AreaAirport>& airports, std::string_view iata)
{
    return std::ranges::any_of(airports, [iata](const AreaAirport& entry) { return entry.airport->iata == iata; });
}

/// `"; …"` after the first three entries when there are more (03 §4.4: the text then ends `…; ….`).
[[nodiscard]] std::string first_entries(const std::vector<std::string>& entries)
{
    return join(entries, "; ", kListedEntries) + (entries.size() > kListedEntries ? "; …" : "");
}

/// `stable_sort_by_value(outsideRadius)` (03 §4.2 step 10): ties keep the first insertion order.
[[nodiscard]] std::vector<std::pair<std::string, double>> by_distance(const OrderedCodeMap<double>& outside_radius)
{
    std::vector<std::pair<std::string, double>> sorted(outside_radius.entries().begin(), outside_radius.entries().end());
    std::ranges::stable_sort(sorted, std::ranges::less{}, &std::pair<std::string, double>::second);
    return sorted;
}

} // namespace

SearchPlan plan(const AirportCatalog& catalog, const LegSearchRequest& request, Date today, const Locale& locale)
{
    std::vector<std::string> blocking;  // reasons nothing (or less) can be searched; shown first
    std::vector<std::string> info;

    // 1. Dates (03 §4.3.1)
    const Date from = request.from < today ? today : request.from;
    const Date to = request.to;
    if (request.from < today) {
        info.push_back("Dates before today were skipped (searching from " + locale.format_date(from, "yyyy-MM-dd") +
                       ").");
    }

    // 2. Airports in each area (03 §4.3.2)
    std::vector<AreaAirport> origins = airports_for(catalog, request.origin, request.origin_radius_km);
    std::vector<AreaAirport> destinations =
        request.destination ? airports_for(catalog, *request.destination, request.destination_radius_km)
                            : std::vector<AreaAirport>{};

    // 3. Same area / overlap (03 §4.3.3, §4.3.4)
    bool same_area = false;
    if (request.destination) {
        const double apart = distance_km(request.origin.location, request.destination->location);
        if (apart < kSameAreaKm) {
            blocking.push_back("Start and destination are the same area (" + format_km(apart, locale) +
                               " km apart). Choose a destination farther away, or leave “To” empty to "
                               "search anywhere.");
            same_area = true;
            origins.clear();
            destinations.clear();
        } else {
            std::vector<std::string> overlap;  // ordinal, origin order
            for (const AreaAirport& origin : origins) {
                if (contains_ordinal(destinations, origin.airport->iata)) {
                    overlap.push_back(origin.airport->iata);
                }
            }
            if (!overlap.empty()) {
                std::erase_if(origins, [&](const AreaAirport& origin) {
                    return std::ranges::find(overlap, origin.airport->iata) != overlap.end();
                });
                if (origins.empty()) {
                    blocking.push_back("All airports near " + request.origin.name + " (" + join(overlap, ", ") +
                                       ") are also inside the destination area. Make the destination radius "
                                       "smaller or choose a destination farther away.");
                } else {
                    info.push_back(join(overlap, ", ") +
                                   " lie(s) in both the start and the destination area and is used as a "
                                   "destination only.");
                }
            }
        }
    }

    // 4. Nothing in range (03 §4.3.5): at most one NoAirportNote.
    if (origins.empty() && blocking.empty()) {
        blocking.push_back(
            no_airport_note(catalog, request.origin, request.origin_radius_km, AreaRole::start, locale));
    }
    if (request.destination && destinations.empty() && blocking.empty()) {
        blocking.push_back(no_airport_note(catalog, *request.destination, request.destination_radius_km,
                                           AreaRole::destination, locale));
    }

    const CodeSet dest_iatas = iata_set(destinations);
    CodeSet dest_groups;
    for (const AreaAirport& entry : destinations) {
        dest_groups.insert(entry.airport->group_key());
    }
    const CodeSet origin_iatas = iata_set(origins);

    std::vector<PlannedRequest> requests;
    std::vector<std::string> not_yet_flying;
    std::vector<std::string> mirror_only;
    OrderedCodeMap<double> outside_radius;
    int routes = 0;
    int suspended = 0;

    if (from <= to) {
        const std::vector<std::pair<Date, Date>> chunks = date_chunker::split(from, to, kMaxDaysPerRequest);
        for (const AreaAirport& origin_entry : origins) {  // nearest first
            const AirportPtr& origin = origin_entry.airport;
            CodeSet served_codes;
            for (const RouteConnection& connection : origin->connections) {
                served_codes.insert(connection.iata);
            }
            OrderedCodeMap<Route> by_group;

            for (const RouteConnection& connection : origin->connections) {  // map JSON order
                const AirportPtr target = catalog.find(connection.iata);
                if (!target) {
                    continue;
                }
                const std::string& group = target->group_key();

                // 5. Mirror stations (03 §4.3.7) — before the destination and home-area filters.
                if (!lists_back(catalog, *target, *origin)) {
                    if (!request.destination || dest_groups.contains(group)) {
                        AirportPtr sibling;
                        for (const AirportPtr& member : catalog.group_members(origin->group_key())) {
                            if (member->iata != origin->iata && lists_back(catalog, *target, *member)) {  // ordinal
                                sibling = member;
                                break;
                            }
                        }
                        std::string entry = origin->iata + " → " + group_name(catalog, group);
                        if (sibling) {
                            entry += " is flown from " + sibling->display() + ", " +
                                     format_km(distance_km(request.origin.location, sibling->location), locale) +
                                     " km away";
                        }
                        mirror_only.push_back(std::move(entry));
                    }
                    continue;
                }

                if (request.destination) {
                    // 6. Destination area (03 §4.3.8)
                    if (!dest_groups.contains(group)) {
                        continue;
                    }
                    bool in_radius = false;
                    if (target->is_fake_station) {
                        const std::vector<AirportPtr> members = catalog.group_members(group);
                        in_radius =
                            std::ranges::any_of(members, [&](const AirportPtr& m) { return dest_iatas.contains(m->iata); }) &&
                            std::ranges::none_of(members,
                                                 [&](const AirportPtr& m) { return served_codes.contains(m->iata); });
                    } else {
                        in_radius = dest_iatas.contains(target->iata);
                    }
                    if (!in_radius) {
                        if (!target->is_fake_station) {
                            outside_radius[target->iata] =
                                distance_km(request.destination->location, target->location);
                        }
                        continue;
                    }
                } else if (origin_iatas.contains(target->iata) ||
                           (target->is_fake_station &&
                            std::ranges::all_of(catalog.group_members(group), [&](const AirportPtr& m) {
                                return origin_iatas.contains(m->iata);
                            }))) {
                    // 7. "Anywhere": no hops inside the home area (03 §4.3.9)
                    continue;
                }

                // 8. Merge into one query per arrival group (03 §4.3.10, D-14)
                const bool is_dest =
                    !target->is_fake_station && (!request.destination || dest_iatas.contains(target->iata));
                const NextDeparture next = normalize_next_departure(connection.next_departure);
                if (Route* existing = by_group.find(group)) {
                    existing->next = merge_next_departures(existing->next, next);
                    if (is_dest && !existing->query_is_dest) {
                        existing->query_to = target->iata;
                        existing->query_is_dest = true;
                    }
                } else {
                    by_group[group] = Route{target->iata, next, is_dest};
                }
            }

            // 9. Date chunks per route (03 §4.3.11), insertion order
            for (const auto& [group, route] : by_group.entries()) {
                ++routes;
                if (route.next.is_suspended()) {
                    ++suspended;
                    continue;
                }
                const std::optional<Date> next_date = route.next.date();
                bool any = false;
                for (const auto& [chunk_start, chunk_end] : chunks) {
                    if (next_date && *next_date > chunk_end) {
                        continue;
                    }
                    requests.push_back(PlannedRequest{origin, route.query_to, group, chunk_start, chunk_end});
                    any = true;
                }
                if (!any && next_date) {
                    not_yet_flying.push_back(origin->iata + " → " + group_name(catalog, group) +
                                             ": next flight " + locale.format_date(*next_date, "dd MMM yyyy"));
                }
            }
        }
    } else {
        blocking.emplace_back("The date range is empty or entirely in the past.");
    }

    // 10. Why nothing could be planned (03 §4.2, §4.4 B6–B8, I3)
    if (!origins.empty() && (!request.destination || !destinations.empty()) && routes == 0 && from <= to) {
        std::vector<std::string> origin_codes;
        for (const AreaAirport& entry : origins) {
            origin_codes.push_back(entry.airport->iata);
        }
        const std::string origin_list = join(origin_codes, ", ", kOriginListLength);

        if (!outside_radius.empty() && request.destination) {
            const auto [iata, km] = by_distance(outside_radius).front();
            const double needed = std::ceil(km / 10) * 10;
            blocking.push_back("Wizz Air does not fly from " + origin_list + " to within " +
                               format_km(request.destination_radius_km, locale) + " km of " +
                               request.destination->name + ". Nearest served: " + display(catalog, iata) + ", " +
                               format_km(km, locale) + " km away — set the destination radius to at least " +
                               format_km(needed, locale) + " km.");
        } else if (std::ranges::any_of(mirror_only, [](const std::string& m) {
                       return m.find(" is flown from ") != std::string::npos;  // ordinal
                   })) {
            std::vector<std::string> flown;
            std::ranges::copy_if(mirror_only, std::back_inserter(flown),
                                 [](const std::string& m) { return m.find(" is flown from ") != std::string::npos; });
            blocking.push_back("Wizz Air flies this from a nearby sibling airport outside your " +
                               format_km(request.origin_radius_km, locale) +
                               " km start radius: " + join(flown, "; ", kListedEntries) + ". Increase the start radius.");
            mirror_only.clear();
        } else {
            std::vector<AirportPtr> origin_airports;
            for (const AreaAirport& entry : origins) {
                origin_airports.push_back(entry.airport);
            }
            const std::vector<std::string> reachable = reachable_names(catalog, origin_airports, origin_codes, locale);
            const std::string more = reachable.size() > kReachableNamesShown
                                         ? " and " + std::to_string(reachable.size() - kReachableNamesShown) + " more"
                                         : std::string{};
            const std::string list = reachable.empty() ? std::string{}
                                                       : " From " + origin_list + " Wizz Air flies to: " +
                                                             join(reachable, ", ", kReachableNamesShown) + more + ".";
            blocking.push_back("Wizz Air has no direct route between these areas." + list);
        }
    } else if (!outside_radius.empty()) {
        std::vector<std::string> nearest_served;
        for (const auto& [iata, km] : by_distance(outside_radius)) {
            if (nearest_served.size() == kListedEntries) {
                break;
            }
            nearest_served.push_back(display(catalog, iata) + " " + format_km(km, locale) + " km");
        }
        info.push_back("Served but outside your " + format_km(request.destination_radius_km, locale) +
                       " km destination radius: " + join(nearest_served, ", ") + ".");
    }

    if (!mirror_only.empty()) {
        info.push_back(std::to_string(mirror_only.size()) +
                       " route(s) skipped because Wizz Air flies them from a sibling airport (searched separately "
                       "if it is in range): " +
                       first_entries(mirror_only) + ".");
    }
    if (suspended > 0) {
        info.push_back(std::to_string(suspended) + " route(s) skipped: Wizz Air shows no upcoming flights on them.");
    }
    if (!not_yet_flying.empty()) {
        info.push_back(std::to_string(not_yet_flying.size()) +
                       " route(s) skipped because their next flight is after your dates: " +
                       first_entries(not_yet_flying) + ".");
    }

    // 11. Order: nearest origins first, nearest destination groups first (03 §4.3.12)
    std::unordered_map<std::string, std::size_t, text::OrdinalIgnoreCaseHash, text::OrdinalIgnoreCaseEqual> origin_rank;
    for (std::size_t i = 0; i < origins.size(); ++i) {
        origin_rank.emplace(origins[i].airport->iata, i);  // first index wins
    }
    std::unordered_map<std::string, int, text::OrdinalIgnoreCaseHash, text::OrdinalIgnoreCaseEqual> dest_rank;
    for (std::size_t i = 0; i < destinations.size(); ++i) {
        dest_rank.emplace(destinations[i].airport->group_key(), static_cast<int>(i));  // min index
    }
    const auto sort_key = [&](const PlannedRequest& r) {
        const auto rank = dest_rank.find(r.group_key);
        return std::tuple{origin_rank.at(r.from->iata), rank == dest_rank.end() ? INT_MAX : rank->second,
                          std::string_view{r.group_key}, r.start_date};
    };
    std::ranges::stable_sort(requests, [&](const PlannedRequest& a, const PlannedRequest& b) {
        return sort_key(a) < sort_key(b);
    });

    std::vector<std::string> notes = std::move(blocking);
    notes.insert(notes.end(), std::make_move_iterator(info.begin()), std::make_move_iterator(info.end()));

    SearchPlan result;
    result.origin_airports = std::move(origins);
    result.destination_airports = std::move(destinations);
    result.requests = std::move(requests);
    result.effective_from = from;
    result.effective_to = to;
    result.routes_considered = routes;
    result.routes_skipped_not_yet_flying = static_cast<int>(not_yet_flying.size()) + suspended;
    result.notes = std::move(notes);
    result.same_area = same_area;
    return result;
}

} // namespace wizz::planner
