// wizz::Decimal — .NET System.Decimal emulation (03 §6.2, §7.1, 04 §9.4,
// 05 CT-FX-03 / CT-SRCH-07 / P-27, 06 §3). Expected strings are the decimal's
// invariant ToString(), so they assert the value AND the scale. Rows marked
// ".NET" were produced by System.Decimal / System.Text.Json on .NET 10.

#include "wizz/core/decimal.hpp"
#include "wizz/core/locale.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <compare>
#include <cstdint>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {

using wizz::Decimal;
using wizz::DecimalBits;
using wizz::DecimalParseError;
using wizz::NumberSymbols;
using wizz::Rounding;

const NumberSymbols kEnUs{.decimal_separator = ".", .group_separator = ",", .negative_sign = "-", .group_sizes = {3}};
const NumberSymbols kDeDe{.decimal_separator = ",", .group_separator = ".", .negative_sign = "-", .group_sizes = {3}};

constexpr std::string_view kOverflow = "<overflow>";
constexpr std::string_view kDivideByZero = "<divide by zero>";

/// A decimal from a JSON number lexeme; throws (failing the test) when invalid.
Decimal dec(std::string_view lexeme)
{
    const auto value = Decimal::from_json_number(lexeme);
    if (!value) {
        throw std::invalid_argument("not a decimal lexeme: " + std::string{lexeme});
    }
    return *value;
}

/// Invariant text of `compute()`, or the sentinel of the exception it throws.
template <class F>
std::string outcome(F compute)
{
    try {
        return compute().to_string();
    } catch (const std::overflow_error&) {
        return std::string{kOverflow};
    } catch (const std::domain_error&) {
        return std::string{kDivideByZero};
    }
}

struct BinaryCase {
    std::string_view a;
    std::string_view b;
    std::string_view expected;
};

// --- construction ------------------------------------------------------------

static_assert(std::is_convertible_v<int, Decimal>);
static_assert(std::is_convertible_v<std::uint64_t, Decimal>);
static_assert(!std::is_constructible_v<Decimal, double>);
static_assert(!std::is_constructible_v<Decimal, bool>);
static_assert(!std::is_constructible_v<Decimal, char>);
static_assert(Decimal{}.is_zero() && Decimal{}.scale() == 0);
static_assert(Decimal{-7}.signum() == -1 && Decimal{7}.signum() == 1 && Decimal{0}.signum() == 0);
static_assert((-Decimal{5}).signum() == -1 && (-Decimal{0}).signum() == 0);
static_assert(Decimal::max_value().bits() == DecimalBits{0xFFFF'FFFFu, 0xFFFF'FFFFu, 0xFFFF'FFFFu, 0, false});
static_assert(Decimal::min_value().bits() == DecimalBits{0xFFFF'FFFFu, 0xFFFF'FFFFu, 0xFFFF'FFFFu, 0, true});

TEST(Decimal, Integers_convert_exactly_with_scale_zero)
{
    EXPECT_EQ(Decimal{0}.to_string(), "0");
    EXPECT_EQ(Decimal{19290}.to_string(), "19290");
    EXPECT_EQ(Decimal{-7}.to_string(), "-7");
    EXPECT_EQ(Decimal{std::numeric_limits<std::int64_t>::min()}.to_string(), "-9223372036854775808");
    EXPECT_EQ(Decimal{std::numeric_limits<std::uint64_t>::max()}.to_string(), "18446744073709551615");
    EXPECT_EQ(Decimal{2}.scale(), 0);
    EXPECT_EQ(Decimal::max_value().to_string(), "79228162514264337593543950335");
    EXPECT_EQ(Decimal::min_value().to_string(), "-79228162514264337593543950335");
}

TEST(Decimal, From_bits_validates_the_scale_and_has_no_negative_zero)
{
    const auto value = Decimal::from_bits({.lo = 235, .scale = 2, .negative = true});
    EXPECT_EQ(value.to_string(), "-2.35");
    EXPECT_EQ(value.bits(), (DecimalBits{235, 0, 0, 2, true}));
    EXPECT_EQ(Decimal::from_bits({.lo = 0, .mid = 1}).to_string(), "4294967296");
    EXPECT_EQ(Decimal::from_bits({.hi = 1}).to_string(), "18446744073709551616");

    const auto negative_zero = Decimal::from_bits({.scale = 3, .negative = true});
    EXPECT_EQ(negative_zero.bits(), (DecimalBits{0, 0, 0, 3, false}));
    EXPECT_EQ(negative_zero.to_string(), "0.000");

    EXPECT_THROW((void)Decimal::from_bits({.lo = 1, .scale = 29}), std::invalid_argument);
    EXPECT_THROW((void)Decimal::from_bits({.lo = 1, .scale = -1}), std::invalid_argument);
}

// --- JSON lexemes (04 §9.4: System.Text.Json GetDecimal) --------------------

TEST(DecimalJson, Keeps_the_scale_of_the_lexeme)
{
    // 04 §9.4 / protocol-reference: HUF "30790.0" keeps scale 1; GBP 54.99 stays exact.
    const auto huf = dec("30790.0");
    EXPECT_EQ(huf.to_string(), "30790.0");
    EXPECT_EQ(huf.scale(), 1);
    EXPECT_EQ(huf, Decimal{30790});
    EXPECT_EQ(dec("54.99").to_string(), "54.99");
    EXPECT_EQ(dec("366.3").to_string(), "366.3");
    EXPECT_EQ(dec("0.83536").to_string(), "0.83536");
    EXPECT_EQ(dec("1.0").to_string(), "1.0");
    EXPECT_EQ(dec("0").to_string(), "0");
    EXPECT_EQ(dec("0.000").to_string(), "0.000");
    EXPECT_EQ(dec("-0.0").to_string(), "0.0");
    EXPECT_EQ(dec("-2.35").to_string(), "-2.35");
    EXPECT_EQ(dec("79228162514264337593543950335"), Decimal::max_value());
}

TEST(DecimalJson, Applies_exponents)
{
    EXPECT_EQ(dec("1.5e2").to_string(), "150"); // 04 §9.4
    EXPECT_EQ(dec("1E2").to_string(), "100");
    EXPECT_EQ(dec("1e+2").to_string(), "100");
    EXPECT_EQ(dec("100e-2").to_string(), "1.00");
    EXPECT_EQ(dec("1.0e1").to_string(), "10");
    EXPECT_EQ(dec("1.50e1").to_string(), "15.0");
    EXPECT_EQ(dec("0e5").to_string(), "0");
    EXPECT_EQ(dec("0e-5").to_string(), "0.00000");
    EXPECT_EQ(dec("0.0e1").to_string(), "0");
    EXPECT_EQ(dec("1e28").to_string(), "10000000000000000000000000000");
    EXPECT_EQ(dec("7.9228162514264337593543950335e28"), Decimal::max_value());
    EXPECT_EQ(dec("79228162514264337593543950335e-28").to_string(), "7.9228162514264337593543950335");
}

TEST(DecimalJson, Rounds_excess_digits_like_dotnet)
{
    struct Case {
        std::string_view lexeme;
        std::string_view expected; // .NET
    };
    constexpr Case kCases[] = {
        {"0.1234567890123456789012345678901234", "0.1234567890123456789012345679"},
        {"12345678901234567890123456789.5", "12345678901234567890123456790"},
        {"9.99999999999999999999999999995", "10.000000000000000000000000000"},
        {"0.99999999999999999999999999995", "1.0000000000000000000000000000"},
        {"7.92281625142643375935439503355", "7.922816251426433759354395034"}, // 2^96 carry renormalised
        {"2.5e-28", "0.0000000000000000000000000002"},                       // half to even
        {"3.5e-28", "0.0000000000000000000000000004"},
        {"6e-29", "0.0000000000000000000000000001"},
        {"5e-29", "0.0000000000000000000000000000"},
        {"1e-50", "0.0000000000000000000000000000"},
        {"0.00000000000000000000000000250000000001", "0.0000000000000000000000000025"},
        // Utf8Parser counts the 30th significant digit as a non-zero tail, so
        // these exact halves round up instead of to even.
        {"1.00000000000000000000000000005", "1.0000000000000000000000000001"},
        {"1.000000000000000000000000000050", "1.0000000000000000000000000001"},
        {"79228162514264337593543950334.5", "79228162514264337593543950335"},
        // Zero scales: (byte)Math.Clamp(-e, 0, 28) with C#'s int.MinValue wrap.
        {"0e-2147483647", "0.0000000000000000000000000000"},
        {"0e-2147483648", "0"},
        {"0e-99999999999", "0"},
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(dec(c.lexeme).to_string(), c.expected) << c.lexeme;
    }
}

TEST(DecimalJson, Rejects_magnitudes_beyond_the_decimal_range)
{
    // 04 §9.4: "> ~7.9e28 → FormatException (verified 1e40)".
    for (const std::string_view lexeme : {"1e40", "1e29", "-1e29", "79228162514264337593543950336",
                                          "79228162514264337593543950335.5", "7.9228162514264337593543950336e28",
                                          "12345678901234567890123456789012345", "1e99999999999"}) {
        const auto value = Decimal::from_json_number(lexeme);
        ASSERT_FALSE(value.has_value()) << lexeme;
        EXPECT_EQ(value.error(), DecimalParseError::out_of_range) << lexeme;
    }
}

TEST(DecimalJson, Rejects_text_that_is_not_a_json_number)
{
    for (const std::string_view lexeme : {"", "-", "+1", "01", "-01", ".5", "5.", "1.", "1e", "1e+", "1E-", "0x10",
                                          " 1", "1 ", "NaN", "Infinity", "-Infinity", "1.5.2", "--1", "1,5", "1e5.0",
                                          "\"1\"", "true", "1_000"}) {
        const auto value = Decimal::from_json_number(lexeme);
        ASSERT_FALSE(value.has_value()) << lexeme;
        EXPECT_EQ(value.error(), DecimalParseError::invalid_syntax) << lexeme;
    }
}

// --- invariant plain text ----------------------------------------------------

TEST(DecimalParse, Reads_the_built_in_fx_table_exactly)
{
    // 03 §7.2 built-in table (CT-FX-02 values): every rate keeps its digits.
    for (const std::string_view rate :
         {"4.1058", "90.85", "1.95583", "0.92121", "24.355", "7.4795", "0.83536", "2.8972", "366.3", "20.107",
          "61.685", "10.3204", "4.4032", "5.346", "117.59", "10.9955", "1.118", "50.774", "90.1", "55.814",
          "136.01", "3.4933", "4.1925", "1.899", "402.42", "62.601", "0.79266", "10.9867", "507.37", "13399",
          "1.5627", "1.5913", "176.21", "7.4388"}) {
        const auto value = Decimal::parse(rate);
        ASSERT_TRUE(value.has_value()) << rate;
        EXPECT_EQ(value->to_string(), rate);
        EXPECT_EQ(*value, dec(rate));
    }
}

TEST(DecimalParse, Accepts_the_number_styles_float_grammar)
{
    // decimal.Parse(text, NumberStyles.Float, InvariantCulture) on .NET.
    struct Case {
        std::string_view text;
        std::string_view expected;
    };
    constexpr Case kCases[] = {{" 1.5 ", "1.5"}, {"+.5", "0.5"},  {"-5.", "-5"},      {"1e3", "1000"},
                               {"1E+3", "1000"}, {"\t2.5e-1\n", "0.25"}, {"007", "7"}, {"-0.50", "-0.50"}};
    for (const auto& c : kCases) {
        const auto value = Decimal::parse(c.text);
        ASSERT_TRUE(value.has_value()) << c.text;
        EXPECT_EQ(value->to_string(), c.expected) << c.text;
    }
    for (const std::string_view text :
         {"", "   ", ".", "e5", "1e", "1,5", "1 000", "0x10", "NaN", "--1", "+-1", "1.2.3", "1-", "1e40"}) {
        EXPECT_FALSE(Decimal::parse(text).has_value()) << text;
    }
}

// --- comparison and hashing --------------------------------------------------

TEST(DecimalEquality, Ignores_the_scale)
{
    // 02 §2.7 "1.0 == 1.00"; 05 §1.4 "30790 equals 30790.0".
    EXPECT_EQ(dec("1.0"), dec("1.00"));
    EXPECT_EQ(dec("30790"), dec("30790.0"));
    EXPECT_EQ(dec("0"), dec("-0.000"));
    EXPECT_NE(dec("1.0"), dec("1.01"));
    EXPECT_NE(dec("-1"), dec("1"));
    EXPECT_EQ(dec("1.0") <=> dec("1.00"), std::weak_ordering::equivalent);
}

TEST(DecimalOrdering, Compares_numerically_across_scales_and_signs)
{
    EXPECT_LT(dec("1.5"), dec("1.50001"));
    EXPECT_LT(dec("-2"), dec("-1.9999999999999999999999999999"));
    EXPECT_LT(dec("-0.0000000000000000000000000001"), dec("0"));
    EXPECT_LT(dec("0"), dec("0.0000000000000000000000000001"));
    EXPECT_GT(Decimal::max_value(), dec("79228162514264337593543950334"));
    EXPECT_EQ(Decimal::max_value(), dec("79228162514264337593543950334.9")); // 30 digits: rounds to fit
    EXPECT_LT(Decimal::min_value(), dec("-79228162514264337593543950334"));
    EXPECT_LE(dec("2.50"), dec("2.5"));
    EXPECT_GE(dec("2.50"), dec("2.5"));

    std::vector<Decimal> values{dec("105.32"), dec("29.98"), dec("-1"), dec("117.88"), dec("29.980"), dec("0")};
    std::ranges::stable_sort(values);
    std::vector<std::string> texts;
    std::ranges::transform(values, std::back_inserter(texts), [](const Decimal& d) { return d.to_string(); });
    EXPECT_EQ(texts, (std::vector<std::string>{"-1", "0", "29.98", "29.980", "105.32", "117.88"}));
}

TEST(DecimalHash, Is_consistent_with_equality)
{
    EXPECT_EQ(dec("1.0").hash(), dec("1.00").hash());
    EXPECT_EQ(dec("30790").hash(), dec("30790.0000").hash());
    EXPECT_EQ(dec("0").hash(), dec("0.000").hash());
    EXPECT_EQ(std::hash<Decimal>{}(dec("-2.50")), std::hash<Decimal>{}(dec("-2.5")));
    EXPECT_NE(dec("1").hash(), dec("-1").hash());
    EXPECT_NE(dec("1.5").hash(), dec("15").hash());

    const std::unordered_set<Decimal> totals{dec("105.32"), dec("105.320"), dec("117.88"), dec("117.8800")};
    EXPECT_EQ(totals.size(), 2u);
}

// --- arithmetic --------------------------------------------------------------

TEST(DecimalArithmetic, CT_FX_03_Exact_conversion_digits)
{
    // 03 §6.2 / §7.1 and CT-FX-03 [D]: divide first, then multiply, with .NET's
    // intermediate rounding.
    const auto huf = dec("366.3");
    const auto gbp = dec("0.83536");
    EXPECT_EQ((Decimal{19290} / huf).to_string(), "52.661752661752661752661752662");
    EXPECT_EQ((Decimal{30790} / huf).to_string(), "84.05678405678405678405678406");
    EXPECT_EQ((Decimal{30790} / huf * gbp).to_string(), "70.217675129675129675129675132");
    EXPECT_EQ((Decimal{19290} / huf * gbp).to_string(), "43.991521703521703521703521704");
    EXPECT_EQ((dec("14.99") * huf).to_string(), "5490.837");
    EXPECT_EQ((dec("1.5") * dec("0.67")).to_string(), "1.005"); // 03 §7.1

    // CT-FX-03 [A] ranges.
    const auto eur = dec("30790") / huf;
    EXPECT_GE(eur, Decimal{83});
    EXPECT_LE(eur, Decimal{85});
    const auto pounds = eur * gbp;
    EXPECT_GE(pounds, eur * dec("0.83"));
    EXPECT_LE(pounds, eur * dec("0.84"));
}

TEST(DecimalArithmetic, Addition_aligns_scales_and_rounds_like_dotnet)
{
    constexpr BinaryCase kCases[] = {
        // .NET
        {"1.10", "2.205", "3.305"},
        {"0.1", "0.2", "0.3"},
        {"5", "0.00", "5.00"},
        {"-5", "5", "0"},
        {"1.5", "-2.75", "-1.25"},
        {"0.0000000000000000000000000001", "0.0000000000000000000000000001", "0.0000000000000000000000000002"},
        {"79228162514264337593543950335", "0.4", "79228162514264337593543950335"},
        {"7922816251426433759354395033.5", "0.5", "7922816251426433759354395034"}, // carry: one digit dropped
        {"0.5", "79228162514264337593543950334", "79228162514264337593543950334"}, // tie to even
        {"9999999999999999999999999999", "0.99999999999999999999999999995", "10000000000000000000000000000"},
        {"79228162514264337593543950335", "1", kOverflow},
        {"79228162514264337593543950335", "0.5", kOverflow},
        {"1.5", "79228162514264337593543950334", kOverflow},
        {"-79228162514264337593543950335", "-0.5", kOverflow},
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(outcome([&] { return dec(c.a) + dec(c.b); }), c.expected) << c.a << " + " << c.b;
        EXPECT_EQ(outcome([&] { return dec(c.b) + dec(c.a); }), c.expected) << c.b << " + " << c.a;
    }
}

TEST(DecimalArithmetic, Subtraction_rounds_like_dotnet)
{
    constexpr BinaryCase kCases[] = {
        // .NET
        {"1", "0.0000000000000000000000000001", "0.9999999999999999999999999999"},
        {"10000000000000000000000000000", "0.1", "10000000000000000000000000000"},
        {"79228162514264337593543950335", "0.0000000000000000000000000001", "79228162514264337593543950335"},
        {"0.000", "0.000", "0.000"},
        {"2.5", "2.50", "0.00"},
        {"1", "3.000", "-2.000"},
        {"-79228162514264337593543950335", "1", kOverflow},
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(outcome([&] { return dec(c.a) - dec(c.b); }), c.expected) << c.a << " - " << c.b;
    }
}

TEST(DecimalArithmetic, Multiplication_rounds_like_dotnet)
{
    constexpr BinaryCase kCases[] = {
        // .NET
        {"14.99", "366.3", "5490.837"},
        {"52.66", "2", "105.32"},
        {"94.70", "2", "189.40"},
        {"-2.5", "0.5", "-1.25"},
        {"0", "1.50", "0.00"},
        {"0", "79228162514264337593543950335", "0"},
        {"0.0000000000000001", "0.0000000000000001", "0.0000000000000000000000000000"},
        {"0.000000000000000000000001", "0.000000000000000000000001", "0"},
        {"79228162514264337593543950335", "1.0", "79228162514264337593543950335"},
        {"1.0000000000000000000000000001", "1.0000000000000000000000000001", "1.0000000000000000000000000002"},
        {"0.5", "0.0000000000000000000000000001", "0.0000000000000000000000000000"},
        {"1.5", "0.0000000000000000000000000001", "0.0000000000000000000000000002"},
        {"2.5", "0.0000000000000000000000000001", "0.0000000000000000000000000002"},
        {"123456789012.345678", "987654321098.765432", "121932631137021794322511.81222"},
        {"-0.000000000001", "79228162514.264337593543950335", "-0.0792281625142643375935439503"},
        {"79228162514264337593543950335", "2", kOverflow},
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(outcome([&] { return dec(c.a) * dec(c.b); }), c.expected) << c.a << " * " << c.b;
        EXPECT_EQ(outcome([&] { return dec(c.b) * dec(c.a); }), c.expected) << c.b << " * " << c.a;
    }
}

TEST(DecimalArithmetic, Division_rounds_like_dotnet)
{
    constexpr BinaryCase kCases[] = {
        // .NET
        {"1.00", "2", "0.50"}, // exact: natural scale kept
        {"10", "0.5", "20"},
        {"100", "4.0", "25"},
        {"0.00", "5", "0.00"},
        {"1", "4", "0.25"}, // inexact at first: trailing zeros dropped
        {"1", "8", "0.125"},
        {"1", "3", "0.3333333333333333333333333333"},
        {"2", "3", "0.6666666666666666666666666667"},
        {"-2", "3", "-0.6666666666666666666666666667"},
        {"79228162514264337593543950335", "79228162514264337593543950335", "1"},
        {"79228162514264337593543950335", "-1", "-79228162514264337593543950335"},
        {"79228162514264337593543950335", "10", "7922816251426433759354395033.5"},
        {"7.9228162514264337593543950335", "3", "2.6409387504754779197847983445"},
        {"1", "79228162514264337593543950335", "0"},
        {"0.0000000000000000000000000001", "3", "0"},
        {"447717.28", "59636424277738.4921469318520", "0.0000000075074467562792339467"},
        {"-54523631", "0.0000000000000002", "-272618155000000000000000"},
        {"1", "0.0000000000000000000000000003", "3333333333333333333333333333.3"},
        {"5", "0.0000000000000000000000000002", "25000000000000000000000000000"},
        {"79228162514264337593543950335", "0.1", kOverflow},
        {"79228162514264337593543950335", "0.5", kOverflow},
        {"79228162514264337593543950335", "0.99999999999999999999999999", kOverflow},
        {"1", "0", kDivideByZero},
        {"0", "0", kDivideByZero},
        {"0", "0.000", kDivideByZero},
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(outcome([&] { return dec(c.a) / dec(c.b); }), c.expected) << c.a << " / " << c.b;
    }
}

TEST(DecimalArithmetic, Integers_mix_in_implicitly)
{
    const auto per_person = dec("52.66");
    EXPECT_EQ((per_person * 2).to_string(), "105.32"); // 03 §6.2: total = per person × adults
    EXPECT_EQ((2 * per_person).to_string(), "105.32");
    EXPECT_EQ((per_person + 1).to_string(), "53.66");
    EXPECT_EQ((Decimal{99'999'999} - per_person).to_string(), "99999946.34");
    EXPECT_TRUE(per_person > 0);
    EXPECT_TRUE(dec("-0.01") < 0);
}

TEST(DecimalArithmetic, Compound_assignment_and_negation)
{
    Decimal total = dec("14.99");
    total += dec("94.70");
    EXPECT_EQ(total.to_string(), "109.69");
    total -= dec("0.69");
    EXPECT_EQ(total.to_string(), "109.00");
    total *= 3;
    EXPECT_EQ(total.to_string(), "327.00");
    total /= dec("0.5");
    EXPECT_EQ(total.to_string(), "654.0"); // exact quotient: natural scale 2 - 1
    EXPECT_EQ((-total).to_string(), "-654.0");
    EXPECT_EQ((-(-total)), total);
    EXPECT_EQ((-dec("0.00")).to_string(), "0.00");
    EXPECT_THROW(total /= 0, std::domain_error);
    EXPECT_EQ(total.to_string(), "654.0"); // unchanged after the throw
}

// --- rounding ----------------------------------------------------------------

TEST(DecimalRound, P_27_Rounding_mode_away_from_zero)
{
    // P-27: rates {XXX: 0.5, HUF: 0.5}; amount / rate(EUR = 1) × rate(target).
    const auto rate = dec("0.5");
    const auto xxx = dec("2.01") / 1 * rate;
    EXPECT_EQ(xxx.to_string(), "1.005");
    EXPECT_EQ(xxx.round(2, Rounding::away_from_zero).to_string(), "1.01");
    EXPECT_EQ(xxx.round(2, Rounding::to_even).to_string(), "1.00"); // banker's would give 1.00
    const auto huf5 = Decimal{5} / 1 * rate;
    EXPECT_EQ(huf5.round(0, Rounding::away_from_zero).to_string(), "3");
    EXPECT_EQ(huf5.round(0, Rounding::to_even).to_string(), "2"); // banker's would give 2
    EXPECT_EQ((Decimal{7} / 1 * rate).round(0, Rounding::away_from_zero).to_string(), "4");
}

TEST(DecimalRound, Matches_math_round)
{
    struct Case {
        std::string_view value;
        int decimals;
        std::string_view to_even;        // .NET Math.Round(..., ToEven)
        std::string_view away_from_zero; // .NET Math.Round(..., AwayFromZero)
    };
    constexpr Case kCases[] = {
        {"2.5", 0, "2", "3"},
        {"-2.5", 0, "-2", "-3"},
        {"3.5", 0, "4", "4"},
        {"1.005", 2, "1.00", "1.01"},
        {"2.50000000000000000000000001", 0, "3", "3"},
        {"2.4999999999999999999999999999", 0, "2", "2"},
        {"-0.004", 2, "0.00", "0.00"},
        {"0.995", 2, "1.00", "1.00"},
        {"9.995", 2, "10.00", "10.00"},
        {"1.5", 4, "1.5", "1.5"}, // scale <= decimals: unchanged
        {"5490.837", 0, "5491", "5491"},
        {"52.661752661752661752661752662", 2, "52.66", "52.66"},
        {"79228162514264337593543950335", 0, "79228162514264337593543950335", "79228162514264337593543950335"},
        {"7.9228162514264337593543950335", 0, "8", "8"},
        {"-7.9228162514264337593543950335", 27, "-7.922816251426433759354395034", "-7.922816251426433759354395034"},
        {"0.00000000000000000000000000005", 28, "0.0000000000000000000000000000", "0.0000000000000000000000000000"},
    };
    for (const auto& c : kCases) {
        const auto value = dec(c.value);
        EXPECT_EQ(value.round(c.decimals, Rounding::to_even).to_string(), c.to_even) << c.value;
        EXPECT_EQ(value.round(c.decimals, Rounding::away_from_zero).to_string(), c.away_from_zero) << c.value;
    }
}

TEST(DecimalRound, Rejects_decimals_outside_0_to_28)
{
    EXPECT_THROW((void)dec("1.5").round(-1, Rounding::to_even), std::out_of_range);
    EXPECT_THROW((void)dec("1.5").round(29, Rounding::away_from_zero), std::out_of_range);
    EXPECT_EQ(dec("1.5").round(28, Rounding::to_even).to_string(), "1.5");
}

// --- the 03 §11 walk-through and CT-FSS-01 / CT-SRCH-07 numbers --------------

/// 03 §6.2: convert (amount / rate(from) × rate(to)), round per person away
/// from zero to the target decimals, then multiply by the adults.
struct Priced {
    std::string per_person;
    std::string total;
};

Priced reprice(Decimal amount, Decimal rate_from, Decimal rate_to, int decimals, int adults)
{
    const Decimal per_person = (amount / rate_from * rate_to).round(decimals, Rounding::away_from_zero);
    return {per_person.to_string(), (per_person * adults).to_string()};
}

TEST(DecimalPricing, CT_FSS_01_Per_person_and_totals_in_EUR)
{
    // 03 §11 table / CT-FSS-01 [D]: HUF fares ÷ 366.3, 2 dp, × 2 adults.
    struct Case {
        int huf;
        std::string_view per_person;
        std::string_view total;
    };
    constexpr Case kCases[] = {{19290, "52.66", "105.32"},
                               {21590, "58.94", "117.88"},
                               {25390, "69.31", "138.62"},
                               {30790, "84.06", "168.12"},
                               {34690, "94.70", "189.40"}};
    for (const auto& c : kCases) {
        const auto priced = reprice(Decimal{c.huf}, dec("366.3"), Decimal{1}, 2, 2);
        EXPECT_EQ(priced.per_person, c.per_person) << c.huf;
        EXPECT_EQ(priced.total, c.total) << c.huf;
    }
    // Same currency (BTS→LTN 14.99 EUR): Convert returns the amount, Reprice still rounds.
    const auto bts = dec("14.99").round(2, Rounding::away_from_zero);
    EXPECT_EQ(bts.to_string(), "14.99");
    EXPECT_EQ((bts * 2).to_string(), "29.98");
}

TEST(DecimalPricing, Reprice_to_HUF_and_GBP_from_03_11)
{
    // 03 §11 "Reprice": 14.99 × 366.3 = 5490.837 → 5491 → 10982 HUF.
    const auto huf = reprice(dec("14.99"), Decimal{1}, dec("366.3"), 0, 2);
    EXPECT_EQ(huf.per_person, "5491");
    EXPECT_EQ(huf.total, "10982");
    // GBP: 14.99 × 0.83536 → 12.52 → 25.04; 19290 / 366.3 × 0.83536 → 43.99 → 87.98 (CT-SRCH-06 43.99).
    const auto bts = reprice(dec("14.99"), Decimal{1}, dec("0.83536"), 2, 2);
    EXPECT_EQ(bts.per_person, "12.52");
    EXPECT_EQ(bts.total, "25.04");
    const auto bud = reprice(Decimal{19290}, dec("366.3"), dec("0.83536"), 2, 2);
    EXPECT_EQ(bud.per_person, "43.99");
    EXPECT_EQ(bud.total, "87.98");
}

TEST(DecimalPricing, Return_leg_GBP_fares_from_03_11)
{
    // 03 §11 return leg: synthetic GBP fares ÷ 0.83536, 2 dp, × 2 adults.
    struct Case {
        std::string_view gbp;
        std::string_view per_person;
        std::string_view total;
    };
    constexpr Case kCases[] = {{"17.99", "21.54", "43.08"},
                               {"19.99", "23.93", "47.86"},
                               {"24.99", "29.92", "59.84"},
                               {"29.99", "35.90", "71.80"}};
    for (const auto& c : kCases) {
        const auto priced = reprice(dec(c.gbp), dec("0.83536"), Decimal{1}, 2, 2);
        EXPECT_EQ(priced.per_person, c.per_person) << c.gbp;
        EXPECT_EQ(priced.total, c.total) << c.gbp;
    }
}

TEST(DecimalPricing, CT_SRCH_07_Same_currency_stays_exact)
{
    // CT-SRCH-07: 19290 HUF → HUF is 19290 (0 dp) and 38580 for 2 adults.
    const auto per_person = Decimal{19290}.round(0, Rounding::away_from_zero);
    EXPECT_EQ(per_person.to_string(), "19290");
    EXPECT_EQ((per_person * 2).to_string(), "38580");
    // 03 §15: a fractional same-currency fare is still rounded (19290.5 → 19291).
    EXPECT_EQ(dec("19290.5").round(0, Rounding::away_from_zero).to_string(), "19291");
    // The JSON lexeme "30790.0" rounds to scale 0 without changing its value.
    EXPECT_EQ(dec("30790.0").round(0, Rounding::away_from_zero).to_string(), "30790");
}

// --- text ----------------------------------------------------------------------

TEST(DecimalToString, Prints_the_value_with_its_own_scale)
{
    // 01 §8.10: "the raw decimal's default string ... the decimal's own scale".
    EXPECT_EQ(dec("29.99").to_string(), "29.99");
    EXPECT_EQ(dec("30790.0").to_string(), "30790.0");
    EXPECT_EQ(dec("-2.35").to_string(), "-2.35");
    EXPECT_EQ(dec("0").to_string(), "0");
    EXPECT_EQ(dec("0.00").to_string(), "0.00");
    EXPECT_EQ(dec("0.05").to_string(), "0.05");
    EXPECT_EQ(dec("-0.0000000000000000000000000001").to_string(), "-0.0000000000000000000000000001");
    EXPECT_EQ(dec("15.00").to_string(), "15.00");
    EXPECT_EQ(dec("1e3").to_string(), "1000");
}

TEST(DecimalToString, Streams_the_invariant_text)
{
    std::ostringstream out;
    out << dec("-30790.0") << ' ' << Decimal{} << ' ' << dec("0.05");
    EXPECT_EQ(out.str(), "-30790.0 0 0.05");
    EXPECT_EQ(testing::PrintToString(dec("52.66")), "52.66");
}

TEST(DecimalToString, Uses_the_culture_separator_and_sign)
{
    EXPECT_EQ(dec("-1234.50").to_string(kDeDe), "-1234,50");
    EXPECT_EQ(dec("29.99").to_string(kEnUs), "29.99");
    const NumberSymbols swedish{.decimal_separator = ",", .group_separator = " ", .negative_sign = "−"};
    EXPECT_EQ(dec("-0.5").to_string(swedish), "−0,5");
    EXPECT_EQ(dec("-0.00").to_string(swedish), "0,00");
}

TEST(DecimalFormat, App_patterns_round_half_away_from_zero)
{
    struct Case {
        std::string_view value;
        std::string_view pattern;
        std::string_view expected; // .NET, en-US
    };
    constexpr Case kCases[] = {
        {"2.345", "#,##0.00", "2.35"},
        {"2.355", "#,##0.00", "2.36"},
        {"-2.345", "#,##0.00", "-2.35"},
        {"1234567.891", "#,##0.00", "1,234,567.89"},
        {"-0.004", "#,##0.00", "0.00"}, // no minus sign once rounded to zero (02 §2.7)
        {"0", "#,##0.00", "0.00"},
        {"99.995", "#,##0.00", "100.00"},
        {"999999.995", "#,##0.00", "1,000,000.00"},
        {"2.5", "#,##0", "3"},
        {"3.5", "#,##0", "4"},
        {"-2.5", "#,##0", "-3"},
        {"30790.0", "#,##0", "30,790"},
        {"0.5", "#,##0", "1"},
        {"12345678901234567890.5", "#,##0", "12,345,678,901,234,567,891"},
        {"105.32", "0.##", "105.32"}, // 01 §16 "{Total:0.##}"
        {"38580", "0.##", "38580"},
        {"15.00", "0.##", "15"},
        {"1.50", "0.##", "1.5"},
        {"0.05", "0.##", "0.05"},
        {"-1.005", "0.##", "-1.01"},
        {"0.995", "0.##", "1"},
        {"0.0000000000000000000000000001", "0.##", "0"},
        {"-0.4", "0", "0"},
        {"123", "0.00", "123.00"},
        {"172.5", "0", "173"},
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(dec(c.value).format(c.pattern, kEnUs), c.expected) << c.value << " " << c.pattern;
    }
}

TEST(DecimalFormat, Follows_dotnet_custom_pattern_rules)
{
    struct Case {
        std::string_view value;
        std::string_view pattern;
        std::string_view expected; // .NET, en-US
    };
    constexpr Case kCases[] = {
        {"0.5", "#.##", ".5"},          {"-0.5", "#.##", "-.5"},           {"-0.4", "#", ""},
        {"5", "0,000", "0,005"},        {"1234567", "#,#,##0", "1,234,567"}, {"1234.5", ",##0.0,0", "1234.50"},
        {"1234567", "#,##0,", "1,235"}, {"1234567", "0,", "1235"},         {"1234567", "0,,.00", "1.23"},
        {"5", "0.0.0", "5.00"},         {"1234567.5", "#,##0.##", "1,234,567.5"},
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(dec(c.value).format(c.pattern, kEnUs), c.expected) << c.value << " " << c.pattern;
    }
}

TEST(DecimalFormat, Uses_culture_separators_and_group_sizes)
{
    EXPECT_EQ(dec("1234567.891").format("#,##0.00", kDeDe), "1.234.567,89");
    EXPECT_EQ(dec("2.345").format("#,##0.00", kDeDe), "2,35");
    EXPECT_EQ(dec("-1234.5").format("#,##0.00", kDeDe), "-1.234,50");

    const NumberSymbols french{.decimal_separator = ",", .group_separator = " ", .negative_sign = "−"};
    EXPECT_EQ(dec("-1234567.891").format("#,##0.00", french), "−1 234 567,89");

    // .NET NumberGroupSizes semantics: the last size repeats; 0 stops grouping.
    const auto value = Decimal{1234567890};
    NumberSymbols indian = kEnUs;
    indian.group_sizes = {3, 2};
    EXPECT_EQ(value.format("#,##0", indian), "1,23,45,67,890");
    NumberSymbols single = kEnUs;
    single.group_sizes = {3, 0};
    EXPECT_EQ(value.format("#,##0", single), "1234567,890");
    NumberSymbols none = kEnUs;
    none.group_sizes = {};
    EXPECT_EQ(value.format("#,##0", none), "1234567890");
}

TEST(DecimalFormat, Rejects_unsupported_patterns)
{
    for (const std::string_view pattern : {"", ".", ",", "%", "0%", "0.00E+0", "#;(#)", "'x'0", "\\0", "N2", "0 EUR"}) {
        EXPECT_THROW((void)dec("1").format(pattern, kEnUs), std::invalid_argument) << pattern;
    }
}

TEST(DecimalToDouble, Converts_like_dotnet)
{
    struct Case {
        std::string_view value;
        std::uint64_t bits; // .NET BitConverter.DoubleToInt64Bits((double)value)
    };
    constexpr Case kCases[] = {
        {"0.1", 0x3FB999999999999Au},
        {"52.661752661752661752661752662", 0x404A54B44FAC2263u},
        {"79228162514264337593543950335", 0x45F0000000000000u},
        {"-2.35", 0xC002CCCCCCCCCCCDu},
        {"0.0000000000000000000000000001", 0x3A1FB0F6BE50601Au},
        {"0", 0x0u},
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(dec(c.value).to_double()), c.bits) << c.value;
    }
}

} // namespace
