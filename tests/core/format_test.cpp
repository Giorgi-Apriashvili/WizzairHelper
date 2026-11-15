// Label rounding (07 §2, 03 §0) and duration texts (01 §10.7, §11.10, D-17,
// 05 §7.2 U-04, U-07, U-08).

#include "wizz/core/format.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;
using wizz::format_duration;
using wizz::format_elapsed;
using wizz::format_label;
using wizz::format_remaining;
using wizz::round_half_even;

const wizz::NumberSymbols kEnUs{};
const wizz::NumberSymbols kDeDe{.decimal_separator = ",", .group_separator = ".", .negative_sign = "-",
                                .group_sizes = {3}};

/// Seconds as a tick duration (TimeSpan.FromSeconds for the test values).
[[nodiscard]] wizz::Duration seconds(double s)
{
    return std::chrono::duration_cast<wizz::Duration>(std::chrono::duration<double>{s});
}

// ---------------------------------------------------------------------------
// format_label (07 §2)

TEST(FormatLabel, Table_07_2_zero_and_one_decimal)
{
    struct Row {
        double value;
        const char* zero_decimals;
        const char* one_decimal;
    };
    for (const Row& row : {Row{172.4999999999999, "173", "172.5"}, Row{172.49999999999, "172", "172.5"},
                           Row{0.49999999999999994, "1", "0.5"}, Row{2.5, "3", "2.5"}, Row{4.35, "4", "4.4"},
                           Row{0.25, "0", "0.3"}}) {
        EXPECT_EQ(format_label(row.value, 0, kEnUs), row.zero_decimals) << row.value;
        EXPECT_EQ(format_label(row.value, 1, kEnUs), row.one_decimal) << row.value;
    }
}

TEST(FormatLabel, Probes_03_0_halves_away_from_zero_after_15_digits)
{
    EXPECT_EQ(format_label(2.5, 0, kEnUs), "3");
    EXPECT_EQ(format_label(0.5, 0, kEnUs), "1");
    EXPECT_EQ(format_label(44.5, 0, kEnUs), "45");
    EXPECT_EQ(format_label(172.4999999999999, 0, kEnUs), "173");
    EXPECT_EQ(format_label(0.49999999999999994, 0, kEnUs), "1");
}

TEST(FormatLabel, Busy_panel_countdown_and_slow_step_U_07)
{
    EXPECT_EQ(format_label(4.24, 1, kEnUs), "4.2");  // "⏳ retrying in 4.2 s"
    EXPECT_EQ(format_label(3.6, 0, kEnUs), "4");     // "waiting for server… 4 s"
}

TEST(FormatLabel, Distance_labels_from_the_spec)
{
    EXPECT_EQ(format_label(81.73582262738209, 0, kEnUs), "82");   // P-18 note "London Gatwick (LGW) 82 km"
    EXPECT_EQ(format_label(172.54389037013857, 0, kEnUs), "173");
    EXPECT_EQ(format_label(40.5581, 1, kEnUs), "40.6");
}

TEST(FormatLabel, Binary_values_below_a_half_round_up_after_the_15_digit_step)
{
    // The binary values sit just below the half: printf("%.1f") gives 0.1 / 0.3 / 1.1.
    EXPECT_EQ(format_label(0.15, 1, kEnUs), "0.2");
    EXPECT_EQ(format_label(0.35, 1, kEnUs), "0.4");
    EXPECT_EQ(format_label(1.15, 1, kEnUs), "1.2");
    EXPECT_EQ(format_label(0.05, 1, kEnUs), "0.1");
}

TEST(FormatLabel, Uses_the_culture_decimal_separator_and_no_grouping)
{
    EXPECT_EQ(format_label(172.4999999999999, 1, kDeDe), "172,5");
    EXPECT_EQ(format_label(1234567.891, 0, kEnUs), "1234568");
    EXPECT_EQ(format_label(1234567.891, 1, kEnUs), "1234567.9");
    EXPECT_EQ(format_label(1234567.891, 1, kDeDe), "1234567,9");
}

TEST(FormatLabel, Carries_propagate_into_new_digits)
{
    EXPECT_EQ(format_label(9.5, 0, kEnUs), "10");
    EXPECT_EQ(format_label(99.95, 1, kEnUs), "100.0");
    EXPECT_EQ(format_label(999.5, 0, kEnUs), "1000");
}

TEST(FormatLabel, Zero_tiny_and_huge_values)
{
    EXPECT_EQ(format_label(0.0, 0, kEnUs), "0");
    EXPECT_EQ(format_label(0.0, 1, kEnUs), "0.0");
    EXPECT_EQ(format_label(-0.0, 1, kEnUs), "0.0");
    EXPECT_EQ(format_label(1e-20, 1, kEnUs), "0.0");
    EXPECT_EQ(format_label(0.04, 1, kEnUs), "0.0");
    EXPECT_EQ(format_label(std::numeric_limits<double>::denorm_min(), 0, kEnUs), "0");
    EXPECT_EQ(format_label(1e20, 0, kEnUs), "100000000000000000000");
    // Only 15 significant digits survive, like .NET's custom formats.
    EXPECT_EQ(format_label(1234567890123456789.0, 0, kEnUs), "1234567890123460000");
    EXPECT_EQ(format_label(0.123456789012345678, 15, kEnUs), "0.123456789012346");
}

TEST(FormatLabel, Negative_values_use_the_culture_sign_but_never_print_minus_zero)
{
    EXPECT_EQ(format_label(-2.5, 0, kEnUs), "-3");
    EXPECT_EQ(format_label(-4.35, 1, kEnUs), "-4.4");
    EXPECT_EQ(format_label(-0.4, 0, kEnUs), "0");
    EXPECT_EQ(format_label(-0.04, 1, kEnUs), "0.0");
    const wizz::NumberSymbols minus{.decimal_separator = ".", .group_separator = ",", .negative_sign = "−",
                                    .group_sizes = {3}};
    EXPECT_EQ(format_label(-1.25, 1, minus), "−1.3");
}

TEST(FormatLabel, Non_finite_values)
{
    EXPECT_EQ(format_label(std::numeric_limits<double>::quiet_NaN(), 0, kEnUs), "NaN");
    EXPECT_EQ(format_label(std::numeric_limits<double>::infinity(), 0, kEnUs), "∞");
    EXPECT_EQ(format_label(-std::numeric_limits<double>::infinity(), 1, kEnUs), "-∞");
}

TEST(FormatLabel, Decimals_outside_zero_to_fifteen_are_rejected)
{
    EXPECT_THROW((void)format_label(1.0, -1, kEnUs), std::invalid_argument);
    EXPECT_THROW((void)format_label(1.0, 16, kEnUs), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// round_half_even (07 §2 nearest-even values)

TEST(RoundHalfEven, Ties_go_to_the_even_neighbour)
{
    EXPECT_EQ(round_half_even(0.5), 0.0);
    EXPECT_EQ(round_half_even(1.5), 2.0);
    EXPECT_EQ(round_half_even(2.5), 2.0);
    EXPECT_EQ(round_half_even(3.5), 4.0);
    EXPECT_EQ(round_half_even(-2.5), -2.0);
    EXPECT_EQ(round_half_even(-3.5), -4.0);
    EXPECT_TRUE(std::signbit(round_half_even(-0.5)));
}

TEST(RoundHalfEven, Non_ties_round_to_nearest_on_the_binary_value)
{
    EXPECT_EQ(round_half_even(2.4999999999999996), 2.0);
    EXPECT_EQ(round_half_even(0.49999999999999994), 0.0);  // the label algorithm gives 1 here
    EXPECT_EQ(round_half_even(172.54389037013857), 173.0);
    EXPECT_EQ(round_half_even(-1.6), -2.0);
    EXPECT_EQ(round_half_even(4503599627370497.0), 4503599627370497.0);
    EXPECT_EQ(round_half_even(std::numeric_limits<double>::infinity()), std::numeric_limits<double>::infinity());
    EXPECT_TRUE(std::isnan(round_half_even(std::numeric_limits<double>::quiet_NaN())));
}

TEST(RoundHalfEven, Agrees_with_nearbyint_under_the_default_rounding_mode)
{
    for (int i = -400; i <= 400; ++i) {
        const double value = i * 0.25;
        EXPECT_EQ(round_half_even(value), std::nearbyint(value)) << value;
    }
}

// ---------------------------------------------------------------------------
// format_duration (01 §11.10, D-17, U-04)

TEST(FormatDuration, U_04_examples)
{
    EXPECT_EQ(format_duration(seconds(59.4)), "59 s");
    EXPECT_EQ(format_duration(seconds(0.2)), "1 s");
    EXPECT_EQ(format_duration(75s), "1 min 15 s");
}

TEST(FormatDuration, Spec_01_11_10_examples)
{
    EXPECT_EQ(format_duration(79s), "1 min 19 s");
    EXPECT_EQ(format_duration(11min + 5s), "11 min 5 s");  // seconds are not padded
    EXPECT_EQ(format_duration(seconds(2.5)), "2 s");        // D-17: banker's rounding
    EXPECT_EQ(format_duration(seconds(59.6)), "60 s");
}

TEST(FormatDuration, Request_estimates_n_times_1_3_seconds)
{
    // "{Label}: starting ({N} requests, about {FormatDuration(N×1.3 s)})…" (01 §6)
    EXPECT_EQ(format_duration(seconds(61 * 1.3)), "1 min 19 s");
    EXPECT_EQ(format_duration(seconds(8 * 1.3)), "10 s");
    EXPECT_EQ(format_duration(seconds(500 * 1.3)), "10 min 50 s");
}

TEST(FormatDuration, Edges_below_and_at_one_minute)
{
    EXPECT_EQ(format_duration(0s), "1 s");
    EXPECT_EQ(format_duration(-5s), "1 s");
    EXPECT_EQ(format_duration(seconds(0.5)), "1 s");
    EXPECT_EQ(format_duration(seconds(1.5)), "2 s");
    EXPECT_EQ(format_duration(seconds(3.5)), "4 s");
    EXPECT_EQ(format_duration(seconds(59.5)), "60 s");
    EXPECT_EQ(format_duration(60s), "1 min 0 s");
    EXPECT_EQ(format_duration(seconds(119.99)), "1 min 59 s");
    EXPECT_EQ(format_duration(2h + 3min + 4s), "123 min 4 s");
}

// ---------------------------------------------------------------------------
// format_elapsed (01 §10.7)

TEST(FormatElapsed, Spec_01_10_7_examples)
{
    EXPECT_EQ(format_elapsed(8s), "0:08");
    EXPECT_EQ(format_elapsed(65s), "1:05");
    EXPECT_EQ(format_elapsed(1h + 2min + 5s), "1:02:05");
    EXPECT_EQ(format_elapsed(12s), "0:12");  // display example "0:12  ~25 s left"
}

TEST(FormatElapsed, Components_are_truncated_and_switch_at_one_hour)
{
    EXPECT_EQ(format_elapsed(seconds(8.999)), "0:08");
    EXPECT_EQ(format_elapsed(59min + 59s), "59:59");
    EXPECT_EQ(format_elapsed(seconds(3599.999)), "59:59");
    EXPECT_EQ(format_elapsed(1h), "1:00:00");
    EXPECT_EQ(format_elapsed(23h + 59min + 59s), "23:59:59");
    EXPECT_EQ(format_elapsed(0s), "0:00");
}

TEST(FormatElapsed, Dotnet_timespan_custom_format_quirks)
{
    // "h" is the 0–23 hours component: the day count is dropped.
    EXPECT_EQ(format_elapsed(25h + 1min), "1:01:00");
    // Custom TimeSpan formats print the magnitude; TotalHours < 1 selects "m:ss".
    EXPECT_EQ(format_elapsed(-8s), "0:08");
    EXPECT_EQ(format_elapsed(-(1h + 2min + 5s)), "2:05");
}

// ---------------------------------------------------------------------------
// format_remaining (01 §10.7, U-08)

TEST(FormatRemaining, Spec_01_10_7_examples)
{
    EXPECT_EQ(format_remaining(24.2), "25 s");
    EXPECT_EQ(format_remaining(125.0), "2 min 05 s");
}

TEST(FormatRemaining, U_08_eta_from_stage_progress)
{
    // Stage started 10 s ago, 2 of 10 done since then: 10 / 2 × 8 = 40 s.
    EXPECT_EQ(format_remaining(10.0 / 2 * 8), "40 s");
    EXPECT_EQ(format_remaining(100.0), "1 min 40 s");
}

TEST(FormatRemaining, Edges)
{
    EXPECT_EQ(format_remaining(0.0), "1 s");
    EXPECT_EQ(format_remaining(0.3), "1 s");
    EXPECT_EQ(format_remaining(-3.0), "1 s");
    EXPECT_EQ(format_remaining(1.0), "1 s");
    EXPECT_EQ(format_remaining(1.01), "2 s");
    EXPECT_EQ(format_remaining(59.01), "60 s");
    EXPECT_EQ(format_remaining(60.0), "1 min 00 s");
    EXPECT_EQ(format_remaining(119.9), "1 min 59 s");
    EXPECT_EQ(format_remaining(3600.0), "60 min 00 s");
    EXPECT_EQ(format_remaining(std::numeric_limits<double>::quiet_NaN()), "1 s");
}

} // namespace
