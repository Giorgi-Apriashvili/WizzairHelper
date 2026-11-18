// The per-day fare store (DECISIONS D-66) and fares.json (D-67): which days of a
// planned chunk are reused and which hull is fetched, the reuse window's edges,
// the same-day limit, not-served marks, pruning and the file format. Pure: every
// instant and "today" is passed in.

#include "wizz/core/fare_days.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/decimal.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/time.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using wizz::Date;
using wizz::DayFare;
using wizz::Decimal;
using wizz::Duration;
using wizz::FareStatus;
using wizz::Money;
using wizz::PriceType;
using wizz::Ticks;
using wizz::UtcInstant;
using wizz::fare_days::CachedDay;
using wizz::fare_days::Lookup;
using wizz::fare_days::ReuseWindow;
using wizz::fare_days::RouteKey;
using wizz::fare_days::Store;

constexpr Date day(unsigned month, unsigned d, int year = 2026) noexcept
{
    return Date::from_ymd(year, month, d);
}

/// 2026-11-02T10:00:00Z, "today" 2026-11-02 (the suite's fixed clock).
constexpr UtcInstant kNow = wizz::make_utc(day(11, 2), 10, 0);
constexpr Date kToday = day(11, 2);
constexpr Duration kWindow = 30min;

[[nodiscard]] Decimal dec(std::string_view lexeme)
{
    const auto parsed = Decimal::from_json_number(lexeme);
    if (!parsed) {
        throw std::logic_error("bad decimal literal in a test");
    }
    return *parsed;
}

[[nodiscard]] RouteKey bud_ltn()
{
    return RouteKey{.from = "BUD", .to = "LTN", .price_type = PriceType::regular, .adults = 1};
}

/// One row dated `date`, departing at `hour`:00 for `amount` HUF.
[[nodiscard]] DayFare fare(Date date, int hour = 6, std::string_view amount = "21590.0", std::string to = "LTN")
{
    return DayFare{.from = "BUD",
                   .to = std::move(to),
                   .date = date,
                   .cheapest_departure = wizz::make_local(date, hour, 0),
                   .departures = {wizz::make_local(date, hour, 0), wizz::make_local(date, 17, 30)},
                   .price_per_person = Money{.amount = dec(amount), .currency = "HUF"},
                   .status = FareStatus::available,
                   .has_mac_flight = false};
}

/// One row per day of [first, last].
[[nodiscard]] std::vector<DayFare> rows(Date first, Date last)
{
    std::vector<DayFare> result;
    for (Date date = first; date <= last; date = date.add_days(1)) {
        result.push_back(fare(date));
    }
    return result;
}

[[nodiscard]] std::vector<Date> dates_of(const std::vector<DayFare>& fares)
{
    std::vector<Date> dates;
    for (const DayFare& f : fares) {
        dates.push_back(f.date);
    }
    return dates;
}

[[nodiscard]] std::optional<std::pair<Date, Date>> hull(Date first, Date last)
{
    return std::pair{first, last};
}

/// BUD→LTN 2026-11-10..2026-11-20 fetched at kNow.
[[nodiscard]] Store stored_chunk()
{
    Store store;
    store.record(bud_ltn(), day(11, 10), day(11, 20), rows(day(11, 10), day(11, 20)), kNow);
    return store;
}

// ---------------------------------------------------------------------------
// Windows

TEST(FareDays, Reuse_windows_and_constants)
{
    using wizz::fare_days::duration_of;
    EXPECT_EQ(duration_of(ReuseWindow::thirty_minutes), Duration{30min});
    EXPECT_EQ(duration_of(ReuseWindow::one_hour), Duration{1h});
    EXPECT_EQ(duration_of(ReuseWindow::three_hours), Duration{3h});
    EXPECT_EQ(duration_of(ReuseWindow::six_hours), Duration{6h});
    EXPECT_EQ(wizz::fare_days::kLongestReuse, Duration{6h});
    EXPECT_EQ(wizz::fare_days::kSameDayReuseLimit, Duration{30min});
    EXPECT_EQ(wizz::fare_days::kMaxStoredDays, 50'000u);
}

// ---------------------------------------------------------------------------
// lookup: the hull

TEST(FareDays, An_empty_store_fetches_the_whole_chunk)
{
    const Store store;
    const Lookup look = store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow, kWindow, kToday);
    EXPECT_FALSE(look.not_served);
    EXPECT_EQ(look.fetch, hull(day(11, 10), day(11, 20)));
    EXPECT_TRUE(look.before.empty());
    EXPECT_TRUE(look.after.empty());
    EXPECT_EQ(look.reused_days, 0);
    EXPECT_EQ(look.oldest, std::nullopt);
}

TEST(FareDays, An_identical_repeat_fetches_nothing_and_keeps_date_and_response_order)
{
    Store store;
    // Response order within a day is kept (two arrival siblings on 11-12, LGW first);
    // the days themselves come back in date order.
    const std::vector<DayFare> response{fare(day(11, 12), 8, "34690.0", "LGW"), fare(day(11, 11)),
                                        fare(day(11, 12), 6, "21590.0", "LTN"), fare(day(11, 10))};
    store.record(bud_ltn(), day(11, 10), day(11, 12), response, kNow);

    const Lookup look = store.lookup(bud_ltn(), day(11, 10), day(11, 12), kNow + 10min, kWindow, kToday);
    EXPECT_EQ(look.fetch, std::nullopt);
    EXPECT_FALSE(look.not_served);
    EXPECT_EQ(look.reused_days, 3);
    EXPECT_EQ(look.oldest, kNow);
    ASSERT_EQ(look.before.size(), 4u);
    EXPECT_TRUE(look.after.empty());
    EXPECT_EQ(look.before[0], response[3]);
    EXPECT_EQ(look.before[1], response[1]);
    EXPECT_EQ(look.before[2], response[0]);  // LGW first, as answered
    EXPECT_EQ(look.before[3], response[2]);
}

TEST(FareDays, A_chunk_shifted_by_one_day_fetches_only_its_new_last_day)
{
    const Store store = stored_chunk();
    const Lookup look = store.lookup(bud_ltn(), day(11, 11), day(11, 21), kNow, kWindow, kToday);
    EXPECT_EQ(look.fetch, hull(day(11, 21), day(11, 21)));
    EXPECT_EQ(dates_of(look.before), dates_of(rows(day(11, 11), day(11, 20))));
    EXPECT_TRUE(look.after.empty());
    EXPECT_EQ(look.reused_days, 10);
    EXPECT_EQ(look.oldest, kNow);
}

TEST(FareDays, A_narrower_chunk_inside_a_stored_one_fetches_nothing)
{
    const Store store = stored_chunk();
    const Lookup look = store.lookup(bud_ltn(), day(11, 12), day(11, 15), kNow, kWindow, kToday);
    EXPECT_EQ(look.fetch, std::nullopt);
    EXPECT_EQ(dates_of(look.before), dates_of(rows(day(11, 12), day(11, 15))));
}

TEST(FareDays, A_gap_in_the_middle_fetches_only_the_hull)
{
    Store store;
    store.record(bud_ltn(), day(11, 10), day(11, 12), rows(day(11, 10), day(11, 12)), kNow);
    store.record(bud_ltn(), day(11, 15), day(11, 16), rows(day(11, 15), day(11, 16)), kNow);
    store.record(bud_ltn(), day(11, 19), day(11, 20), rows(day(11, 19), day(11, 20)), kNow);

    // 11-13..11-18 holds two gaps and the covered 11-15/16 between them: one request
    // for the hull 11-13..11-18, the covered days inside it are fetched again.
    const Lookup look = store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow, kWindow, kToday);
    EXPECT_EQ(look.fetch, hull(day(11, 13), day(11, 18)));
    EXPECT_EQ(dates_of(look.before), dates_of(rows(day(11, 10), day(11, 12))));
    EXPECT_EQ(dates_of(look.after), dates_of(rows(day(11, 19), day(11, 20))));
    EXPECT_EQ(look.reused_days, 5);
}

TEST(FareDays, Gaps_at_both_ends_fetch_the_whole_chunk_and_reuse_nothing)
{
    const Store store = stored_chunk();
    const Lookup look = store.lookup(bud_ltn(), day(11, 9), day(11, 21), kNow, kWindow, kToday);
    EXPECT_EQ(look.fetch, hull(day(11, 9), day(11, 21)));
    EXPECT_TRUE(look.before.empty());
    EXPECT_TRUE(look.after.empty());
    EXPECT_EQ(look.reused_days, 0);
    EXPECT_EQ(look.oldest, std::nullopt);
}

TEST(FareDays, An_empty_chunk_needs_nothing)
{
    const Store store = stored_chunk();
    const Lookup look = store.lookup(bud_ltn(), day(11, 12), day(11, 11), kNow, kWindow, kToday);
    EXPECT_EQ(look.fetch, std::nullopt);
    EXPECT_EQ(look.reused_days, 0);
}

TEST(FareDays, The_oldest_reused_fetch_time_is_reported)
{
    Store store;
    store.record(bud_ltn(), day(11, 10), day(11, 12), rows(day(11, 10), day(11, 12)), kNow - 20min);
    store.record(bud_ltn(), day(11, 13), day(11, 15), rows(day(11, 13), day(11, 15)), kNow - 5min);
    const Lookup look = store.lookup(bud_ltn(), day(11, 11), day(11, 16), kNow, kWindow, kToday);
    EXPECT_EQ(look.fetch, hull(day(11, 16), day(11, 16)));
    EXPECT_EQ(look.oldest, kNow - 20min);
}

// ---------------------------------------------------------------------------
// lookup: freshness

TEST(FareDays, The_window_is_strict_one_tick_before_it_is_reused_at_it_is_not)
{
    const Store store = stored_chunk();
    for (const Duration window : {Duration{30min}, Duration{1h}, Duration{3h}, Duration{6h}}) {
        const Lookup inside = store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow + window - Ticks{1}, window, kToday);
        EXPECT_EQ(inside.fetch, std::nullopt) << window.count();
        const Lookup at_edge = store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow + window, window, kToday);
        EXPECT_EQ(at_edge.fetch, hull(day(11, 10), day(11, 20))) << window.count();
    }
}

TEST(FareDays, A_fetch_time_in_the_future_is_never_reused)
{
    const Store store = stored_chunk();
    const Lookup look = store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow - Ticks{1}, 6h, kToday);
    EXPECT_EQ(look.fetch, hull(day(11, 10), day(11, 20)));
    EXPECT_EQ(look.reused_days, 0);
}

TEST(FareDays, Days_dated_today_are_reused_for_at_most_30_minutes_whatever_the_window)
{
    Store store;
    store.record(bud_ltn(), day(11, 2), day(11, 5), rows(day(11, 2), day(11, 5)), kNow);

    // 31 minutes later with a 6-hour window: today (11-02) is fetched again, the
    // later days are reused.
    const Lookup later = store.lookup(bud_ltn(), day(11, 2), day(11, 5), kNow + 31min, 6h, kToday);
    EXPECT_EQ(later.fetch, hull(day(11, 2), day(11, 2)));
    EXPECT_EQ(dates_of(later.after), dates_of(rows(day(11, 3), day(11, 5))));

    // 29 minutes later everything is reused.
    EXPECT_EQ(store.lookup(bud_ltn(), day(11, 2), day(11, 5), kNow + 29min, 6h, kToday).fetch, std::nullopt);
    // A window shorter than the limit still applies to today.
    EXPECT_EQ(store.lookup(bud_ltn(), day(11, 2), day(11, 2), kNow + 10min, 10min, kToday).fetch,
              hull(day(11, 2), day(11, 2)));
    // The next day, 11-03 is "today": the 30-minute limit moves with the date.
    EXPECT_EQ(store.lookup(bud_ltn(), day(11, 3), day(11, 5), kNow + 31min, 6h, day(11, 3)).fetch,
              hull(day(11, 3), day(11, 3)));
}

// ---------------------------------------------------------------------------
// record

TEST(FareDays, Rows_outside_the_recorded_range_are_not_stored)
{
    Store store;
    std::vector<DayFare> response = rows(day(11, 8), day(11, 14));  // two days early, two days late
    store.record(bud_ltn(), day(11, 10), day(11, 12), response, kNow);
    EXPECT_EQ(store.day_count(), 3u);
    const auto& days = store.routes().at(bud_ltn()).days;
    EXPECT_FALSE(days.contains(day(11, 9)));
    EXPECT_FALSE(days.contains(day(11, 13)));
    EXPECT_EQ(dates_of(days.at(day(11, 10)).fares), (std::vector<Date>{day(11, 10)}));

    EXPECT_EQ(store.lookup(bud_ltn(), day(11, 9), day(11, 9), kNow, kWindow, kToday).fetch, hull(day(11, 9), day(11, 9)));
}

TEST(FareDays, Days_without_rows_count_as_covered)
{
    Store store;
    store.record(bud_ltn(), day(11, 10), day(11, 20), std::vector<DayFare>{fare(day(11, 15))}, kNow);
    EXPECT_EQ(store.day_count(), 11u);
    const Lookup look = store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow, kWindow, kToday);
    EXPECT_EQ(look.fetch, std::nullopt);
    EXPECT_EQ(look.reused_days, 11);
    EXPECT_EQ(dates_of(look.before), (std::vector<Date>{day(11, 15)}));

    // A whole answer without rows ("no flights") is covered as well.
    store.record(bud_ltn(), day(12, 1), day(12, 5), {}, kNow);
    EXPECT_EQ(store.lookup(bud_ltn(), day(12, 1), day(12, 5), kNow, kWindow, kToday).fetch, std::nullopt);
}

TEST(FareDays, A_later_record_replaces_a_day_and_its_rows)
{
    Store store = stored_chunk();
    store.record(bud_ltn(), day(11, 12), day(11, 12), std::vector<DayFare>{fare(day(11, 12), 9, "9990.0")},
                 kNow + 5min);
    const CachedDay& replaced = store.routes().at(bud_ltn()).days.at(day(11, 12));
    EXPECT_EQ(replaced.fetched_at, kNow + 5min);
    ASSERT_EQ(replaced.fares.size(), 1u);
    EXPECT_EQ(replaced.fares[0].price_per_person.amount, dec("9990.0"));
}

TEST(FareDays, Not_served_answers_the_chunk_while_fresh_erases_days_and_a_record_clears_it)
{
    Store store = stored_chunk();
    store.record_not_served(bud_ltn(), kNow);
    EXPECT_EQ(store.day_count(), 0u);

    const Lookup look = store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow + 10min, kWindow, kToday);
    EXPECT_TRUE(look.not_served);
    EXPECT_EQ(look.fetch, std::nullopt);
    EXPECT_EQ(look.oldest, kNow);
    // Any chunk of the route.
    EXPECT_TRUE(store.lookup(bud_ltn(), day(12, 20), day(12, 30), kNow, kWindow, kToday).not_served);
    // Stale or in the future: the chunk is fetched.
    const Lookup stale = store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow + kWindow, kWindow, kToday);
    EXPECT_FALSE(stale.not_served);
    EXPECT_EQ(stale.fetch, hull(day(11, 10), day(11, 20)));
    EXPECT_FALSE(store.lookup(bud_ltn(), day(11, 10), day(11, 20), kNow - Ticks{1}, kWindow, kToday).not_served);

    store.record(bud_ltn(), day(11, 10), day(11, 11), rows(day(11, 10), day(11, 11)), kNow + 1min);
    EXPECT_EQ(store.routes().at(bud_ltn()).not_served_at, std::nullopt);
    const Lookup after = store.lookup(bud_ltn(), day(11, 10), day(11, 12), kNow + 1min, kWindow, kToday);
    EXPECT_FALSE(after.not_served);
    EXPECT_EQ(after.fetch, hull(day(11, 12), day(11, 12)));
}

TEST(FareDays, Every_key_field_separates_routes)
{
    const Store store = stored_chunk();
    const auto misses = [&](RouteKey key) {
        return store.lookup(key, day(11, 10), day(11, 20), kNow, kWindow, kToday).fetch.has_value();
    };
    EXPECT_FALSE(misses(bud_ltn()));
    RouteKey other = bud_ltn();
    other.price_type = PriceType::wizz_discount_club;
    EXPECT_TRUE(misses(other));
    other = bud_ltn();
    other.adults = 0;  // sends adultCount 1 like the original, but is another key
    EXPECT_TRUE(misses(other));
    other = bud_ltn();
    other.adults = 2;
    EXPECT_TRUE(misses(other));
    other = bud_ltn();
    other.from = "bud";  // ordinal, case-sensitive
    EXPECT_TRUE(misses(other));
    other = bud_ltn();
    other.to = "LGW";
    EXPECT_TRUE(misses(other));
}

// ---------------------------------------------------------------------------
// prune

TEST(FareDays, Prune_drops_old_future_and_past_days_and_empty_routes)
{
    Store store;
    const RouteKey old_route{.from = "BUD", .to = "NAP", .price_type = PriceType::regular, .adults = 1};
    const RouteKey marked{.from = "BUD", .to = "PRG", .price_type = PriceType::regular, .adults = 1};
    const RouteKey future{.from = "BUD", .to = "VIE", .price_type = PriceType::regular, .adults = 1};
    store.record(bud_ltn(), day(10, 31), day(11, 4), rows(day(10, 31), day(11, 4)), kNow - 6h + Ticks{1});
    store.record(old_route, day(11, 10), day(11, 12), rows(day(11, 10), day(11, 12)), kNow - 6h);
    store.record_not_served(marked, kNow - 6h);
    store.record(future, day(11, 10), day(11, 10), rows(day(11, 10), day(11, 10)), kNow + Ticks{1});

    store.prune(kNow, kToday);
    // BUD→LTN keeps 11-02..11-04 (today counts; 6 h minus a tick is young enough).
    ASSERT_EQ(store.routes().size(), 1u);
    EXPECT_EQ(store.day_count(), 3u);
    EXPECT_TRUE(store.routes().at(bud_ltn()).days.contains(kToday));
    EXPECT_FALSE(store.routes().at(bud_ltn()).days.contains(day(11, 1)));

    // A fresh not-served mark keeps its route.
    store.record_not_served(marked, kNow - 1h);
    store.prune(kNow, kToday);
    EXPECT_EQ(store.routes().size(), 2u);
    EXPECT_TRUE(store.routes().contains(marked));
}

TEST(FareDays, Prune_caps_the_store_by_dropping_the_oldest_fetches_first)
{
    Store store;
    const std::size_t routes = wizz::fare_days::kMaxStoredDays / 100 + 1;  // 501 routes × 100 days
    for (std::size_t i = 0; i < routes; ++i) {
        const RouteKey key{.from = "BUD", .to = std::format("X{:03}", i), .price_type = PriceType::regular, .adults = 1};
        store.record(key, day(11, 3), day(11, 3).add_days(99), {}, kNow - 1h + std::chrono::seconds{i});
    }
    ASSERT_EQ(store.day_count(), routes * 100);
    store.prune(kNow, kToday);
    EXPECT_EQ(store.day_count(), wizz::fare_days::kMaxStoredDays);
    // The first route was fetched first and is gone; the last one is whole.
    EXPECT_FALSE(store.routes().contains(RouteKey{.from = "BUD", .to = "X000", .price_type = PriceType::regular, .adults = 1}));
    EXPECT_EQ(store.routes().at(RouteKey{.from = "BUD", .to = "X500", .price_type = PriceType::regular, .adults = 1}).days.size(),
              100u);
}

// ---------------------------------------------------------------------------
// fares.json

[[nodiscard]] Store rich_store()
{
    Store store;
    DayFare check = fare(day(11, 11), 12, "0", "LTN");
    check.status = FareStatus::check_on_site;
    check.has_mac_flight = true;
    DayFare odd = fare(day(11, 12), 6, "21590.10", "LGW");
    odd.cheapest_departure += Ticks{1'234'567};                 // a sub-second part
    odd.departures = {odd.cheapest_departure};
    odd.price_per_person.currency = "EUR";
    store.record(bud_ltn(), day(11, 10), day(11, 13), std::vector<DayFare>{fare(day(11, 10)), check, odd}, kNow);
    store.record(bud_ltn(), day(11, 14), day(11, 15), {}, kNow + Ticks{5'000'001});  // a second run, no rows
    store.record(bud_ltn(), day(11, 20), day(11, 20), rows(day(11, 20), day(11, 20)), kNow);  // not adjacent
    store.record_not_served(RouteKey{.from = "BUD", .to = "PRG", .price_type = PriceType::wizz_discount_club, .adults = 0},
                            kNow - 1min);
    return store;
}

TEST(FareDaysFile, Serialize_and_parse_round_trip)
{
    const Store store = rich_store();
    const std::string text = wizz::fare_days::serialize(store);
    const Store back = wizz::fare_days::parse(text);
    EXPECT_EQ(back, store);
    EXPECT_EQ(wizz::fare_days::serialize(back), text);

    // The decimal's scale and the sub-second part survive (== alone ignores the scale).
    const auto& days = back.routes().at(bud_ltn()).days;
    EXPECT_EQ(days.at(day(11, 12)).fares[0].price_per_person.amount.to_string(), "21590.10");
    EXPECT_EQ(days.at(day(11, 10)).fares[0].price_per_person.amount.to_string(), "21590.0");
    EXPECT_EQ(days.at(day(11, 11)).fares[0].status, FareStatus::check_on_site);
    EXPECT_TRUE(days.at(day(11, 11)).fares[0].has_mac_flight);
    EXPECT_EQ(days.at(day(11, 14)).fetched_at, kNow + Ticks{5'000'001});

    EXPECT_TRUE(text.starts_with(R"({"version":1,"routes":[)")) << text;
    EXPECT_TRUE(text.contains(
        R"("covered":[{"first":"2026-11-10","last":"2026-11-13","fetchedAt":"2026-11-02T10:00:00Z"},)"
        R"({"first":"2026-11-14","last":"2026-11-15","fetchedAt":"2026-11-02T10:00:00.5000001Z"},)"
        R"({"first":"2026-11-20","last":"2026-11-20","fetchedAt":"2026-11-02T10:00:00Z"}])"))
        << text;
    EXPECT_TRUE(text.contains(
        R"({"date":"2026-11-12","from":"BUD","to":"LGW","departure":"2026-11-12T06:00:00.1234567",)"
        R"("departures":["2026-11-12T06:00:00.1234567"],"amount":21590.10,"currency":"EUR","checkPrice":false,"mac":false})"))
        << text;
    EXPECT_TRUE(text.contains(
        R"({"from":"BUD","to":"PRG","priceType":"wdc","adults":0,"notServedAt":"2026-11-02T09:59:00Z","covered":[],"fares":[]})"))
        << text;
}

TEST(FareDaysFile, An_empty_store_round_trips)
{
    const Store empty;
    EXPECT_EQ(wizz::fare_days::serialize(empty), R"({"version":1,"routes":[]})");
    EXPECT_EQ(wizz::fare_days::parse(R"({"version":1,"routes":[]})"), empty);
}

/// `parse` throws something for `text`.
void expect_rejected(std::string_view text)
{
    EXPECT_THROW((void)wizz::fare_days::parse(text), std::exception) << text;
}

TEST(FareDaysFile, Parse_rejects_anything_malformed)
{
    const std::string good = wizz::fare_days::serialize(rich_store());
    ASSERT_NO_THROW((void)wizz::fare_days::parse(good));
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string text = good;
        const auto at = text.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        if (at != std::string::npos) {
            text.replace(at, from.size(), to);
        }
        return text;
    };

    // Garbage, other roots and versions.
    expect_rejected("");
    expect_rejected("not json");
    expect_rejected(good.substr(0, good.size() - 1));
    expect_rejected("[]");
    expect_rejected("null");
    expect_rejected(R"({"routes":[]})");
    expect_rejected(R"({"version":2,"routes":[]})");
    expect_rejected(R"({"version":"1","routes":[]})");
    expect_rejected(R"({"version":1.0,"routes":[]})");
    expect_rejected(R"({"version":1})");
    expect_rejected(R"({"version":1,"routes":{}})");
    expect_rejected(R"({"version":1,"routes":[1]})");
    // Missing members and wrong types.
    expect_rejected(replaced(R"("from":"BUD","to":"LTN","priceType")", R"("to":"LTN","priceType")"));
    expect_rejected(replaced(R"("priceType":"regular")", R"("priceType":"Regular")"));
    expect_rejected(replaced(R"("adults":1)", R"("adults":"1")"));
    expect_rejected(replaced(R"("adults":1)", R"("adults":1.5)"));
    expect_rejected(replaced(R"("checkPrice":false)", R"("checkPrice":0)"));
    expect_rejected(replaced(R"(,"mac":false)", ""));
    expect_rejected(replaced(R"("currency":"EUR")", R"("currency":null)"));
    expect_rejected(replaced(R"("amount":21590.10)", R"("amount":"21590.10")"));
    expect_rejected(replaced(R"("amount":21590.10)", R"("amount":1e40)"));  // does not fit a decimal
    expect_rejected(replaced(R"("notServedAt":"2026-11-02T09:59:00Z")", R"("notServedAt":null)"));
    expect_rejected(replaced(R"("notServedAt":"2026-11-02T09:59:00Z")", R"("notServedAt":"2026-11-02T09:59:00")"));
    // Bad dates and instants.
    expect_rejected(replaced(R"("first":"2026-11-10")", R"("first":"2026-11-31")"));
    expect_rejected(replaced(R"("fetchedAt":"2026-11-02T10:00:00.5000001Z")", R"("fetchedAt":"2026-11-02 10:00:00Z")"));
    expect_rejected(replaced(R"("departure":"2026-11-12T06:00:00.1234567")", R"("departure":"2026-11-12T06:00")"));
    expect_rejected(replaced(R"("date":"2026-11-12")", R"("date":"12/11/2026")"));
    // Runs that end before they start or overlap, fares outside every run.
    expect_rejected(replaced(R"("first":"2026-11-14","last":"2026-11-15")", R"("first":"2026-11-15","last":"2026-11-14")"));
    expect_rejected(replaced(R"("first":"2026-11-14","last":"2026-11-15")", R"("first":"2026-11-13","last":"2026-11-15")"));
    expect_rejected(replaced(R"("date":"2026-11-12")", R"("date":"2026-11-18")"));
    // A run longer than the store ever holds.
    expect_rejected(replaced(R"("first":"2026-11-20","last":"2026-11-20")", R"("first":"2026-11-20","last":"2199-12-31")"));
}

TEST(FareDaysFile, A_route_listed_twice_is_rejected)
{
    Store store;
    store.record(bud_ltn(), day(11, 10), day(11, 10), {}, kNow);
    const std::string one = wizz::fare_days::serialize(store);
    const std::string route = one.substr(one.find("[{") + 1, one.size() - one.find("[{") - 3);
    const std::string twice = std::format(R"({{"version":1,"routes":[{},{}]}})", route, route);
    ASSERT_NO_THROW((void)wizz::fare_days::parse(std::format(R"({{"version":1,"routes":[{}]}})", route)));
    expect_rejected(twice);
}

// ---------------------------------------------------------------------------
// The date-time helpers of fares.json

TEST(FareDaysFile, Iso_helpers_round_trip_and_parse_iso_utc_is_strict)
{
    const wizz::LocalDateTime local = wizz::make_local(day(11, 12), 6, 5, 4) + Ticks{1'234'567};
    EXPECT_EQ(wizz::format_iso_local(local), "2026-11-12T06:05:04.1234567");
    EXPECT_EQ(wizz::format_iso_local(wizz::make_local(day(11, 12), 6, 5, 4)), "2026-11-12T06:05:04");
    EXPECT_EQ(wizz::parse_local_datetime(wizz::format_iso_local(local)), local);

    EXPECT_EQ(wizz::parse_iso_utc("2026-11-02T10:00:00Z"), kNow);
    EXPECT_EQ(wizz::parse_iso_utc("2026-11-02T10:00:00.5Z"), kNow + 500ms);
    EXPECT_EQ(wizz::parse_iso_utc("2026-11-02T10:00:00.0000001Z"), kNow + Ticks{1});
    EXPECT_EQ(wizz::parse_iso_utc(wizz::format_iso_utc(kNow + Ticks{1'234'567})), kNow + Ticks{1'234'567});
    for (const std::string_view bad : {"2026-11-02T10:00:00", "2026-11-02T10:00Z", "2026-11-02 10:00:00Z",
                                       " 2026-11-02T10:00:00Z", "2026-11-02T10:00:00Z ", "2026-11-02T10:00:00+00:00",
                                       "2026-11-02T24:00:00Z", "2026-02-30T10:00:00Z", "2026-11-02T10:00:00.12345678Z",
                                       "2026-11-02T10:00:00.Z", "0000-11-02T10:00:00Z", ""}) {
        EXPECT_EQ(wizz::parse_iso_utc(bad), std::nullopt) << bad;
    }
}

} // namespace
