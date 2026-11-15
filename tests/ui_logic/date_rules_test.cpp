// Date and nights coupling (01 §4.4 defaults, §6.1 WireDateRules, §6.2, §11.6,
// §17.2 order; 05 §7.2 U-03). TODAY = 2026-11-02 (a Monday).

#include "wizz/ui/date_rules.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

namespace {

using wizz::Date;
using namespace wizz::ui;

[[nodiscard]] constexpr Date d(int year, unsigned month, unsigned day) noexcept
{
    return Date::from_ymd(year, month, day);
}

[[nodiscard]] constexpr Date nov(unsigned day) noexcept
{
    return d(2026, 11, day);
}

[[nodiscard]] constexpr Date dec(unsigned day) noexcept
{
    return d(2026, 12, day);
}

constexpr Date kToday = nov(2);

void expect_window(const DateRules& rules, Date out_from, Date out_to, Date ret_from, Date ret_to)
{
    EXPECT_EQ(rules.out_from(), out_from);
    EXPECT_EQ(rules.out_to(), out_to);
    EXPECT_EQ(rules.ret_from(), ret_from);
    EXPECT_EQ(rules.ret_to(), ret_to);
}

TEST(DateRules, Defaults_and_range)
{
    const DateRules rules = DateRules::for_today(kToday);
    expect_window(rules, nov(9), dec(9), nov(12), dec(16));  // +7, +37, +10, +44
    EXPECT_EQ(rules.bounds().min, kToday);
    EXPECT_EQ(rules.bounds().max, d(2028, 11, 2));  // AddYears(2)
    EXPECT_EQ(rules.min_nights(), 2);
    EXPECT_EQ(rules.max_nights(), 10);
    EXPECT_EQ(rules.last_out_from(), nov(9));
    EXPECT_FALSE(rules.adjusting());

    const DateRules clamped = DateRules::for_today(kToday, 70, 5);  // clamp(70, 0, 60), max(clamp(5), 60)
    EXPECT_EQ(clamped.min_nights(), 60);
    EXPECT_EQ(clamped.max_nights(), 60);
    const DateRules negative = DateRules::for_today(kToday, -3, 500);
    EXPECT_EQ(negative.min_nights(), 0);
    EXPECT_EQ(negative.max_nights(), 120);
}

TEST(DateRules, Add_years_like_DateTime_AddYears)
{
    EXPECT_EQ(add_years(kToday, 2), d(2028, 11, 2));
    EXPECT_EQ(add_years(d(2028, 2, 29), 2), d(2030, 2, 28));
    EXPECT_EQ(add_years(d(2028, 2, 29), 4), d(2032, 2, 29));
}

TEST(DateRules, U_03_Moving_the_outbound_start_moves_the_return_window)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.set_out_from(nov(12));  // +3 days
    expect_window(rules, nov(12), dec(9), nov(15), dec(19));  // +3, length 34 kept
    EXPECT_EQ(days_between(rules.ret_to(), rules.ret_from()), 34);
    rules.set_out_from(nov(5));  // −7 days
    expect_window(rules, nov(5), dec(9), nov(8), dec(12));
    EXPECT_LE(rules.out_from(), rules.out_to());
    EXPECT_LE(rules.ret_from(), rules.ret_to());
}

TEST(DateRules, U_03_Clamping_can_shorten_the_return_window)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.set_out_from(d(2028, 10, 30));  // outTo is pushed along; the return window hits MaxDate
    EXPECT_EQ(rules.out_from(), d(2028, 10, 30));
    EXPECT_EQ(rules.out_to(), d(2028, 10, 30));
    EXPECT_EQ(rules.ret_from(), d(2028, 11, 2));
    EXPECT_EQ(rules.ret_to(), d(2028, 11, 2));

    DateRules low = DateRules::for_today(kToday);
    low.set_ret_from(nov(3));  // return window [11-03, 12-16]
    low.set_out_from(nov(2));  // −7: new start clamped to MinDate, length kept from there
    expect_window(low, nov(2), dec(9), nov(2), dec(15));
}

TEST(DateRules, Programmatic_values_are_clamped_to_the_picker_range)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.set_ret_to(d(2030, 1, 1));
    EXPECT_EQ(rules.ret_to(), d(2028, 11, 2));
    rules.set_out_to(d(2020, 1, 1));  // clamped to today; outFrom pulled back to it
    EXPECT_EQ(rules.out_to(), kToday);
    EXPECT_EQ(rules.out_from(), kToday);
}

TEST(DateRules, LIVE_4_A_later_outbound_end_extends_the_return_end)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.set_out_to(dec(20));  // retTo ≥ outTo + minNights (2)
    expect_window(rules, nov(9), dec(20), nov(12), dec(22));
    rules.set_out_to(dec(10));  // earlier end: nothing shrinks
    expect_window(rules, nov(9), dec(10), nov(12), dec(22));
}

TEST(DateRules, Outbound_end_before_start_pulls_the_start_without_shifting_the_return)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.set_out_to(nov(5));  // < outFrom 11-09
    expect_window(rules, nov(5), nov(5), nov(12), dec(16));  // nested outFrom handler is guarded
    EXPECT_EQ(rules.last_out_from(), nov(5));  // …but _lastOutFrom is always updated
    rules.set_out_from(nov(6));  // so the next move counts from 11-05: +1
    expect_window(rules, nov(6), nov(6), nov(13), dec(17));
}

TEST(DateRules, Pushing_the_outbound_end_from_the_start_skips_the_min_nights_rule)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.set_ret_from(nov(2));
    rules.set_ret_to(nov(2));  // return window [11-02, 11-02]
    expect_window(rules, nov(9), dec(9), nov(2), nov(2));
    rules.set_out_from(dec(12));  // outTo = outFrom (guarded handler), then shift +33
    expect_window(rules, dec(12), dec(12), dec(5), dec(5));
    EXPECT_LT(rules.ret_to(), rules.out_to().add_days(rules.min_nights()));  // the outTo rule did not run
}

TEST(DateRules, Return_handlers_keep_from_before_to)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.set_ret_from(dec(20));  // after retTo 12-16
    EXPECT_EQ(rules.ret_to(), dec(20));
    rules.set_ret_to(dec(1));  // before retFrom 12-20
    EXPECT_EQ(rules.ret_from(), dec(1));
    expect_window(rules, nov(9), dec(9), dec(1), dec(1));
}

TEST(DateRules, Same_value_raises_no_change)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.set_out_from(nov(9));
    expect_window(rules, nov(9), dec(9), nov(12), dec(16));
    EXPECT_EQ(rules.set_min_nights(2), 0);
}

TEST(DateRules, Nights_coupling_moves_no_dates)
{
    DateRules rules = DateRules::for_today(kToday);
    EXPECT_EQ(rules.set_min_nights(5), 1);  // one RecomputeTrips
    EXPECT_EQ(rules.max_nights(), 10);
    EXPECT_EQ(rules.set_min_nights(15), 2);  // max follows: cascade, RecomputeTrips twice
    EXPECT_EQ(rules.max_nights(), 15);
    EXPECT_EQ(rules.set_max_nights(3), 2);  // min follows down
    EXPECT_EQ(rules.min_nights(), 3);
    EXPECT_EQ(rules.set_max_nights(100), 1);
    EXPECT_EQ(rules.min_nights(), 3);
    EXPECT_EQ(rules.set_min_nights(99), 1);  // clamped to 60
    EXPECT_EQ(rules.min_nights(), 60);
    EXPECT_EQ(rules.set_max_nights(500), 1);  // clamped to 120
    EXPECT_EQ(rules.max_nights(), 120);
    expect_window(rules, nov(9), dec(9), nov(12), dec(16));
}

TEST(DateRules, Min_nights_feed_the_outbound_end_rule)
{
    DateRules rules = DateRules::for_today(kToday);
    (void)rules.set_min_nights(10);
    rules.set_out_to(dec(10));  // retTo ≥ 12-20
    expect_window(rules, nov(9), dec(10), nov(12), dec(20));
}

TEST(DateRules, UX_9_Right_click_return_window_is_outbound_date_plus_nights)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.plan_return_window(nov(20));
    expect_window(rules, nov(9), dec(9), nov(22), nov(30));  // 11-20 + [2, 10]
    EXPECT_FALSE(rules.adjusting());

    rules.plan_return_window(d(2027, 3, 1));  // later than the current window: widened first
    EXPECT_EQ(rules.ret_from(), d(2027, 3, 3));
    EXPECT_EQ(rules.ret_to(), d(2027, 3, 11));

    rules.plan_return_window(nov(5));  // earlier than the current window
    EXPECT_EQ(rules.ret_from(), nov(7));
    EXPECT_EQ(rules.ret_to(), nov(15));

    rules.plan_return_window(d(2028, 10, 30));  // clamped at MaxDate
    EXPECT_EQ(rules.ret_from(), d(2028, 11, 1));
    EXPECT_EQ(rules.ret_to(), d(2028, 11, 2));
    expect_window(rules, nov(9), dec(9), d(2028, 11, 1), d(2028, 11, 2));  // outbound untouched
}

TEST(DateRules, Autotest_applies_outTo_outFrom_retTo_retFrom_in_order)
{
    DateRules rules = DateRules::for_today(kToday);
    rules.apply_autotest_dates(DateWindow{dec(1), dec(10)}, DateWindow{dec(5), dec(20)});
    expect_window(rules, dec(1), dec(10), dec(5), dec(20));

    DateRules single = DateRules::for_today(kToday);
    single.apply_autotest_dates(DateWindow{nov(15), nov(15)}, std::nullopt);
    // outTo 11-15 (retTo stays ≥ 11-17), then outFrom 11-15 shifts the return +6
    expect_window(single, nov(15), nov(15), nov(18), dec(22));
}

TEST(DateRules, U_03_From_never_exceeds_to_and_everything_stays_in_range)
{
    DateRules rules = DateRules::for_today(kToday);
    std::mt19937 random{20261102};
    std::uniform_int_distribution<int> field(0, 6);
    std::uniform_int_distribution<int> offset(-30, 800);
    std::uniform_int_distribution<int> nights(-5, 130);
    for (int step = 0; step < 3000; ++step) {
        const Date value = kToday.add_days(offset(random));
        switch (field(random)) {
        case 0:
            rules.set_out_from(value);
            break;
        case 1:
            rules.set_out_to(value);
            break;
        case 2:
            rules.set_ret_from(value);
            break;
        case 3:
            rules.set_ret_to(value);
            break;
        case 4:
            (void)rules.set_min_nights(nights(random));
            break;
        case 5:
            (void)rules.set_max_nights(nights(random));
            break;
        default:
            rules.plan_return_window(value);
            break;
        }
        ASSERT_LE(rules.out_from(), rules.out_to()) << step;
        ASSERT_LE(rules.ret_from(), rules.ret_to()) << step;
        ASSERT_LE(rules.min_nights(), rules.max_nights()) << step;
        for (const Date date : {rules.out_from(), rules.out_to(), rules.ret_from(), rules.ret_to()}) {
            ASSERT_GE(date, rules.bounds().min) << step;
            ASSERT_LE(date, rules.bounds().max) << step;
        }
        ASSERT_EQ(rules.last_out_from(), rules.out_from()) << step;
        ASSERT_FALSE(rules.adjusting());
    }
}

} // namespace
