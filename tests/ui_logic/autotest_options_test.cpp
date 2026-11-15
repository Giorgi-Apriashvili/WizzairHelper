// The --autotest command line (01 §17.1) and the D-30 companions
// (--data-root, --replay, --fixed-now, --no-pacing, --warm-cache,
// --cancel-after-ms, --diagnostics).

#include "wizz/ui/autotest_options.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/errors.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace wizz;
using namespace wizz::ui;
namespace fs = std::filesystem;

const fs::path kCurrentDir{L"C:\\work\\run"};

[[nodiscard]] std::optional<AutoTestOptions> parse(std::vector<std::string> args)
{
    return AutoTestOptions::parse(args, kCurrentDir);
}

[[nodiscard]] constexpr Date d(unsigned month, unsigned day) noexcept
{
    return Date::from_ymd(2026, month, day);
}

TEST(AutoTestOptions, Without_the_flag_or_its_value_the_app_runs_normally)
{
    EXPECT_FALSE(parse({}));
    EXPECT_FALSE(parse({"--from", "Wien"}));
    EXPECT_FALSE(parse({"--autotest"}));
    EXPECT_FALSE(parse({"--from", "Wien", "--autotest"}));
    EXPECT_FALSE(parse({"--AUTOTEST", "out"}));  // exact, case-sensitive
}

TEST(AutoTestOptions, Defaults)
{
    const std::optional<AutoTestOptions> options = parse({"--autotest", "out"});
    ASSERT_TRUE(options);
    EXPECT_EQ(options->output_dir, fs::path{L"C:\\work\\run\\out"});
    EXPECT_EQ(options->from, "Budapest");
    EXPECT_EQ(options->to, "London");
    EXPECT_FALSE(options->from_radius_km);
    EXPECT_FALSE(options->to_radius_km);
    EXPECT_FALSE(options->outbound);
    EXPECT_FALSE(options->inbound);
    EXPECT_FALSE(options->currency);
    EXPECT_FALSE(options->then_currency);
    EXPECT_FALSE(options->return_from_top);
    EXPECT_FALSE(options->keep_open);
    EXPECT_FALSE(options->data_root);
    EXPECT_FALSE(options->replay_dir);
    EXPECT_FALSE(options->fixed_now);
    EXPECT_FALSE(options->no_pacing);
    EXPECT_FALSE(options->warm_cache);
    EXPECT_FALSE(options->cancel_after);
    EXPECT_FALSE(options->diagnostics);
    EXPECT_FALSE(options->runs_return());
}

TEST(AutoTestOptions, Every_documented_flag)
{
    const std::optional<AutoTestOptions> options =
        parse({"--autotest", "C:\\results\\run1", "--from", "Wien", "--to", "", "--from-radius", "300", "--to-radius",
               "50", "--out", "2026-11-10:2026-11-20", "--ret", "2026-11-25", "--currency", "GBP", "--then-currency",
               "HUF", "--return-from-top", "--keep-open"});
    ASSERT_TRUE(options);
    EXPECT_EQ(options->output_dir, fs::path{L"C:\\results\\run1"});
    EXPECT_EQ(options->from, "Wien");
    EXPECT_EQ(options->to, "");  // "--to \"\"" = anywhere, the empty value is kept
    EXPECT_EQ(options->from_radius_km, 300);
    EXPECT_EQ(options->to_radius_km, 50);
    EXPECT_EQ(options->outbound, (AutoTestDates{d(11, 10), d(11, 20)}));
    EXPECT_EQ(options->inbound, (AutoTestDates{d(11, 25), d(11, 25)}));  // one date: start = end
    EXPECT_EQ(options->currency, "GBP");
    EXPECT_EQ(options->then_currency, "HUF");
    EXPECT_TRUE(options->return_from_top);
    EXPECT_TRUE(options->keep_open);
    EXPECT_TRUE(options->runs_return());
}

TEST(AutoTestOptions, First_occurrence_wins_and_the_value_is_the_next_argument)
{
    const std::optional<AutoTestOptions> options =
        parse({"--autotest", "a", "--from", "First", "--from", "Second", "--to", "--keep-open", "--autotest", "b"});
    ASSERT_TRUE(options);
    EXPECT_EQ(options->output_dir, fs::path{L"C:\\work\\run\\a"});
    EXPECT_EQ(options->from, "First");
    EXPECT_EQ(options->to, "--keep-open");  // whatever follows the flag
    EXPECT_TRUE(options->keep_open);        // …and the flag itself is still present
    EXPECT_EQ(parse({"--autotest", "a", "--FROM", "X"})->from, "Budapest");
    EXPECT_EQ(parse({"--autotest", "a", "--from"})->from, "Budapest");  // no value after it
}

TEST(AutoTestOptions, Output_directory_is_made_absolute_and_normalised)
{
    EXPECT_EQ(parse({"--autotest", "..\\x"})->output_dir, fs::path{L"C:\\work\\x"});
    EXPECT_EQ(parse({"--autotest", ".\\out\\.\\shots"})->output_dir, fs::path{L"C:\\work\\run\\out\\shots"});
    EXPECT_EQ(parse({"--autotest", "\\top"})->output_dir, fs::path{L"C:\\top"});
    EXPECT_EQ(parse({"--autotest", "Ergebnisse-\u00FC"})->output_dir, fs::path{L"C:\\work\\run\\Ergebnisse-\u00FC"});
    EXPECT_THROW((void)parse({"--autotest", ""}), std::invalid_argument);  // Path.GetFullPath("")
}

TEST(AutoTestOptions, Radii_parse_like_int_TryParse_and_are_clamped)
{
    const auto radius = [](std::string text) { return parse({"--autotest", "o", "--from-radius", text})->from_radius_km; };
    EXPECT_EQ(radius("75"), 75);
    EXPECT_EQ(radius(" 75 "), 75);
    EXPECT_EQ(radius("+20"), 20);
    EXPECT_EQ(radius("-5"), 0);
    EXPECT_EQ(radius("2000"), 1500);
    EXPECT_EQ(radius("0"), 0);
    EXPECT_EQ(radius("1500"), 1500);
    EXPECT_FALSE(radius("abc"));  // unparsable: ignored (the settings value stays)
    EXPECT_FALSE(radius("1.5"));
    EXPECT_FALSE(radius("2,000"));
    EXPECT_FALSE(radius(""));
    EXPECT_FALSE(radius("99999999999"));
    EXPECT_EQ(parse({"--autotest", "o", "--to-radius", "abc"})->to_radius_km, std::nullopt);
}

TEST(AutoTestOptions, Culture_int_parsing)
{
    const NumberSymbols en;
    EXPECT_EQ(parse_culture_int("42", en), 42);
    EXPECT_EQ(parse_culture_int("\t 42 \r\n", en), 42);
    EXPECT_EQ(parse_culture_int("-2147483648", en), -2147483647 - 1);
    EXPECT_EQ(parse_culture_int("2147483647", en), 2147483647);
    EXPECT_FALSE(parse_culture_int("2147483648", en));
    EXPECT_EQ(parse_culture_int("0042", en), 42);
    EXPECT_EQ(parse_culture_int(std::string_view{"7\0\0", 3}, en), 7);  // trailing NULs
    EXPECT_FALSE(parse_culture_int("- 4", en));
    EXPECT_FALSE(parse_culture_int("4-", en));
    EXPECT_FALSE(parse_culture_int("\u0664", en));  // Arabic-Indic digit: ASCII digits only
    EXPECT_FALSE(parse_culture_int("+", en));

    NumberSymbols minus;
    minus.negative_sign = "\u2212";  // e.g. sv-SE
    EXPECT_EQ(parse_culture_int("\u221210", minus), -10);
    EXPECT_EQ(parse_culture_int("-10", minus), -10);  // AllowHyphenDuringParsing
    EXPECT_EQ(AutoTestOptions::parse(std::vector<std::string>{"--autotest", "o", "--to-radius", "\u2212" "10"},
                                     kCurrentDir, minus)
                  ->to_radius_km,
              0);
}

TEST(AutoTestOptions, Dates_take_the_first_and_the_last_part)
{
    EXPECT_EQ(parse_autotest_dates("2026-11-10:2026-11-12:2026-11-20"), (AutoTestDates{d(11, 10), d(11, 20)}));
    EXPECT_EQ(parse_autotest_dates("2026-12-01"), (AutoTestDates{d(12, 1), d(12, 1)}));
    // no ordering check here: the date rules of 01 §6.1 sort it out
    EXPECT_EQ(parse_autotest_dates("2026-12-01:2026-11-01"), (AutoTestDates{d(12, 1), d(11, 1)}));
}

TEST(AutoTestOptions, Bad_dates_throw_a_format_error)
{
    const char* bad[] = {"2026-13-01", "", "2026-1-5", "2026-11-10:", ":2026-11-10", "2026-02-30", " 2026-11-10",
                         "2026/11/10", "20261110"};
    for (const char* value : bad) {
        try {
            (void)parse({"--autotest", "o", "--out", value});
            ADD_FAILURE() << "no exception for " << value;
        } catch (const FormatError& error) {
            EXPECT_NE(std::string{error.what()}.find("was not recognized as a valid DateOnly"), std::string::npos);
        }
    }
    EXPECT_THROW((void)parse({"--autotest", "o", "--ret", "tomorrow"}), FormatError);
    EXPECT_NO_THROW((void)parse({"--autotest", "o", "--ret"}));  // no value: ignored
}

TEST(AutoTestOptions, Currencies_apply_only_when_listed)
{
    std::optional<AutoTestOptions> options = parse({"--autotest", "o", "--currency", "eur", "--then-currency", "XYZ"});
    EXPECT_EQ(options->currency, "eur");  // kept as given
    EXPECT_FALSE(options->listed_currency());
    EXPECT_FALSE(options->listed_then_currency());
    options = parse({"--autotest", "o", "--currency", "ILS", "--then-currency", "HUF"});
    EXPECT_EQ(options->listed_currency(), "ILS");
    EXPECT_EQ(options->listed_then_currency(), "HUF");
}

TEST(AutoTestOptions, D_30_Data_root_replay_and_fixed_now)
{
    const std::optional<AutoTestOptions> options =
        parse({"--autotest", "o", "--data-root", "profile", "--replay", "D:\\fixtures\\replay", "--fixed-now",
               "2026-11-02T10:00:00Z"});
    ASSERT_TRUE(options);
    EXPECT_EQ(options->data_root, fs::path{L"C:\\work\\run\\profile"});
    EXPECT_EQ(options->replay_dir, fs::path{L"D:\\fixtures\\replay"});
    EXPECT_EQ(options->fixed_now, FixedClock::kDefaultUtcNow);
    EXPECT_FALSE(parse({"--autotest", "o", "--replay"})->replay_dir);
    EXPECT_THROW((void)parse({"--autotest", "o", "--data-root", ""}), std::invalid_argument);
}

TEST(AutoTestOptions, D_30_Fixed_now_formats)
{
    EXPECT_EQ(parse_fixed_now("2026-11-02T10:00:00"), FixedClock::kDefaultUtcNow);
    EXPECT_EQ(parse_fixed_now("2026-11-02T10:00:00.5Z"), FixedClock::kDefaultUtcNow + Ticks{5'000'000});
    EXPECT_EQ(parse_fixed_now("2026-11-02T23:59:59.1234567Z"),
              make_utc(Date::from_ymd(2026, 11, 2), 23, 59, 59) + Ticks{1'234'567});
    for (const char* bad : {"2026-11-02", "2026-11-02 10:00:00", "2026-11-02T24:00:00", "2026-11-02T10:00:00+02:00",
                            "2026-11-02T10:00:00.", "2026-11-02T10:00:00.12345678", "x"}) {
        EXPECT_THROW((void)parse_fixed_now(bad), FormatError) << bad;
    }
    EXPECT_THROW((void)parse({"--autotest", "o", "--fixed-now", "soon"}), FormatError);
}

TEST(AutoTestOptions, D_30_Acceptance_companions)
{
    using std::chrono::milliseconds;
    const std::optional<AutoTestOptions> options =
        parse({"--autotest", "o", "--no-pacing", "--warm-cache", "--cancel-after-ms", "1500", "--diagnostics"});
    ASSERT_TRUE(options);
    EXPECT_TRUE(options->no_pacing);
    EXPECT_TRUE(options->warm_cache);
    EXPECT_EQ(options->cancel_after, milliseconds{1500});
    EXPECT_TRUE(options->diagnostics);
    EXPECT_EQ(parse({"--autotest", "o", "--cancel-after-ms", "0"})->cancel_after, milliseconds{0});
    EXPECT_FALSE(parse({"--autotest", "o", "--cancel-after-ms"})->cancel_after) << "no value: not given";
    EXPECT_FALSE(parse({"--autotest", "o", "--Diagnostics", "--no-Pacing"})->diagnostics) << "exact, case-sensitive";
    // Only together with --autotest.
    EXPECT_FALSE(parse({"--warm-cache", "--diagnostics", "--cancel-after-ms", "5"}));
}

TEST(AutoTestOptions, D_30_Cancel_after_ms_values)
{
    using std::chrono::milliseconds;
    EXPECT_EQ(parse_cancel_after("0"), milliseconds{0});
    EXPECT_EQ(parse_cancel_after("007"), milliseconds{7});
    EXPECT_EQ(parse_cancel_after("999999999"), milliseconds{999'999'999});
    for (const char* bad : {"", "-1", "+5", " 5", "5 ", "1.5", "1e3", "1000000000", "Ù¡"}) {
        EXPECT_THROW((void)parse_cancel_after(bad), FormatError) << bad;
    }
    EXPECT_THROW((void)parse({"--autotest", "o", "--cancel-after-ms", "soon"}), FormatError);
}

} // namespace
