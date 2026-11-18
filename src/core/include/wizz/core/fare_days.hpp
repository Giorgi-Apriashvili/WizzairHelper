#pragma once
// Per-day reuse of timetable rows (DECISIONS D-66, D-67). Pure: no I/O, no clock,
// no locking — the Wizz client owns one Store under its timetable mutex, passes
// "now" and "today" in, and writes `serialize`'s text to `cache\fares.json`.
//
// The unit of reuse is one day of one route: a timetable response holds at most one
// row per day and arrival station, and a day's rows do not depend on the window
// that was asked for. Each day remembers when it was fetched (the END of the fetch)
// and its rows in response order; a day without rows is still covered ("no flights
// that day"). A planned chunk then needs at most ONE request: the hull from the
// first to the last day that is not fresh, which never sends more requests than the
// plan, since a request costs the same whatever its length.

#include "wizz/core/domain.hpp"
#include "wizz/core/time.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wizz::fare_days {

/// The user's choice "Reuse fares fetched within" (settings.json `ReuseFaresWithin`, D-68).
enum class ReuseWindow : std::uint8_t { thirty_minutes, one_hour, three_hours, six_hours };

/// The length of a reuse window.
[[nodiscard]] constexpr Duration duration_of(ReuseWindow window) noexcept
{
    switch (window) {
    case ReuseWindow::one_hour:
        return std::chrono::hours{1};
    case ReuseWindow::three_hours:
        return std::chrono::hours{3};
    case ReuseWindow::six_hours:
        return std::chrono::hours{6};
    case ReuseWindow::thirty_minutes:
        break;
    }
    return std::chrono::minutes{30};
}

/// The longest window: older days are never reused whatever the setting, so they are pruned.
inline constexpr Duration kLongestReuse = std::chrono::hours{6};
/// Days dated today (machine-local) are reused for at most this long whatever the window:
/// departures drop out during the day and fares turn into "check on the site".
inline constexpr Duration kSameDayReuseLimit = std::chrono::minutes{30};
/// At most this many days are kept; beyond it the oldest fetches go first.
inline constexpr std::size_t kMaxStoredDays = 50'000;
/// The `version` of fares.json this code writes and reads.
inline constexpr int kFileVersion = 1;

/// One route as a timetable query names it: the query's codes (ordinal, case-sensitive),
/// price type and RAW adults count (0 ≠ 1, although both send adultCount 1) — the old
/// per-query cache key without its dates.
struct RouteKey {
    std::string from;
    std::string to;
    PriceType price_type = PriceType::regular;
    int adults = 1;

    friend bool operator==(const RouteKey&, const RouteKey&) = default;
    friend std::strong_ordering operator<=>(const RouteKey&, const RouteKey&) = default;
};

/// One stored day: when it was fetched and its rows in response order (empty = no flights).
struct CachedDay {
    UtcInstant fetched_at;
    std::vector<DayFare> fares;

    friend bool operator==(const CachedDay&, const CachedDay&) = default;
};

/// Everything stored for one route.
struct RouteDays {
    std::map<Date, CachedDay> days;
    /// When Wizz last answered InvalidMarket for the route (cleared by a successful fetch).
    std::optional<UtcInstant> not_served_at;

    friend bool operator==(const RouteDays&, const RouteDays&) = default;
};

/// What `Store::lookup` found for one planned chunk.
struct Lookup {
    /// A fresh "not served" mark answers the whole chunk; nothing else is filled then
    /// apart from `oldest` (the mark's time).
    bool not_served = false;
    /// The days to fetch (inclusive), or nullopt when every day is fresh.
    std::optional<std::pair<Date, Date>> fetch;
    /// Rows of the fresh days before the fetch (all of them when nothing is fetched),
    /// in date order and, within a day, in response order.
    std::vector<DayFare> before;
    /// Rows of the fresh days after the fetch.
    std::vector<DayFare> after;
    /// Days answered from the store.
    int reused_days = 0;
    /// The oldest fetch time among them (or of the not-served mark).
    std::optional<UtcInstant> oldest;
};

/// The per-day store. A value type: copy it to snapshot it.
class Store {
public:
    /// The chunk [first, last] of `key` at `now` with reuse window `window`. A day is fresh
    /// when it is stored, `fetched_at <= now` (a fetch time in the future is never trusted)
    /// and `now - fetched_at < limit`, where limit is `window`, or min(window,
    /// kSameDayReuseLimit) for days dated `today` or earlier. The fetch is the hull from the
    /// first to the last day that is not fresh; with nothing fresh it is the whole chunk,
    /// so the answer is then exactly the response. An empty chunk (last < first) needs nothing.
    [[nodiscard]] Lookup lookup(const RouteKey& key, Date first, Date last, UtcInstant now, Duration window,
                                Date today) const;

    /// Stores a successful answer for [first, last] fetched at `at`: every day of the range
    /// is replaced by the rows of `response` dated that day, in response order (days without
    /// rows are stored empty); rows dated outside the range are left out. Clears the route's
    /// not-served mark.
    void record(const RouteKey& key, Date first, Date last, std::span<const DayFare> response, UtcInstant at);

    /// Marks the route not served at `at` and forgets its days.
    void record_not_served(const RouteKey& key, UtcInstant at);

    /// Drops what can never be reused again: days and marks fetched kLongestReuse or more
    /// before `now` or after it, days dated before `today`, then routes left empty, then —
    /// beyond kMaxStoredDays — the days with the oldest fetch times.
    void prune(UtcInstant now, Date today);

    /// Days stored over all routes.
    [[nodiscard]] std::size_t day_count() const noexcept;

    [[nodiscard]] const std::map<RouteKey, RouteDays>& routes() const noexcept { return routes_; }

    friend bool operator==(const Store&, const Store&) = default;

private:
    friend Store parse(std::string_view json_text);

    std::map<RouteKey, RouteDays> routes_;
};

/// `fares.json` version 1, compact (D-67):
/// `{"version":1,"routes":[{"from","to","priceType":"regular"|"wdc","adults","notServedAt"?,
///   "covered":[{"first","last","fetchedAt"}],"fares":[{"date","from","to","departure",
///   "departures":[…],"amount","currency","checkPrice","mac"}]}]}`.
/// `covered` holds runs of consecutive days sharing one fetch time; amounts are written
/// exactly (their decimal text, scale kept); instants as ISO UTC, date-times with seven
/// fractional digits when they have a sub-second part.
[[nodiscard]] std::string serialize(const Store& store);

/// Reads `serialize`'s text. Throws (wizz::FormatError, json::Error) on ANY problem — wrong
/// version, a missing member or one of the wrong type, a bad date, an amount that does not
/// fit, overlapping runs, a fare outside every covered run, a route listed twice — so the
/// caller treats the whole file as a miss.
[[nodiscard]] Store parse(std::string_view json_text);

} // namespace wizz::fare_days
