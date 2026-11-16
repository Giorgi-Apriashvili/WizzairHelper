// Civil time helpers, naive date-time parsing (02 §2.4, 04 §9.4, D-22),
// DateChunker (03 §2, 04 §9.1, CT-CHK-01/02) and the clocks (05 §1.1).

#include "wizz/core/clocks.hpp"
#include "wizz/core/date_chunker.hpp"
#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/time.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <climits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::Date;
using wizz::LocalDateTime;
using wizz::Ticks;
using wizz::make_local;
using wizz::parse_local_datetime;

constexpr Date d(int year, unsigned month, unsigned day)
{
    return Date::from_ymd(year, month, day);
}

// ---------------------------------------------------------------------------
// Date / time.hpp helpers

TEST(Time, Date_helpers_split_and_join_wall_clock_values)
{
    const LocalDateTime t = make_local(d(2026, 11, 15), 8, 30, 15) + Ticks{1'230'000};
    EXPECT_EQ(wizz::date_of(t), d(2026, 11, 15));
    EXPECT_EQ(wizz::time_of_day(t), 8h + 30min + 15s + Ticks{1'230'000});
    EXPECT_EQ(wizz::at_midnight(d(2026, 11, 15)), make_local(d(2026, 11, 15), 0, 0));
    EXPECT_EQ(days_between(d(2027, 2, 7), d(2026, 11, 10)), 89);
    EXPECT_EQ(d(2026, 11, 2).weekday(), std::chrono::Monday);
}

TEST(Time, Make_utc_builds_the_fixed_test_instant)
{
    constexpr wizz::UtcInstant instant = wizz::make_utc(d(2026, 11, 2), 10, 0, 5);
    EXPECT_EQ(instant, std::chrono::sys_days{std::chrono::year{2026} / 11 / 2} + 10h + 5s);
    EXPECT_EQ(wizz::format_iso_utc(instant), "2026-11-02T10:00:05Z");
}

// ---------------------------------------------------------------------------
// parse_local_datetime: the 02 §2.4 table (route-map operationStartDate)

TEST(DateTimeParse, Route_map_table_02_2_4)
{
    EXPECT_EQ(parse_local_datetime("2026-11-02T06:00:00"), make_local(d(2026, 11, 2), 6, 0));
    EXPECT_EQ(parse_local_datetime("2026-11-02"), make_local(d(2026, 11, 2), 0, 0));
    EXPECT_EQ(parse_local_datetime("2026-11-02 06:00"), make_local(d(2026, 11, 2), 6, 0));
    EXPECT_EQ(parse_local_datetime("2026-11-02T06:00:00.123"), make_local(d(2026, 11, 2), 6, 0) + 123ms);
    EXPECT_EQ(parse_local_datetime("11/02/2026"), make_local(d(2026, 11, 2), 0, 0));
    // D-22: Z and offsets keep the wall clock (the .NET predecessor converted to local time).
    EXPECT_EQ(parse_local_datetime("2026-11-02T06:00:00Z"), make_local(d(2026, 11, 2), 6, 0));
    EXPECT_EQ(parse_local_datetime("2026-11-02T06:00:00+02:00"), make_local(d(2026, 11, 2), 6, 0));
    EXPECT_EQ(parse_local_datetime("garbage"), std::nullopt);
}

TEST(DateTimeParse, Suspended_route_marker_1900_parses_normally)
{
    // 02 §2.4: Wizz writes 1900-01-01T00:00:00 for routes with nothing on sale.
    EXPECT_EQ(parse_local_datetime("1900-01-01T00:00:00"), make_local(d(1900, 1, 1), 0, 0));
}

// ---------------------------------------------------------------------------
// parse_local_datetime: the 04 §9.4 table (timetable TryParseLocal)

TEST(DateTimeParse, Timetable_table_04_9_4_accepted_forms)
{
    const LocalDateTime at_0830 = make_local(d(2026, 11, 15), 8, 30);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30:00"), at_0830);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30:00.123"), at_0830 + 123ms);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30"), at_0830);
    EXPECT_EQ(parse_local_datetime("2026-11-15 08:30:00"), at_0830);
    EXPECT_EQ(parse_local_datetime("2026-11-15"), make_local(d(2026, 11, 15), 0, 0));
    EXPECT_EQ(parse_local_datetime(" 2026-11-15T08:30:00 "), at_0830);
    EXPECT_EQ(parse_local_datetime("11/15/2026"), make_local(d(2026, 11, 15), 0, 0));
}

TEST(DateTimeParse, Timetable_table_04_9_4_rejected_forms)
{
    EXPECT_EQ(parse_local_datetime("15/11/2026"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15T24:00:00"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-02-30T00:00:00"), std::nullopt);
    EXPECT_EQ(parse_local_datetime(""), std::nullopt);
    EXPECT_EQ(parse_local_datetime("garbage"), std::nullopt);
}

TEST(DateTimeParse, Timetable_offsets_keep_the_wall_clock_D_22)
{
    const LocalDateTime at_0800 = make_local(d(2026, 11, 15), 8, 0);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00Z"), at_0800);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00+02:00"), at_0800);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00-05:30"), at_0800);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00+0200"), at_0800);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00+02"), at_0800);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00.5z"), at_0800 + 500ms);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00+15:00"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00+02:60"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00+02:"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:00:00+"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15Z"), std::nullopt);
}

TEST(DateTimeParse, Fractions_carry_up_to_seven_digits_of_100ns_ticks)
{
    const LocalDateTime base = make_local(d(2026, 11, 15), 8, 30);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30:00.1"), base + 100ms);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30:00.1234567"), base + Ticks{1'234'567});
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30:00.0000001"), base + Ticks{1});
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30:00.12345678"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30:00."), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30.5"), std::nullopt);
}

TEST(DateTimeParse, Surrounding_ascii_white_space_is_ignored)
{
    EXPECT_EQ(parse_local_datetime("\t2026-11-15T08:30:00\r\n"), make_local(d(2026, 11, 15), 8, 30));
    EXPECT_EQ(parse_local_datetime("  11/15/2026  "), make_local(d(2026, 11, 15), 0, 0));
    EXPECT_EQ(parse_local_datetime("   "), std::nullopt);
}

TEST(DateTimeParse, Calendar_and_clock_ranges_are_validated)
{
    EXPECT_EQ(parse_local_datetime("2024-02-29"), make_local(d(2024, 2, 29), 0, 0));
    EXPECT_EQ(parse_local_datetime("2026-02-29"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-13-01"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-00-10"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-00"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("0000-01-01"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15T23:59:59"), make_local(d(2026, 11, 15), 23, 59, 59));
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:60"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("2026-11-15T08:30:60"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("02/29/2026"), std::nullopt);
    EXPECT_EQ(parse_local_datetime("13/01/2026"), std::nullopt);
}

TEST(DateTimeParse, Other_shapes_are_unparsable)
{
    for (const char* text : {"2026-11-15T", "2026-11-15T08", "2026-11-15T8:30", "2026-1-15", "26-11-15",
                             "2026/11/15", "2026-11-15  08:30", "2026-11-15t08:30", "2026-11-15T08:30:00 x",
                             "1/2/2026", "11/15/2026 08:30", "+2026-11-15", "2026-11-15T08:30:00:00",
                             "２０２６-11-15"}) {
        EXPECT_EQ(parse_local_datetime(text), std::nullopt) << text;
    }
}

// ---------------------------------------------------------------------------
// Invariant text forms (07 §1, 01 §3 errors.log "s", summary.json startedAt)

TEST(DateTimeFormat, Hhmm_is_invariant_and_zero_padded)
{
    EXPECT_EQ(wizz::format_hhmm(make_local(d(2026, 11, 15), 6, 5)), "06:05");
    EXPECT_EQ(wizz::format_hhmm(make_local(d(2026, 11, 15), 17, 30, 59) + 999ms), "17:30");
    EXPECT_EQ(wizz::format_hhmm(make_local(d(2026, 11, 15), 0, 0)), "00:00");
}

TEST(DateTimeFormat, Iso_seconds_matches_dotnet_s_format)
{
    EXPECT_EQ(wizz::format_iso_seconds(make_local(d(2026, 11, 2), 14, 46, 14) + Ticks{9'999'999}),
              "2026-11-02T14:46:14");
    EXPECT_EQ(wizz::format_iso_seconds(make_local(d(1900, 1, 1), 0, 0)), "1900-01-01T00:00:00");
}

TEST(DateTimeFormat, Iso_utc_adds_a_fraction_only_when_present)
{
    EXPECT_EQ(wizz::format_iso_utc(wizz::FixedClock::kDefaultUtcNow), "2026-11-02T10:00:00Z");
    EXPECT_EQ(wizz::format_iso_utc(wizz::make_utc(d(2026, 11, 2), 10, 0, 5) + Ticks{1'234'567}),
              "2026-11-02T10:00:05.1234567Z");
    EXPECT_EQ(wizz::format_iso_utc(wizz::make_utc(d(2026, 11, 2), 10, 0) + 500ms), "2026-11-02T10:00:00.5000000Z");
}

TEST(DateTimeFormat, Parse_and_format_round_trip)
{
    const auto parsed = parse_local_datetime("2026-12-01T21:05:00");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(wizz::format_iso_seconds(*parsed), "2026-12-01T21:05:00");
    EXPECT_EQ(wizz::format_hhmm(*parsed), "21:05");
}

// ---------------------------------------------------------------------------
// DateChunker (03 §2, 04 §9.1)

using Chunks = std::vector<std::pair<Date, Date>>;

TEST(DateChunker, CT_CHK_01_Splits_into_42_day_inclusive_chunks)
{
    const Chunks expected{
        {d(2026, 11, 10), d(2026, 12, 21)},
        {d(2026, 12, 22), d(2027, 2, 1)},
        {d(2027, 2, 2), d(2027, 2, 7)},
    };
    EXPECT_EQ(wizz::date_chunker::split(d(2026, 11, 10), d(2027, 2, 7), 42), expected);
}

TEST(DateChunker, CT_CHK_02_Single_day_and_empty_ranges)
{
    const Chunks single{{d(2026, 12, 1), d(2026, 12, 1)}};
    EXPECT_EQ(wizz::date_chunker::split(d(2026, 12, 1), d(2026, 12, 1), 42), single);
    EXPECT_TRUE(wizz::date_chunker::split(d(2026, 12, 1), d(2026, 11, 30), 42).empty());
}

TEST(DateChunker, CT_CHK_02_Max_days_below_one_is_an_argument_error)
{
    EXPECT_THROW((void)wizz::date_chunker::split(d(2026, 12, 1), d(2026, 12, 5), 0), std::invalid_argument);
    EXPECT_THROW((void)wizz::date_chunker::split(d(2026, 12, 1), d(2026, 12, 5), -1), std::invalid_argument);
    EXPECT_THROW((void)wizz::date_chunker::split(d(2026, 12, 5), d(2026, 12, 1), 0), std::invalid_argument);
}

TEST(DateChunker, Example_04_9_1_ninety_one_days_from_today)
{
    // 2026-11-02..2027-01-31 gives 42 + 42 + 7 days (CT-API-10 [D] windows).
    const Chunks expected{
        {d(2026, 11, 2), d(2026, 12, 13)},
        {d(2026, 12, 14), d(2027, 1, 24)},
        {d(2027, 1, 25), d(2027, 1, 31)},
    };
    const Chunks chunks = wizz::date_chunker::split(d(2026, 11, 2), d(2027, 1, 31), 42);
    EXPECT_EQ(chunks, expected);
    for (const auto& [from, to] : chunks) {
        EXPECT_LE(days_between(to, from), 41);
    }
}

TEST(DateChunker, Exact_multiples_and_one_day_chunks)
{
    const Chunks two{{d(2026, 11, 1), d(2026, 12, 12)}, {d(2026, 12, 13), d(2027, 1, 23)}};
    EXPECT_EQ(wizz::date_chunker::split(d(2026, 11, 1), d(2027, 1, 23), 42), two);

    const Chunks days{{d(2026, 12, 30), d(2026, 12, 30)},
                      {d(2026, 12, 31), d(2026, 12, 31)},
                      {d(2027, 1, 1), d(2027, 1, 1)}};
    EXPECT_EQ(wizz::date_chunker::split(d(2026, 12, 30), d(2027, 1, 1), 1), days);
}

TEST(DateChunker, Huge_max_days_gives_one_chunk_without_overflow)
{
    const Chunks one{{d(2026, 11, 2), d(9999, 12, 31)}};
    EXPECT_EQ(wizz::date_chunker::split(d(2026, 11, 2), d(9999, 12, 31), INT_MAX), one);
}

// ---------------------------------------------------------------------------
// Clocks (05 §1.1)

TEST(FixedClock, Defaults_to_2026_11_02_10_00_utc_with_a_utc_local_zone)
{
    const wizz::FixedClock clock;
    EXPECT_EQ(clock.utc_now(), wizz::make_utc(d(2026, 11, 2), 10, 0));
    EXPECT_EQ(clock.local_now(), make_local(d(2026, 11, 2), 10, 0));
    EXPECT_EQ(clock.local_today(), d(2026, 11, 2));
    EXPECT_EQ(clock.local_offset(), Ticks::zero());
}

TEST(FixedClock, Never_advances_unless_set)
{
    const wizz::FixedClock clock;
    const auto utc = clock.utc_now();
    const auto monotonic = clock.monotonic_now();
    std::this_thread::sleep_for(2ms);
    EXPECT_EQ(clock.utc_now(), utc);
    EXPECT_EQ(clock.monotonic_now(), monotonic);
}

TEST(FixedClock, Setters_move_utc_local_and_monotonic_time)
{
    wizz::FixedClock clock;
    clock.advance(29min);
    EXPECT_EQ(clock.utc_now(), wizz::make_utc(d(2026, 11, 2), 10, 29));
    EXPECT_EQ(clock.monotonic_now(), Ticks{29min});

    clock.set_utc_now(wizz::make_utc(d(2026, 11, 2), 23, 30));
    EXPECT_EQ(clock.monotonic_now(), Ticks{29min});  // a wall-clock jump is not elapsed time
    EXPECT_EQ(clock.local_today(), d(2026, 11, 2));

    clock.set_local_offset(4h);  // e.g. the predecessor's UTC+4 machine
    EXPECT_EQ(clock.local_now(), make_local(d(2026, 11, 3), 3, 30));
    EXPECT_EQ(clock.local_today(), d(2026, 11, 3));

    clock.set_local_offset(-11h);
    EXPECT_EQ(clock.local_today(), d(2026, 11, 2));
    EXPECT_EQ(clock.utc_now(), wizz::make_utc(d(2026, 11, 2), 23, 30));
}

TEST(FixedClock, Constructor_takes_an_instant_and_an_offset)
{
    const wizz::FixedClock clock{wizz::make_utc(d(2026, 12, 31), 22, 0), 2h};
    EXPECT_EQ(clock.local_today(), d(2027, 1, 1));
}

TEST(FixedClock, Concurrent_advances_are_not_lost)
{
    wizz::FixedClock clock;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&clock] {
                for (int i = 0; i < 1000; ++i) {
                    clock.advance(1s);
                }
            });
        }
    }
    EXPECT_EQ(clock.monotonic_now(), Ticks{8000s});
    EXPECT_EQ(clock.utc_now(), wizz::FixedClock::kDefaultUtcNow + 8000s);
}

TEST(SystemClock, Utc_now_follows_the_system_clock)
{
    const wizz::SystemClock clock;
    const auto before = std::chrono::system_clock::now();
    const auto now = clock.utc_now();
    const auto after = std::chrono::system_clock::now();
    EXPECT_GE(now, std::chrono::time_point_cast<Ticks>(before));
    EXPECT_LE(now, std::chrono::time_point_cast<Ticks>(after));
}

TEST(SystemClock, Local_now_is_utc_shifted_by_the_machine_zone_offset)
{
    const wizz::SystemClock clock;
    // The two readings are taken one after the other; the time between them (measured
    // around them, so a busy machine cannot make it exceed the bound) is the only slack.
    const auto before = std::chrono::steady_clock::now();
    const auto utc = clock.utc_now();
    const auto local = clock.local_now();
    const auto between = std::chrono::steady_clock::now() - before;
    const auto offset = local.time_since_epoch() - utc.time_since_epoch();
    EXPECT_LE(offset, Ticks{14h} + std::chrono::ceil<Ticks>(between));
    EXPECT_GE(offset, -Ticks{14h});
    const auto expected = std::chrono::current_zone()->get_info(utc).offset;
    EXPECT_LE(offset - expected, std::chrono::ceil<Ticks>(between) + Ticks{1ms});
    EXPECT_GE(offset - expected, Ticks::zero());
}

TEST(SystemClock, Monotonic_time_never_goes_back)
{
    const wizz::SystemClock clock;
    const auto first = clock.monotonic_now();
    std::this_thread::sleep_for(2ms);
    const auto second = clock.monotonic_now();
    EXPECT_GE(second - first, Ticks{1ms});
}

} // namespace
