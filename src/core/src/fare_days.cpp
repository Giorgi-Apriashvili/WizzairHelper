#include "wizz/core/fare_days.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/decimal.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/json.hpp"

#include <algorithm>
#include <expected>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>

namespace wizz::fare_days {

namespace {

constexpr std::string_view kRegular = "regular";
constexpr std::string_view kDiscountClub = "wdc";

/// Stored at `at`, fresh at `now` for `limit`: never a fetch time in the future (D-66).
[[nodiscard]] constexpr bool is_fresh(UtcInstant at, UtcInstant now, Duration limit) noexcept
{
    return at <= now && now - at < limit;
}

/// Kept by `prune`: neither in the future nor as old as the longest window.
[[nodiscard]] constexpr bool may_be_reused(UtcInstant at, UtcInstant now) noexcept
{
    return is_fresh(at, now, kLongestReuse);
}

void append(std::vector<DayFare>& target, const std::vector<DayFare>& source)
{
    target.insert(target.end(), source.begin(), source.end());
}

// ---------------------------------------------------------------------------
// Reading helpers: every problem throws, so the whole file is a miss.
// ---------------------------------------------------------------------------

[[noreturn]] void reject(std::string_view what)
{
    throw FormatError("fares.json: " + std::string{what});
}

[[nodiscard]] const json::Value& required(const json::Value& object, std::string_view name)
{
    const json::Value* value = object.find(name);
    if (value == nullptr) {
        reject("missing '" + std::string{name} + "'");
    }
    return *value;
}

[[nodiscard]] std::string string_of(const json::Value& object, std::string_view name)
{
    return std::string{required(object, name).as_string()};
}

[[nodiscard]] int int_of(const json::Value& object, std::string_view name)
{
    const json::Value& value = required(object, name);
    if (!value.is_number()) {
        reject("'" + std::string{name} + "' is not a number");
    }
    const std::optional<std::int32_t> number = value.try_int32();
    if (!number) {
        reject("'" + std::string{name} + "' is not an integer");
    }
    return *number;
}

[[nodiscard]] Date date_of_member(const json::Value& object, std::string_view name)
{
    const std::optional<Date> date = Date::parse_iso(required(object, name).as_string());
    if (!date) {
        reject("'" + std::string{name} + "' is not a date");
    }
    return *date;
}

[[nodiscard]] UtcInstant instant_of(const json::Value& value)
{
    const std::optional<UtcInstant> instant = parse_iso_utc(value.as_string());
    if (!instant) {
        reject("an instant is not ISO UTC");
    }
    return *instant;
}

[[nodiscard]] LocalDateTime local_of(const json::Value& value)
{
    const std::string_view text = value.as_string();
    const std::optional<LocalDateTime> time = parse_local_datetime(text);
    if (!time || format_iso_local(*time) != text) {
        reject("a departure is not yyyy-MM-ddTHH:mm:ss");
    }
    return *time;
}

[[nodiscard]] PriceType price_type_of(const json::Value& object)
{
    const std::string_view text = required(object, "priceType").as_string();
    if (text == kRegular) {
        return PriceType::regular;
    }
    if (text == kDiscountClub) {
        return PriceType::wizz_discount_club;
    }
    reject("unknown priceType");
}

[[nodiscard]] DayFare fare_of(const json::Value& row, Date date)
{
    DayFare fare;
    fare.from = string_of(row, "from");
    fare.to = string_of(row, "to");
    fare.date = date;
    fare.cheapest_departure = local_of(required(row, "departure"));
    for (const json::Value& departure : required(row, "departures").as_array()) {
        fare.departures.push_back(local_of(departure));
    }
    const json::Value& amount = required(row, "amount");
    if (!amount.is_number()) {
        reject("'amount' is not a number");
    }
    const std::expected<Decimal, DecimalParseError> parsed = Decimal::from_json_number(amount.number_lexeme());
    if (!parsed) {
        reject("'amount' does not fit a decimal");
    }
    fare.price_per_person = Money{.amount = *parsed, .currency = string_of(row, "currency")};
    fare.status = required(row, "checkPrice").as_bool() ? FareStatus::check_on_site : FareStatus::available;
    fare.has_mac_flight = required(row, "mac").as_bool();
    return fare;
}

void write_fare(json::Writer& writer, const DayFare& fare)
{
    writer.begin_object();
    writer.key("date").string(fare.date.iso());
    writer.key("from").string(fare.from);
    writer.key("to").string(fare.to);
    writer.key("departure").string(format_iso_local(fare.cheapest_departure));
    writer.key("departures").begin_array();
    for (const LocalDateTime departure : fare.departures) {
        writer.string(format_iso_local(departure));
    }
    writer.end_array();
    writer.key("amount").number_lexeme(fare.price_per_person.amount.to_string());
    writer.key("currency").string(fare.price_per_person.currency);
    writer.key("checkPrice").boolean(fare.status == FareStatus::check_on_site);
    writer.key("mac").boolean(fare.has_mac_flight);
    writer.end_object();
}

} // namespace

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

Lookup Store::lookup(const RouteKey& key, Date first, Date last, UtcInstant now, Duration window, Date today) const
{
    Lookup result;
    if (last < first) {
        return result;  // nothing asked, nothing to fetch
    }
    const auto route = routes_.find(key);
    if (route == routes_.end()) {
        result.fetch.emplace(first, last);
        return result;
    }
    const RouteDays& stored = route->second;
    if (stored.not_served_at && is_fresh(*stored.not_served_at, now, window)) {
        result.not_served = true;
        result.oldest = stored.not_served_at;
        return result;
    }

    const Duration same_day_limit = std::min(window, kSameDayReuseLimit);
    const auto fresh_day = [&](Date date) -> const CachedDay* {
        const auto found = stored.days.find(date);
        if (found == stored.days.end() ||
            !is_fresh(found->second.fetched_at, now, date <= today ? same_day_limit : window)) {
            return nullptr;
        }
        return &found->second;
    };

    std::optional<Date> hull_first;
    std::optional<Date> hull_last;
    for (Date date = first; date <= last; date = date.add_days(1)) {
        if (fresh_day(date) == nullptr) {
            if (!hull_first) {
                hull_first = date;
            }
            hull_last = date;
        }
    }

    const auto reuse = [&](Date from, Date to, std::vector<DayFare>& rows) {
        for (Date date = from; date <= to; date = date.add_days(1)) {
            const CachedDay* day = fresh_day(date);  // every day outside the hull is fresh
            append(rows, day->fares);
            ++result.reused_days;
            if (!result.oldest || day->fetched_at < *result.oldest) {
                result.oldest = day->fetched_at;
            }
        }
    };
    if (!hull_first) {
        reuse(first, last, result.before);
        return result;
    }
    result.fetch.emplace(*hull_first, *hull_last);
    reuse(first, hull_first->add_days(-1), result.before);
    reuse(hull_last->add_days(1), last, result.after);
    return result;
}

void Store::record(const RouteKey& key, Date first, Date last, std::span<const DayFare> response, UtcInstant at)
{
    RouteDays& stored = routes_[key];
    stored.not_served_at.reset();
    for (Date date = first; date <= last; date = date.add_days(1)) {
        stored.days.insert_or_assign(date, CachedDay{.fetched_at = at, .fares = {}});
    }
    for (const DayFare& fare : response) {
        if (fare.date >= first && fare.date <= last) {
            stored.days[fare.date].fares.push_back(fare);  // response order within the day
        }
    }
    if (stored.days.empty()) {
        routes_.erase(key);  // an empty range stores nothing
    }
}

void Store::record_not_served(const RouteKey& key, UtcInstant at)
{
    RouteDays& stored = routes_[key];
    stored.days.clear();
    stored.not_served_at = at;
}

void Store::prune(UtcInstant now, Date today)
{
    for (auto route = routes_.begin(); route != routes_.end();) {
        RouteDays& stored = route->second;
        std::erase_if(stored.days, [&](const auto& entry) {
            return entry.first < today || !may_be_reused(entry.second.fetched_at, now);
        });
        if (stored.not_served_at && !may_be_reused(*stored.not_served_at, now)) {
            stored.not_served_at.reset();
        }
        route = stored.days.empty() && !stored.not_served_at ? routes_.erase(route) : std::next(route);
    }

    const std::size_t stored_days = day_count();
    if (stored_days <= kMaxStoredDays) {
        return;
    }
    // The oldest fetches go first; equal times in route and date order (deterministic).
    std::vector<std::tuple<UtcInstant, RouteDays*, Date>> by_age;
    by_age.reserve(stored_days);
    for (auto& [key, stored] : routes_) {
        for (const auto& [date, day] : stored.days) {
            by_age.emplace_back(day.fetched_at, &stored, date);
        }
    }
    std::ranges::stable_sort(by_age, {}, [](const auto& entry) { return std::get<0>(entry); });
    for (std::size_t i = 0; i < stored_days - kMaxStoredDays; ++i) {
        std::get<1>(by_age[i])->days.erase(std::get<2>(by_age[i]));
    }
    std::erase_if(routes_, [](const auto& entry) { return entry.second.days.empty() && !entry.second.not_served_at; });
}

std::size_t Store::day_count() const noexcept
{
    std::size_t count = 0;
    for (const auto& [key, stored] : routes_) {
        count += stored.days.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// fares.json
// ---------------------------------------------------------------------------

std::string serialize(const Store& store)
{
    json::Writer writer;
    writer.begin_object();
    writer.key("version").integer(kFileVersion);
    writer.key("routes").begin_array();
    for (const auto& [key, stored] : store.routes()) {
        writer.begin_object();
        writer.key("from").string(key.from);
        writer.key("to").string(key.to);
        writer.key("priceType").string(key.price_type == PriceType::wizz_discount_club ? kDiscountClub : kRegular);
        writer.key("adults").integer(key.adults);
        if (stored.not_served_at) {
            writer.key("notServedAt").string(format_iso_utc(*stored.not_served_at));
        }
        writer.key("covered").begin_array();
        for (auto day = stored.days.begin(); day != stored.days.end();) {
            // One run: consecutive days fetched at the same time.
            const Date run_first = day->first;
            const UtcInstant fetched_at = day->second.fetched_at;
            Date run_last = run_first;
            for (++day; day != stored.days.end() && day->first == run_last.add_days(1) &&
                        day->second.fetched_at == fetched_at;
                 ++day) {
                run_last = day->first;
            }
            writer.begin_object();
            writer.key("first").string(run_first.iso());
            writer.key("last").string(run_last.iso());
            writer.key("fetchedAt").string(format_iso_utc(fetched_at));
            writer.end_object();
        }
        writer.end_array();
        writer.key("fares").begin_array();
        for (const auto& [date, day] : stored.days) {
            for (const DayFare& fare : day.fares) {
                write_fare(writer, fare);
            }
        }
        writer.end_array();
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return writer.take();
}

Store parse(std::string_view json_text)
{
    const json::Value root = json::parse(json_text);
    if (!root.is_object()) {
        reject("not an object");
    }
    if (int_of(root, "version") != kFileVersion) {
        reject("unknown version");
    }
    Store store;
    std::size_t stored_days = 0;
    for (const json::Value& route : required(root, "routes").as_array()) {
        if (!route.is_object()) {
            reject("a route is not an object");
        }
        RouteKey key{.from = string_of(route, "from"),
                     .to = string_of(route, "to"),
                     .price_type = price_type_of(route),
                     .adults = int_of(route, "adults")};
        RouteDays stored;
        if (const json::Value* not_served = route.find("notServedAt")) {
            stored.not_served_at = instant_of(*not_served);
        }
        for (const json::Value& run : required(route, "covered").as_array()) {
            const Date first = date_of_member(run, "first");
            const Date last = date_of_member(run, "last");
            const UtcInstant fetched_at = instant_of(required(run, "fetchedAt"));
            if (last < first) {
                reject("a covered run ends before it starts");
            }
            for (Date date = first; date <= last; date = date.add_days(1)) {
                if (++stored_days > kMaxStoredDays) {
                    reject("more days than are ever stored");
                }
                if (!stored.days.try_emplace(date, CachedDay{.fetched_at = fetched_at, .fares = {}}).second) {
                    reject("covered runs overlap");
                }
            }
        }
        for (const json::Value& row : required(route, "fares").as_array()) {
            const Date date = date_of_member(row, "date");
            const auto day = stored.days.find(date);
            if (day == stored.days.end()) {
                reject("a fare outside every covered run");
            }
            day->second.fares.push_back(fare_of(row, date));
        }
        if (!store.routes_.try_emplace(std::move(key), std::move(stored)).second) {
            reject("a route is listed twice");
        }
    }
    return store;
}

} // namespace wizz::fare_days
