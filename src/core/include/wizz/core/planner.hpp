#pragma once
// SearchPlanner (03 §4): the pure, offline first phase of a leg search. From a
// catalog, a LegSearchRequest and "today" it works out which departure
// airports to query, which arrival group to ask each of them for and which
// date chunks to request, and explains everything it skipped in notes whose
// texts are fixed by 03 §4.4 and 01 §18.5 A1–A16.
//
// Everything here is a free function of immutable inputs: no I/O, no clock, no
// state, safe to call concurrently. The UI calls `plan` for its preview and
// the flight search service calls it again with its own clock (03 §1).
//
// Locale (07 §1): the notes format distances with `{x:0}` (wizz::format_label,
// 07 §2) and dates with the locale's "yyyy-MM-dd" / "dd MMM yyyy" patterns,
// and ReachableNames collates and prefix-matches with the locale. The GUI
// passes the user locale; tests and diagnostics use the en-US default.
//
// Codes (IATA, MAC, group keys) are compared ordinally ignoring case (03 §0
// `ci`) except where 03 §4.2 says `ordinal` (overlap removal, the sibling
// exclusion, the GroupKey sort key, AirportsFor's pin de-duplication).

#include "wizz/core/catalog.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/time.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wizz::planner {

/// WizzApiClient.MaxDaysPerRequest (03 §2): the longest inclusive request window.
inline constexpr int kMaxDaysPerRequest = 42;
/// Place centres closer than this (strict <) are the same area (03 §4.3.3).
inline constexpr double kSameAreaKm = 40.0;
/// NoAirportNote adds its radius hint only up to this distance (03 §4.3.5).
inline constexpr double kRadiusHintMaxKm = 1500.0;
/// A next departure in a year before this means "nothing on sale" (03 §4.3.10).
inline constexpr int kSuspendedBeforeYear = 2000;

/// The next-departure state of a route (03 §4.3.10): an explicit tri-state,
/// never a sentinel date.
/// - unknown: the map gave no (parsable) date; such a route is never skipped;
/// - suspended: Wizz wrote a year before 2000 (1900-01-01); the route is skipped and counted;
/// - date: the naive local date-time of the next departure.
class NextDeparture {
public:
    enum class Kind : std::uint8_t { unknown, suspended, date };

    /// Unknown.
    constexpr NextDeparture() noexcept = default;

    [[nodiscard]] static constexpr NextDeparture unknown() noexcept { return NextDeparture{}; }
    [[nodiscard]] static constexpr NextDeparture suspended() noexcept
    {
        return NextDeparture{Kind::suspended, LocalDateTime{}};
    }
    /// A dated next departure (no normalisation: see normalize_next_departure).
    [[nodiscard]] static constexpr NextDeparture at(LocalDateTime time) noexcept
    {
        return NextDeparture{Kind::date, time};
    }

    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool is_unknown() const noexcept { return kind_ == Kind::unknown; }
    [[nodiscard]] constexpr bool is_suspended() const noexcept { return kind_ == Kind::suspended; }
    [[nodiscard]] constexpr bool is_date() const noexcept { return kind_ == Kind::date; }
    /// The date-time when kind() == date, else nullopt.
    [[nodiscard]] constexpr std::optional<LocalDateTime> time() const noexcept
    {
        return is_date() ? std::optional<LocalDateTime>{time_} : std::nullopt;
    }
    /// The calendar date of time() when kind() == date, else nullopt.
    [[nodiscard]] constexpr std::optional<Date> date() const noexcept
    {
        return is_date() ? std::optional<Date>{date_of(time_)} : std::nullopt;
    }

    friend constexpr bool operator==(const NextDeparture&, const NextDeparture&) noexcept = default;

private:
    constexpr NextDeparture(Kind kind, LocalDateTime time) noexcept : kind_(kind), time_(time) {}

    Kind kind_ = Kind::unknown;
    LocalDateTime time_{};  ///< meaningful only for Kind::date; default otherwise so == is exact
};

/// 03 §4.3.10 Normalize: nullopt → unknown; a value in a year before 2000 →
/// suspended; anything else → that date-time.
[[nodiscard]] NextDeparture normalize_next_departure(std::optional<LocalDateTime> next) noexcept;

/// 03 §4.3.10 Min: unknown wins ("never skip on missing data"); otherwise the
/// earlier of the two, where suspended counts as +∞.
/// {1900, 2027-01-05} → 2027-01-05; {unknown, 2027-03-01} → unknown; {1900, 1900} → suspended.
[[nodiscard]] NextDeparture merge_next_departures(NextDeparture a, NextDeparture b) noexcept;

/// The `{role}` of NoAirportNote (03 §4.3.5): the literal "start" or "destination".
enum class AreaRole : std::uint8_t { start, destination };

/// 03 §4.2 SearchPlanner.Plan. Decision order: date clamping (I1), area
/// airports, same area (B1) / overlap removal (B2, I2), no-airport notes (B3,
/// B4), per-origin connection walk in map order (mirror check first, then the
/// destination in-radius rule or the "anywhere" home-area exclusion, then the
/// per-group merge with QueryTo preference and the next-departure Min over the
/// connections that survived those filters, DECISIONS D-14), date chunks with
/// suspended / not-yet-flying skips, the "why nothing" note (B6–B8) or I3, then
/// I4–I6, and finally the stable request sort (origin rank, destination-group
/// rank, ordinal GroupKey, StartDate). `Notes` = blocking notes, then info notes.
/// `today` is the local calendar date of the caller's clock (03 §0).
[[nodiscard]] SearchPlan plan(const AirportCatalog& catalog, const LegSearchRequest& request, Date today,
                              const Locale& locale = *Locale::en_us());

/// 03 §4.3.2 AirportsFor: `catalog.airports_near(place.location, max(0, radius_km))`
/// plus every pinned code expanded with group_members_or_self (pin order; an
/// airport already present, compared ordinally, is not added again), measured
/// from the place, then stable-sorted by distance (ties: Near's name order, then
/// pin order). Only real stations; a NaN radius finds nothing near but still
/// adds the pins (.NET Math.Max(0, NaN) is NaN).
[[nodiscard]] std::vector<AreaAirport> airports_for(const AirportCatalog& catalog, const Place& place, double radius_km);

/// 03 §4.3.2 GroupMembersOrSelf: unknown code → []; a pseudo station → its real
/// members (catalog.group_members of its IATA); a real station → just itself.
[[nodiscard]] std::vector<AirportPtr> group_members_or_self(const AirportCatalog& catalog, std::string_view code);

/// 03 §4.3.5 NoAirportNote (01 §18.5 A5/A6):
/// "No Wizz Air airport within {radius_km:0} km of {place.name}. The nearest is
/// {nearest.Display}, {km:0} km away." plus " Set the {role} radius to at least
/// {ceil(km/10)*10:0} km." when km ≤ 1500; "No Wizz Air airport near the {role}
/// ({place.name})." when the catalog has no real station. `radius_km` is printed raw.
[[nodiscard]] std::string no_airport_note(const AirportCatalog& catalog, const Place& place, double radius_km,
                                          AreaRole role, const Locale& locale = *Locale::en_us());

/// 03 §4.3.7 ListsBack: is origin → target a genuine route? True when the
/// target lists the origin's IATA (or the origin's mac when that mac is a pseudo
/// station), when a pseudo target has a real member that does, or when the
/// target's mac is a pseudo station that does. Regional groups without a pseudo
/// station (SPQ, LTE, BUH, …) never count as the origin's city code.
[[nodiscard]] bool lists_back(const AirportCatalog& catalog, const Airport& target, const Airport& origin);

/// 03 §4.4 ReachableNames (the B8 list): for each origin (given order) and each
/// connection (map order) whose target is known, not in `exclude` (ci) and
/// genuine (lists_back), the target's name (the city name for a pseudo
/// station) goes into its group's set, ordered and de-duplicated by the
/// locale's tertiary collation (groups keyed ci, insertion order). A set gives
/// its shortest name (UTF-16 length, first in collation order) when every name
/// starts with it (culture, ignoring case), else all names joined " / ". The
/// result is stable-sorted by the locale's collation; duplicates across groups stay.
[[nodiscard]] std::vector<std::string> reachable_names(const AirportCatalog& catalog,
                                                       std::span<const AirportPtr> origins,
                                                       std::span<const std::string> exclude,
                                                       const Locale& locale = *Locale::en_us());

/// 03 §4.4 GroupName: a pseudo station → its city name ("London"); else the
/// group members: none → the code itself ("XXX"), one → its name ("Bratislava"
/// for VBR), several → their distinct names (ordinal, first kept) joined " / "
/// ("Katowice / Kraków").
[[nodiscard]] std::string group_name(const AirportCatalog& catalog, std::string_view group_key);

/// 03 §5.9 GroupLabel, the route name in progress text and warnings (01 §18.5 B9):
/// "{name} ({group_key})" for a pseudo station ("London (LON)"), "{name}
/// ({member.iata})" for a one-member group ("Bratislava (BTS)" for VBR,
/// "Barcelona El Prat (BCN)"), otherwise just the name ("Katowice / Kraków").
[[nodiscard]] std::string group_label(const AirportCatalog& catalog, std::string_view group_key);

/// 03 §4.2 `Display(iata)`: `catalog.find(iata)->display()` ("London Luton
/// (LTN)"), or the code itself when it is unknown.
[[nodiscard]] std::string display(const AirportCatalog& catalog, std::string_view iata);

} // namespace wizz::planner
