// FX rates: 05 §3.5 CT-FX-01..04, CT-LATENT-01 (parse part), the frozen live
// fx.json of 02 §14, 03 §7.1 FxRates.Supports/Convert, 03 §7.2 parsers, disk
// cache shape and built-in table, DECISIONS D-13.

#include "wizz/core/fx_rates.hpp"

#include "wizz/core/errors.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/text.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using wizz::Date;
using wizz::Decimal;
using wizz::FxRates;
using wizz::FxRatesPtr;
namespace fx = wizz::fx_rates;
namespace json = wizz::json;

/// The 18 Wizz currencies (05 §2.6).
constexpr std::array<std::string_view, 18> kWizzCurrencies{"AED", "ALL", "BAM", "CHF", "CZK", "DKK",
                                                           "EUR", "GBP", "GEL", "HUF", "MDL", "MKD",
                                                           "NOK", "PLN", "RON", "RSD", "SEK", "USD"};

[[nodiscard]] Decimal dec(std::string_view lexeme)
{
    const auto parsed = Decimal::from_json_number(lexeme);
    if (!parsed) {
        throw std::logic_error("bad decimal literal in a test");
    }
    return *parsed;
}

[[nodiscard]] const FxRates& rates()
{
    static const FxRates parsed = fx::parse_frankfurter_v2(wizz::test::read_fixture("fx_frankfurter_v2.json"));
    return parsed;
}

[[nodiscard]] const FxRates& fawaz()
{
    static const FxRates parsed = fx::parse_fawaz(wizz::test::read_fixture("fx_fawaz.json"));
    return parsed;
}

/// The stored rate for `code` (exact key), or nullopt.
[[nodiscard]] std::optional<Decimal> rate(const FxRates& table, const std::string& code)
{
    const auto it = table.per_eur.find(code);
    return it == table.per_eur.end() ? std::nullopt : std::optional<Decimal>{it->second};
}

/// Runs `action`, expects exactly an `E`, and returns its message.
template <class E>
[[nodiscard]] std::string message_of(const std::function<void()>& action)
{
    try {
        action();
    } catch (const E& error) {
        return error.what();
    }
    ADD_FAILURE() << "expected an exception";
    return {};
}

[[nodiscard]] FxRates table(std::initializer_list<std::pair<const std::string, Decimal>> entries)
{
    FxRates result;
    result.per_eur = std::map<std::string, Decimal>{entries};
    return result;
}

/// The currency-service loop of 03 §7.2 reduced to parsing: the first source whose
/// table parses and holds at least 10 rates wins; json / format errors move on.
[[nodiscard]] std::optional<FxRates> first_usable(const std::vector<std::pair<std::string, std::string>>& sources)
{
    for (const auto& [name, body] : sources) {
        try {
            FxRates parsed = name.starts_with("Frankfurter") ? fx::parse_frankfurter_v2(body) : fx::parse_fawaz(body, name);
            if (parsed.per_eur.size() < 10) {
                continue;
            }
            return parsed;
        } catch (const json::Error&) {
            continue;
        } catch (const wizz::FormatError&) {
            continue;
        }
    }
    return std::nullopt;
}

/// `text` with the first occurrence of `from` replaced by `to`.
[[nodiscard]] std::string replace_first(std::string text, std::string_view from, std::string_view to)
{
    const std::size_t at = text.find(from);
    if (at == std::string::npos) {
        throw std::logic_error("pattern missing from fixture");
    }
    text.replace(at, from.size(), to);
    return text;
}

// ---------------------------------------------------------------------------
// CT-FX-01 — Frankfurter v2
// ---------------------------------------------------------------------------

TEST(FxRates, CT_FX_01_Frankfurter_v2_covers_every_wizz_currency)
{
    const FxRates& r = rates();
    // [A]
    EXPECT_EQ(r.source, "Frankfurter");
    EXPECT_EQ(r.date, Date::from_ymd(2026, 11, 2));
    for (const std::string_view code : kWizzCurrencies) {
        EXPECT_TRUE(fx::supports(r, code)) << code;
    }
    // [D] 170 entries, EUR included (03 §7.1: never stripped), not approximate.
    EXPECT_EQ(r.per_eur.size(), 170u);
    ASSERT_TRUE(rate(r, "EUR").has_value());
    EXPECT_EQ(rate(r, "EUR")->to_string(), "1.0");
    EXPECT_FALSE(r.is_approximate);
    // 05 §2.5: the rates of the 18 Wizz currencies.
    const std::vector<std::pair<std::string, std::string_view>> expected{
        {"AED", "4.1058"}, {"ALL", "90.85"},   {"BAM", "1.9558"}, {"CHF", "0.92121"}, {"CZK", "24.355"},
        {"DKK", "7.4795"}, {"EUR", "1"},       {"GBP", "0.83536"}, {"GEL", "2.8972"}, {"HUF", "366.3"},
        {"MDL", "20.107"}, {"MKD", "61.685"},  {"NOK", "10.3204"}, {"PLN", "4.4032"}, {"RON", "5.346"},
        {"RSD", "117.59"}, {"SEK", "10.9955"}, {"USD", "1.118"},
    };
    for (const auto& [code, value] : expected) {
        ASSERT_TRUE(rate(r, code).has_value()) << code;
        EXPECT_EQ(*rate(r, code), dec(value)) << code;
    }
    // [N] quote keys are upper-cased.
    for (const auto& [code, value] : r.per_eur) {
        EXPECT_EQ(code, wizz::text::to_upper_invariant(code));
        EXPECT_GT(value, Decimal{0}) << code;
    }
}

TEST(FxRates, CT_FX_01_N_Rows_with_other_base_empty_quote_or_non_positive_rate_are_skipped)
{
    const FxRates r = fx::parse_frankfurter_v2(R"([
        {"date":"2026-11-02","base":"EUR","quote":"huf","rate":366.3},
        {"date":"2026-11-05","base":"USD","quote":"GBP","rate":0.75},
        {"date":"2026-11-06","quote":"PLN","rate":4.4},
        {"date":"2026-11-07","base":null,"quote":"PLN","rate":4.4},
        {"date":"2026-11-08","base":"EUR","quote":"","rate":1.5},
        {"date":"2026-11-09","base":"EUR","quote":"CZK","rate":0},
        {"date":"2026-11-10","base":"EUR","quote":"DKK","rate":-7.4},
        {"date":"2026-10-30","base":"eur","quote":"Gbp","rate":0.8},
        {"date":"not a date","base":"EUR","quote":"SEK","rate":10.9955},
        {"base":"EUR","quote":"NOK","rate":10.32},
        {"date":null,"base":"EUR","quote":"RON","rate":5.346},
        {"date":"2026-11-01","base":"EUR","quote":"GBP","rate":0.83536}
    ])");
    // Kept: HUF, GBP (base compared case-insensitively; the later duplicate overwrites), SEK, NOK, RON.
    ASSERT_EQ(r.per_eur.size(), 5u);
    EXPECT_EQ(*rate(r, "HUF"), dec("366.3"));
    EXPECT_EQ(rate(r, "GBP")->to_string(), "0.83536");
    EXPECT_TRUE(rate(r, "SEK").has_value());
    EXPECT_TRUE(rate(r, "NOK").has_value());
    EXPECT_TRUE(rate(r, "RON").has_value());
    EXPECT_FALSE(rate(r, "PLN").has_value());
    EXPECT_FALSE(rate(r, "CZK").has_value());
    EXPECT_FALSE(rate(r, "DKK").has_value());
    EXPECT_FALSE(rate(r, "").has_value());
    // Date = the latest date of a kept row; skipped rows (11-05..11-10) do not count.
    EXPECT_EQ(r.date, Date::from_ymd(2026, 11, 2));
    EXPECT_EQ(r.source, "Frankfurter");
    EXPECT_FALSE(r.is_approximate);
}

TEST(FxRates, Frankfurter_without_any_dated_row_is_dated_0001_01_01)
{
    const FxRates r = fx::parse_frankfurter_v2(R"([{"base":"EUR","quote":"HUF","rate":366.3}])");
    EXPECT_EQ(r.date, Date::min_value());
    EXPECT_EQ(r.per_eur.size(), 1u);
    EXPECT_TRUE(fx::parse_frankfurter_v2("[]").per_eur.empty());
}

TEST(FxRates, D13_Frankfurter_structural_problems_reject_the_whole_table)
{
    // Root not an array (03 §7.2 exact text).
    EXPECT_EQ(message_of<wizz::FormatError>([] { (void)fx::parse_frankfurter_v2(R"({"rates":[]})"); }),
              "Frankfurter v2: expected an array.");
    // A non-object element (InvalidOperationException).
    EXPECT_EQ(message_of<json::TypeError>([] {
                  (void)fx::parse_frankfurter_v2(R"([{"base":"EUR","quote":"HUF","rate":366.3},"oops"])");
              }),
              "The requested operation requires an element of type 'Object', but the target element has type "
              "'String'.");
    EXPECT_THROW((void)fx::parse_frankfurter_v2("[[1,2]]"), json::TypeError);
    EXPECT_THROW((void)fx::parse_frankfurter_v2("[null]"), json::TypeError);
    // Missing quote / rate (KeyNotFoundException).
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"base":"EUR","rate":366.3}])"), wizz::FormatError);
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"base":"EUR","quote":"HUF"}])"), wizz::FormatError);
    // Wrongly typed quote / rate / base / date.
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"base":"EUR","quote":null,"rate":366.3}])"), json::TypeError);
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"base":"EUR","quote":348,"rate":366.3}])"), json::TypeError);
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"base":"EUR","quote":"HUF","rate":"366.3"}])"),
                 json::TypeError);
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"base":"EUR","quote":"HUF","rate":null}])"), json::TypeError);
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"base":1,"quote":"HUF","rate":366.3}])"), json::TypeError);
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"date":20261102,"base":"EUR","quote":"HUF","rate":366.3}])"),
                 json::TypeError);
    // A rate that does not fit a decimal (FormatException).
    EXPECT_THROW((void)fx::parse_frankfurter_v2(R"([{"base":"EUR","quote":"HUF","rate":1e30}])"),
                 json::FormatError);
    // Malformed JSON (JsonException).
    EXPECT_THROW((void)fx::parse_frankfurter_v2("[{"), json::ParseError);
    EXPECT_THROW((void)fx::parse_frankfurter_v2(""), json::ParseError);
    // The base filter runs first: a non-EUR row is skipped before its fields are read.
    EXPECT_TRUE(fx::parse_frankfurter_v2(R"([{"base":"USD"}])").per_eur.empty());
    // A row skipped for an empty quote or a non-positive rate never reads its date.
    EXPECT_TRUE(fx::parse_frankfurter_v2(R"([{"date":5,"base":"EUR","quote":"","rate":1.5},
                                             {"date":[],"base":"EUR","quote":"HUF","rate":0}])")
                    .per_eur.empty());
}

// ---------------------------------------------------------------------------
// CT-FX-02 — built-in table
// ---------------------------------------------------------------------------

TEST(FxRates, CT_FX_02_Built_in_table_covers_every_wizz_currency)
{
    const FxRatesPtr built_in = fx::built_in_rates();
    ASSERT_NE(built_in, nullptr);
    // [A]
    for (const std::string_view code : kWizzCurrencies) {
        EXPECT_TRUE(fx::supports(*built_in, code)) << code;
    }
    EXPECT_TRUE(built_in->is_approximate);
    // [D]
    EXPECT_EQ(built_in->per_eur.size(), 34u);
    EXPECT_EQ(built_in->date, Date::from_ymd(2026, 11, 2));
    EXPECT_EQ(built_in->source, "built-in table");
    const std::vector<std::pair<std::string, std::string_view>> expected{
        {"AED", "4.1058"},  {"ALL", "90.85"},  {"BAM", "1.95583"}, {"CHF", "0.92121"}, {"CZK", "24.355"},
        {"DKK", "7.4795"},  {"GBP", "0.83536"}, {"GEL", "2.8972"}, {"HUF", "366.3"},   {"MDL", "20.107"},
        {"MKD", "61.685"},  {"NOK", "10.3204"}, {"PLN", "4.4032"}, {"RON", "5.346"},   {"RSD", "117.59"},
        {"SEK", "10.9955"}, {"USD", "1.118"},   {"UAH", "50.774"}, {"RUB", "90.1"},    {"TRY", "55.814"},
        {"ISK", "136.01"},  {"ILS", "3.4933"},  {"SAR", "4.1925"}, {"AZN", "1.899"},   {"AMD", "402.42"},
        {"EGP", "62.601"},  {"JOD", "0.79266"}, {"MAD", "10.9867"}, {"KZT", "507.37"}, {"UZS", "13399"},
        {"CAD", "1.5627"},  {"AUD", "1.5913"},  {"JPY", "176.21"}, {"CNY", "7.4388"},
    };
    for (const auto& [code, value] : expected) {
        ASSERT_TRUE(rate(*built_in, code).has_value()) << code;
        EXPECT_EQ(rate(*built_in, code)->to_string(), value) << code;
    }
    // EUR is implicit, not a table entry.
    EXPECT_FALSE(rate(*built_in, "EUR").has_value());
    EXPECT_TRUE(fx::supports(*built_in, "EUR"));
    // CT-FX-05 Assert.Same: the very same instance every time.
    EXPECT_EQ(fx::built_in_rates().get(), built_in.get());
}

// ---------------------------------------------------------------------------
// CT-FX-03 — Convert
// ---------------------------------------------------------------------------

TEST(FxRates, CT_FX_03_Converts_through_eur)
{
    const FxRates& r = rates();
    const std::optional<Decimal> eur = fx::convert(r, Decimal{30790}, "HUF", "EUR");
    ASSERT_TRUE(eur.has_value());
    // [A]
    EXPECT_GE(*eur, Decimal{83});
    EXPECT_LE(*eur, Decimal{85});
    const std::optional<Decimal> gbp = fx::convert(r, Decimal{30790}, "huf", "gbp");
    ASSERT_TRUE(gbp.has_value());
    EXPECT_GE(*gbp, *eur * dec("0.83"));
    EXPECT_LE(*gbp, *eur * dec("0.84"));
    const std::optional<Decimal> pln = fx::convert(r, dec("12.5"), "PLN", "PLN");
    ASSERT_TRUE(pln.has_value());
    EXPECT_EQ(*pln, dec("12.5"));
    EXPECT_FALSE(fx::convert(r, Decimal{1}, "XXX", "EUR").has_value());
    // [D] exact System.Decimal digits.
    EXPECT_EQ(eur->to_string(), "84.05678405678405678405678406");
    EXPECT_EQ(gbp->to_string(), "70.217675129675129675129675132");
}

TEST(FxRates, Convert_follows_03_7_1_exactly)
{
    const FxRates& r = rates();
    // 03 §7.1 decimal examples (divide first, then multiply).
    EXPECT_EQ(fx::convert(r, Decimal{19290}, "HUF", "EUR")->to_string(), "52.661752661752661752661752662");
    EXPECT_EQ(fx::convert(r, Decimal{19290}, "HUF", "GBP")->to_string(), "43.991521703521703521703521704");
    EXPECT_EQ(fx::convert(r, dec("14.99"), "EUR", "HUF")->to_string(), "5490.837");
    // Equal codes return the amount unchanged — scale kept, unknown codes too, no rounding.
    EXPECT_EQ(fx::convert(r, dec("12.50"), "PLN", "pln")->to_string(), "12.50");
    EXPECT_EQ(fx::convert(r, dec("1.005"), "xxx", "XXX")->to_string(), "1.005");
    EXPECT_EQ(fx::convert(FxRates{}, dec("19290.5"), "HUF", "HUF")->to_string(), "19290.5");
    // Unknown target.
    EXPECT_FALSE(fx::convert(r, Decimal{1}, "EUR", "XYZ").has_value());
    EXPECT_FALSE(fx::convert(r, Decimal{1}, "", "EUR").has_value());
    // EUR is 1 before any table lookup, even when the table holds another EUR value.
    const FxRates odd = table({{"EUR", Decimal{2}}, {"HUF", dec("366.3")}});
    EXPECT_EQ(*fx::convert(odd, Decimal{1}, "EUR", "HUF"), dec("366.3"));
    EXPECT_EQ(*fx::convert(odd, dec("366.3"), "HUF", "eur"), Decimal{1});
    // A source rate <= 0 gives null; a target rate <= 0 is not guarded.
    const FxRates broken = table({{"ZZZ", Decimal{0}}, {"NEG", Decimal{-2}}, {"HUF", dec("366.3")}});
    EXPECT_FALSE(fx::convert(broken, Decimal{10}, "ZZZ", "HUF").has_value());
    EXPECT_FALSE(fx::convert(broken, Decimal{10}, "NEG", "HUF").has_value());
    EXPECT_EQ(*fx::convert(broken, Decimal{10}, "EUR", "ZZZ"), Decimal{0});
    EXPECT_EQ(*fx::convert(broken, Decimal{10}, "EUR", "NEG"), Decimal{-20});
    // Case-insensitive keys, also in a hand-built table with lower-case keys.
    const FxRates lower = table({{"huf", dec("366.3")}});
    EXPECT_EQ(*fx::convert(lower, Decimal{1}, "EUR", "HUF"), dec("366.3"));
}

TEST(FxRates, Supports_EUR_always_and_other_codes_case_insensitively)
{
    EXPECT_TRUE(fx::supports(FxRates{}, "EUR"));
    EXPECT_TRUE(fx::supports(FxRates{}, "eur"));
    EXPECT_FALSE(fx::supports(FxRates{}, "HUF"));
    EXPECT_FALSE(fx::supports(FxRates{}, ""));
    EXPECT_TRUE(fx::supports(rates(), "huf"));
    EXPECT_TRUE(fx::supports(rates(), "Gbp"));
    EXPECT_FALSE(fx::supports(rates(), "XYZ"));
    EXPECT_TRUE(fx::supports(table({{"aed", dec("4.1")}}), "AED"));
}

// ---------------------------------------------------------------------------
// CT-FX-04 — Fawaz currency-api
// ---------------------------------------------------------------------------

TEST(FxRates, CT_FX_04_Fawaz_format_is_upper_cased_and_crypto_free)
{
    const FxRates& r = fawaz();
    // [A]
    EXPECT_TRUE(fx::supports(r, "GEL"));
    EXPECT_TRUE(fx::supports(r, "huf"));
    for (const auto& [code, value] : r.per_eur) {
        EXPECT_EQ(wizz::text::utf16_length(code), 3u) << code;
        EXPECT_EQ(code, wizz::text::to_upper_invariant(code));
    }
    // [D]
    EXPECT_EQ(r.per_eur.size(), 288u);
    EXPECT_EQ(r.source, "currency-api");
    EXPECT_EQ(r.date, Date::from_ymd(2026, 11, 1));
    EXPECT_FALSE(r.is_approximate);
    for (const std::string& crypto : {"BTC", "ETH", "ADA", "SOL", "XRP", "BNB", "DOT", "LTC", "TRX"}) {
        EXPECT_TRUE(rate(r, crypto).has_value()) << crypto;
    }
    for (const std::string& dropped : {"1INCH", "AAVE", "ALGO"}) {
        EXPECT_FALSE(rate(r, dropped).has_value()) << dropped;
    }
    // 05 §2.5 samples; the "eur": 1 entry is kept (03 §7.1).
    EXPECT_EQ(rate(r, "HUF")->to_string(), "367.41835572");
    EXPECT_EQ(rate(r, "GBP")->to_string(), "0.83637627");
    EXPECT_EQ(rate(r, "GEL")->to_string(), "2.90748622");
    EXPECT_EQ(rate(r, "PLN")->to_string(), "4.42194664");
    EXPECT_EQ(rate(r, "EUR")->to_string(), "1");
    for (const std::string_view code : kWizzCurrencies) {
        EXPECT_TRUE(fx::supports(r, code)) << code;
    }
    // The service passes its source name.
    const FxRates named = fx::parse_fawaz(wizz::test::read_fixture("fx_fawaz.json"), "currency-api (jsDelivr)");
    EXPECT_EQ(named.source, "currency-api (jsDelivr)");
    EXPECT_EQ(named.per_eur, r.per_eur);
}

TEST(FxRates, CT_FX_04_N_Fawaz_keeps_three_letter_names_with_positive_numbers)
{
    // U+00FC (ü) is a letter: "ünk" is 3 UTF-16 units. U+1D400 (a letter) is a
    // surrogate pair, so "\U0001D400b" is 3 units of which two are not letters.
    const FxRates r = fx::parse_fawaz(
        "{\"date\":\"2026-11-01\",\"eur\":{\"aed\":4.11,\"a1b\":2,\"abcd\":3,\"zer\":0,\"neg\":-1,"
        "\"\xC3\xBCnk\":5,\"\xF0\x9D\x90\x80" "b\":6,\"1inch\":\"text\",\"meta\":{\"x\":1},"
        "\"Gbp\":0.8,\"gbp\":0.83637627,\"exp\":1.5e2}}");
    ASSERT_EQ(r.per_eur.size(), 4u);
    EXPECT_EQ(*rate(r, "AED"), dec("4.11"));
    EXPECT_EQ(rate(r, "GBP")->to_string(), "0.83637627"); // the later duplicate overwrites
    EXPECT_TRUE(rate(r, "\xC3\x9CNK").has_value());       // upper-cased with invariant rules
    EXPECT_EQ(*rate(r, "EXP"), Decimal{150});
    EXPECT_EQ(r.date, Date::from_ymd(2026, 11, 1));
}

TEST(FxRates, Fawaz_date_defaults_to_0001_01_01)
{
    EXPECT_EQ(fx::parse_fawaz(R"({"eur":{"huf":367.4}})").date, Date::min_value());
    EXPECT_EQ(fx::parse_fawaz(R"({"date":null,"eur":{"huf":367.4}})").date, Date::min_value());
    EXPECT_EQ(fx::parse_fawaz(R"({"date":"yesterday","eur":{"huf":367.4}})").date, Date::min_value());
    EXPECT_TRUE(fx::parse_fawaz(R"({"eur":{}})").per_eur.empty());
}

TEST(FxRates, D13_Fawaz_structural_problems_reject_the_whole_table)
{
    EXPECT_THROW((void)fx::parse_fawaz("[1,2,3]"), json::TypeError);
    EXPECT_EQ(message_of<wizz::FormatError>([] { (void)fx::parse_fawaz(R"({"date":"2026-11-01"})"); }),
              "currency-api: expected an object named 'eur'.");
    EXPECT_THROW((void)fx::parse_fawaz(R"({"eur":[1]})", "currency-api (Cloudflare)"), wizz::FormatError);
    EXPECT_THROW((void)fx::parse_fawaz(R"({"eur":null})"), wizz::FormatError);
    EXPECT_THROW((void)fx::parse_fawaz(R"({"eur":{"huf":"367.4"}})"), json::TypeError);
    EXPECT_THROW((void)fx::parse_fawaz(R"({"eur":{"huf":null}})"), json::TypeError);
    EXPECT_THROW((void)fx::parse_fawaz(R"({"eur":{"huf":1e30}})"), json::FormatError);
    EXPECT_THROW((void)fx::parse_fawaz(R"({"date":20261101,"eur":{"huf":367.4}})"), json::TypeError);
    EXPECT_THROW((void)fx::parse_fawaz("{\"eur\":"), json::ParseError);
}

// ---------------------------------------------------------------------------
// CT-LATENT-01 — a malformed primary table is not usable (parse part)
// ---------------------------------------------------------------------------

TEST(FxRates, CT_LATENT_01_Malformed_primary_rates_fall_through_to_currency_api)
{
    const std::string frankfurter = wizz::test::read_fixture("fx_frankfurter_v2.json");
    const std::string currency_api = wizz::test::read_fixture("fx_fawaz.json");
    // Run 1: one Frankfurter row lacks its quote currency.
    const std::string missing_quote = replace_first(frankfurter, R"("quote":"AED",)", "");
    EXPECT_THROW((void)fx::parse_frankfurter_v2(missing_quote), wizz::FormatError);
    // Run 2: the array holds a string instead of a row.
    const std::string string_row = replace_first(frankfurter, "[", R"(["oops",)");
    EXPECT_THROW((void)fx::parse_frankfurter_v2(string_row), json::TypeError);
    // The intact fixture still parses (the edits are what break it).
    EXPECT_EQ(fx::parse_frankfurter_v2(frankfurter).per_eur.size(), 170u);

    for (const std::string& malformed : {missing_quote, string_row}) {
        const std::optional<FxRates> used = first_usable({
            {"Frankfurter", malformed},
            {"currency-api (jsDelivr)", currency_api},
            {"currency-api (Cloudflare)", currency_api},
        });
        ASSERT_TRUE(used.has_value());
        EXPECT_TRUE(used->source.starts_with("currency-api"));
        EXPECT_EQ(used->source, "currency-api (jsDelivr)");
        EXPECT_EQ(used->date, Date::from_ymd(2026, 11, 1));
        EXPECT_EQ(used->per_eur.size(), 288u);
        EXPECT_EQ(rate(*used, "HUF")->to_string(), "367.41835572");
    }
}

// ---------------------------------------------------------------------------
// Disk cache fx.json (03 §7.2) and the frozen live copy (02 §14)
// ---------------------------------------------------------------------------

TEST(FxRates, Live_cache_fx_json_parses_170_codes_including_EUR)
{
    const std::string text = wizz::test::read_live_cache("fx.json");
    ASSERT_EQ(text.size(), 2297u);
    const FxRates live = fx::parse_rates_cache(text);
    EXPECT_EQ(live.per_eur.size(), 170u);
    ASSERT_TRUE(rate(live, "EUR").has_value());
    EXPECT_EQ(rate(live, "EUR")->to_string(), "1.0");
    EXPECT_EQ(live.date, Date::from_ymd(2026, 11, 2));
    EXPECT_EQ(live.source, "Frankfurter");
    EXPECT_FALSE(live.is_approximate);
    for (const std::string_view code : kWizzCurrencies) {
        EXPECT_TRUE(fx::supports(live, code)) << code;
    }
    EXPECT_EQ(rate(live, "HUF")->to_string(), "366.28");
    EXPECT_EQ(rate(live, "GBP")->to_string(), "0.83525");
    EXPECT_EQ(rate(live, "BTC")->to_string(), "0.000013745");
    // The writer reproduces the file byte for byte (sorted keys, exact decimal text, no newline).
    EXPECT_EQ(fx::serialize_rates_cache(live), text);
}

TEST(FxRates, Rates_cache_round_trips_with_exact_decimal_text)
{
    const std::string text = fx::serialize_rates_cache(rates());
    EXPECT_TRUE(text.starts_with(R"({"Date":"2026-11-02","Source":"Frankfurter","PerEur":{"AED":4.1058,)"));
    EXPECT_NE(text.find(R"("EUR":1.0,)"), std::string::npos);
    EXPECT_NE(text.find(R"("HUF":366.3,)"), std::string::npos);
    EXPECT_TRUE(text.ends_with("}}"));
    const FxRates back = fx::parse_rates_cache(text);
    EXPECT_EQ(back.per_eur, rates().per_eur);
    EXPECT_EQ(back.date, rates().date);
    EXPECT_EQ(back.source, "Frankfurter");
    EXPECT_FALSE(back.is_approximate);

    // The built-in table: approximate is not stored; BAM keeps all digits.
    const FxRates built_in = fx::parse_rates_cache(fx::serialize_rates_cache(*fx::built_in_rates()));
    EXPECT_EQ(built_in.source, "built-in table");
    EXPECT_FALSE(built_in.is_approximate);
    EXPECT_EQ(rate(built_in, "BAM")->to_string(), "1.95583");
    EXPECT_EQ(built_in.per_eur.size(), 34u);

    // A tiny value is written without an exponent.
    FxRates tiny = table({{"BTC", dec("1.3745e-5")}});
    tiny.date = Date::from_ymd(2026, 11, 2);
    tiny.source = "x";
    EXPECT_EQ(fx::serialize_rates_cache(tiny), R"({"Date":"2026-11-02","Source":"x","PerEur":{"BTC":0.000013745}})");
}

TEST(FxRates, Rates_cache_reader_is_case_sensitive_and_strict)
{
    const auto parse = [](std::string_view text) { return fx::parse_rates_cache(text); };
    // Source missing or null → "cache"; keys upper-cased; extra members ignored.
    const FxRates no_source = parse(R"({"Date":"2026-11-02","PerEur":{"huf":366.3,"EUR":1.0},"IsApproximate":true})");
    EXPECT_EQ(no_source.source, "cache");
    EXPECT_FALSE(no_source.is_approximate);
    EXPECT_EQ(no_source.per_eur.size(), 2u);
    EXPECT_EQ(*rate(no_source, "HUF"), dec("366.3"));
    EXPECT_EQ(parse(R"({"Date":"2026-11-02","Source":null,"PerEur":{"HUF":366.3}})").source, "cache");

    // Any of these means "no cache".
    EXPECT_EQ(message_of<wizz::FormatError>([&] { (void)parse("null"); }), "Empty FX cache.");
    EXPECT_EQ(message_of<wizz::FormatError>([&] { (void)parse(R"({"Source":"Frankfurter","PerEur":{"HUF":1}})"); }),
              "FX cache has no date.");
    EXPECT_EQ(message_of<wizz::FormatError>([&] { (void)parse(R"({"date":"2026-11-02","PerEur":{"HUF":1}})"); }),
              "FX cache has no date.");
    EXPECT_EQ(message_of<wizz::FormatError>([&] { (void)parse(R"({"Date":"2026-11-2","PerEur":{"HUF":1}})"); }),
              "FX cache date is not yyyy-MM-dd.");
    EXPECT_THROW((void)parse(R"({"Date":"11/02/2026","PerEur":{"HUF":1}})"), wizz::FormatError);
    EXPECT_THROW((void)parse(R"({"Date":"2026-11-02T00:00:00","PerEur":{"HUF":1}})"), wizz::FormatError);
    EXPECT_EQ(message_of<wizz::FormatError>([&] { (void)parse(R"({"Date":"2026-11-02","PerEur":null})"); }),
              "FX cache has no rates.");
    EXPECT_EQ(message_of<wizz::FormatError>([&] { (void)parse(R"({"Date":"2026-11-02","PerEur":{}})"); }),
              "FX cache has no rates.");
    EXPECT_EQ(message_of<wizz::FormatError>([&] { (void)parse(R"({"Date":"2026-11-02","perEur":{"HUF":1}})"); }),
              "FX cache has no rates.");
    EXPECT_THROW((void)parse(R"([{"Date":"2026-11-02"}])"), json::TypeError);
    EXPECT_THROW((void)parse(R"({"Date":20261102,"PerEur":{"HUF":1}})"), json::TypeError);
    EXPECT_THROW((void)parse(R"({"Date":"2026-11-02","Source":7,"PerEur":{"HUF":1}})"), json::TypeError);
    EXPECT_THROW((void)parse(R"({"Date":"2026-11-02","PerEur":[1]})"), json::TypeError);
    EXPECT_THROW((void)parse(R"({"Date":"2026-11-02","PerEur":{"HUF":"366.3"}})"), json::TypeError);
    EXPECT_THROW((void)parse(R"({"Date":"2026-11-02","PerEur":{"HUF":1e30}})"), json::FormatError);
    EXPECT_THROW((void)parse(R"({"Date":"2026-11-02",)"), json::ParseError);
    EXPECT_THROW((void)parse(""), json::ParseError);
}

} // namespace
