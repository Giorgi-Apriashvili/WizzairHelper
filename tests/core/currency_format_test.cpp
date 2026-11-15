// wizz::currency_format — CurrencyFormat of 03 §6.4 and 02 §2.7 (Money.ToString),
// CT-SRCH-07. Tests run under en-US (07 §1); de-DE uses its NumberSymbols.

#include "wizz/core/currency_format.hpp"
#include "wizz/core/decimal.hpp"
#include "wizz/core/locale.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace currency_format = wizz::currency_format;
using wizz::Decimal;
using wizz::NumberSymbols;

const NumberSymbols kEnUs{.decimal_separator = ".", .group_separator = ",", .negative_sign = "-", .group_sizes = {3}};
const NumberSymbols kDeDe{.decimal_separator = ",", .group_separator = ".", .negative_sign = "-", .group_sizes = {3}};

Decimal dec(std::string_view lexeme)
{
    const auto value = Decimal::from_json_number(lexeme);
    if (!value) {
        throw std::invalid_argument("not a decimal lexeme: " + std::string{lexeme});
    }
    return *value;
}

TEST(CurrencyFormat, Decimals_are_zero_only_for_the_no_minor_unit_codes)
{
    // 03 §6.4: NoMinorUnits compared case-insensitively; everything else is 2,
    // including unknown and empty codes (not ISO 4217: HUF would be 2 there).
    EXPECT_EQ(currency_format::kNoMinorUnits.size(), 7u);
    for (const std::string_view code : {"HUF", "ISK", "JPY", "KRW", "UZS", "KZT", "AMD", "huf", "Huf", "jPy", "amd"}) {
        EXPECT_EQ(currency_format::decimals(code), 0) << code;
    }
    for (const std::string_view code : {"EUR", "GBP", "USD", "PLN", "RON", "CZK", "", "XYZ", "XXX", "HU", "HUFF",
                                        " HUF", "HUF ", "\xC4\xB1SK" /* dotless i: no ordinal case fold */}) {
        EXPECT_EQ(currency_format::decimals(code), 2) << code;
    }
}

TEST(CurrencyFormat, Number_format_follows_the_decimals)
{
    EXPECT_EQ(currency_format::number_format("HUF"), "#,##0");
    EXPECT_EQ(currency_format::number_format("jpy"), "#,##0");
    EXPECT_EQ(currency_format::number_format("EUR"), "#,##0.00");
    EXPECT_EQ(currency_format::number_format(""), "#,##0.00");
}

TEST(CurrencyFormat, Money_table_of_02_2_7)
{
    struct Case {
        std::string_view amount;
        std::string_view code;
        std::string_view en_us;
    };
    constexpr Case kCases[] = {
        {"2.345", "EUR", "2.35 EUR"},
        {"2.355", "EUR", "2.36 EUR"},
        {"-2.345", "EUR", "-2.35 EUR"},
        {"2.5", "HUF", "3 HUF"},
        {"3.5", "huf", "4 HUF"},
        {"-2.5", "HUF", "-3 HUF"},
        {"1234567.891", "eur", "1,234,567.89 EUR"},
        {"30790.0", "HUF", "30,790 HUF"},
        {"0.5", "JPY", "1 JPY"},
        {"-0.004", "EUR", "0.00 EUR"}, // no minus sign
    };
    for (const auto& c : kCases) {
        EXPECT_EQ(currency_format::format(dec(c.amount), c.code, kEnUs), c.en_us) << c.amount << " " << c.code;
    }
    // de-DE column (the table omits the " EUR" suffix that Format always appends).
    EXPECT_EQ(currency_format::format(dec("2.345"), "EUR", kDeDe), "2,35 EUR");
    EXPECT_EQ(currency_format::format(dec("1234567.891"), "eur", kDeDe), "1.234.567,89 EUR");
}

TEST(CurrencyFormat, Examples_of_03_6_4)
{
    EXPECT_EQ(currency_format::format(dec("1234.5"), "EUR", kEnUs), "1,234.50 EUR");
    EXPECT_EQ(currency_format::format(dec("5490.5"), "huf", kEnUs), "5,491 HUF");
    EXPECT_EQ(currency_format::format(dec("-1234.5"), "EUR", kEnUs), "-1,234.50 EUR");
    EXPECT_EQ(currency_format::format(dec("1234.5"), "EUR", kDeDe), "1.234,50 EUR");
    EXPECT_EQ(currency_format::format(Decimal{19290}, "HUF", kEnUs), "19,290 HUF"); // Money.ToString()
}

TEST(CurrencyFormat, CT_SRCH_07_Decimals_number_format_and_format)
{
    EXPECT_EQ(currency_format::decimals("HUF"), 0);
    EXPECT_EQ(currency_format::number_format("EUR"), "#,##0.00");
    EXPECT_EQ(currency_format::format(dec("1234.5"), "EUR", kEnUs), "1,234.50 EUR");
    EXPECT_EQ(currency_format::format(Decimal{19290}, "HUF", kEnUs), "19,290 HUF");
}

TEST(CurrencyFormat, Grid_fare_and_total_cells)
{
    // 01 §8 "Fare (local)" examples and CT-FSS-01 converted totals.
    EXPECT_EQ(currency_format::format(dec("29.99"), "EUR", kEnUs), "29.99 EUR");
    EXPECT_EQ(currency_format::format(Decimal{8790}, "HUF", kEnUs), "8,790 HUF");
    EXPECT_EQ(currency_format::format(dec("105.32"), "EUR", kEnUs), "105.32 EUR");
    EXPECT_EQ(currency_format::format(Decimal{38580}, "HUF", kEnUs), "38,580 HUF");
    EXPECT_EQ(currency_format::format(Decimal{15}, "EUR", kEnUs), "15.00 EUR");
}

TEST(CurrencyFormat, Unknown_and_empty_codes_use_two_decimals)
{
    EXPECT_EQ(currency_format::format(dec("12.5"), "xyz", kEnUs), "12.50 XYZ");
    EXPECT_EQ(currency_format::format(Decimal{5}, "", kEnUs), "5.00 ");
    EXPECT_EQ(currency_format::format(dec("0.004"), "btc", kEnUs), "0.00 BTC");
}

} // namespace
