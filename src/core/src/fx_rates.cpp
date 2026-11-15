// FxRates conversion, provider parsers, disk-cache shape and built-in table
// (03 §7.1, §7.2; DECISIONS D-13).

#include "wizz/core/fx_rates.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace wizz::fx_rates {

namespace {

constexpr std::string_view kEur = "EUR";

/// The rate stored for an already upper-cased code: the exact key first, then a
/// case-insensitive scan (PerEur has OrdinalIgnoreCase keys in 03 §7.1).
[[nodiscard]] const Decimal* find_rate(const FxRates& rates, const std::string& upper_code)
{
    if (const auto it = rates.per_eur.find(upper_code); it != rates.per_eur.end()) {
        return &it->second;
    }
    for (const auto& [code, rate] : rates.per_eur) {
        if (text::ordinal_ignore_case_equals(code, upper_code)) {
            return &rate;
        }
    }
    return nullptr;
}

/// TryRate of 03 §7.1: 1 for EUR (before any table lookup), else the table entry.
[[nodiscard]] std::optional<Decimal> rate_of(const FxRates& rates, const std::string& upper_code)
{
    if (upper_code == kEur) {
        return Decimal{1};
    }
    if (const Decimal* rate = find_rate(rates, upper_code)) {
        return *rate;
    }
    return std::nullopt;
}

/// JsonElement.GetDecimal: json::TypeError unless a number; json::FormatError
/// when the lexeme does not fit a decimal (> ~7.9e28).
[[nodiscard]] Decimal decimal_of(const json::Value& value)
{
    const std::expected<Decimal, DecimalParseError> parsed = Decimal::from_json_number(value.number_lexeme());
    if (!parsed) {
        throw json::FormatError("The JSON value is not in a supported Decimal format.");
    }
    return *parsed;
}

/// A provider row date "parsed invariantly": the date part of any D-22 form
/// (`yyyy-MM-dd`, an ISO date-time, `MM/dd/yyyy`); nullopt otherwise.
[[nodiscard]] std::optional<Date> parse_row_date(std::string_view text)
{
    if (const std::optional<LocalDateTime> parsed = parse_local_datetime(text)) {
        return date_of(*parsed);
    }
    return std::nullopt;
}

/// The member's string when it is present and not null; json::TypeError for any
/// other kind (GetString on a number or object); nullopt when absent or null.
[[nodiscard]] std::optional<std::string_view> optional_string(const json::Value& object, std::string_view name)
{
    const json::Value* member = object.find(name);
    if (member == nullptr || member->is_null()) {
        return std::nullopt;
    }
    return member->as_string();
}

/// Fawaz key filter (03 §7.2): exactly 3 UTF-16 code units, each char.IsLetter.
/// A surrogate unit is never a letter, so a supplementary character fails.
[[nodiscard]] bool is_three_letter_code(std::string_view name)
{
    const std::u16string units = text::utf8_to_utf16(name);
    return units.size() == 3 &&
           std::ranges::all_of(units, [](char16_t unit) { return text::is_letter(static_cast<char32_t>(unit)); });
}

/// One built-in table entry (03 §7.2), parsed from its literal text.
struct BuiltInEntry {
    std::string_view code;
    std::string_view per_eur;
};

constexpr std::array<BuiltInEntry, 34> kBuiltInTable{{
    {"AED", "4.1058"},  {"ALL", "90.85"},  {"BAM", "1.95583"}, {"CHF", "0.92121"}, {"CZK", "24.355"},
    {"DKK", "7.4795"},  {"GBP", "0.83536"}, {"GEL", "2.8972"}, {"HUF", "366.3"},   {"MDL", "20.107"},
    {"MKD", "61.685"},  {"NOK", "10.3204"}, {"PLN", "4.4032"}, {"RON", "5.346"},   {"RSD", "117.59"},
    {"SEK", "10.9955"}, {"USD", "1.118"},   {"UAH", "50.774"}, {"RUB", "90.1"},    {"TRY", "55.814"},
    {"ISK", "136.01"},  {"ILS", "3.4933"},  {"SAR", "4.1925"}, {"AZN", "1.899"},   {"AMD", "402.42"},
    {"EGP", "62.601"},  {"JOD", "0.79266"}, {"MAD", "10.9867"}, {"KZT", "507.37"}, {"UZS", "13399"},
    {"CAD", "1.5627"},  {"AUD", "1.5913"},  {"JPY", "176.21"}, {"CNY", "7.4388"},
}};

[[nodiscard]] FxRatesPtr make_built_in_rates()
{
    FxRates rates;
    for (const BuiltInEntry& entry : kBuiltInTable) {
        const std::optional<Decimal> value = Decimal::parse(entry.per_eur);
        if (!value) {
            throw std::logic_error("built-in FX table literal does not parse");
        }
        rates.per_eur.emplace(std::string{entry.code}, *value);
    }
    rates.date = Date::from_ymd(2026, 11, 2);
    rates.source = std::string{kBuiltInSource};
    rates.is_approximate = true;
    return std::make_shared<const FxRates>(std::move(rates));
}

} // namespace

bool supports(const FxRates& rates, std::string_view code)
{
    if (text::ordinal_ignore_case_equals(code, kEur)) {
        return true;
    }
    return find_rate(rates, text::to_upper_invariant(code)) != nullptr;
}

std::optional<Decimal> convert(const FxRates& rates, const Decimal& amount, std::string_view from, std::string_view to)
{
    const std::string from_code = text::to_upper_invariant(from);
    const std::string to_code = text::to_upper_invariant(to);
    if (from_code == to_code) {
        return amount;
    }
    const std::optional<Decimal> rate_from = rate_of(rates, from_code);
    if (!rate_from) {
        return std::nullopt;
    }
    const std::optional<Decimal> rate_to = rate_of(rates, to_code);
    if (!rate_to) {
        return std::nullopt;
    }
    if (rate_from->signum() <= 0) {
        return std::nullopt;
    }
    return amount / *rate_from * *rate_to;
}

FxRates parse_frankfurter_v2(std::string_view json_text)
{
    const json::Value root = json::parse(json_text);
    if (!root.is_array()) {
        throw FormatError("Frankfurter v2: expected an array.");
    }
    FxRates rates;
    rates.source = std::string{kFrankfurterSource};
    Date latest = Date::min_value();
    for (const json::Value& row : root.as_array()) {
        // find() on a non-object element throws json::TypeError (D-13).
        const std::optional<std::string_view> base = optional_string(row, "base");
        if (!base || !text::ordinal_ignore_case_equals(*base, kEur)) {
            continue;
        }
        const json::Value* quote = row.find("quote");
        if (quote == nullptr) {
            throw FormatError("Frankfurter v2: a rate row has no 'quote'.");
        }
        const std::string_view quote_code = quote->as_string();
        const json::Value* rate = row.find("rate");
        if (rate == nullptr) {
            throw FormatError("Frankfurter v2: a rate row has no 'rate'.");
        }
        const Decimal per_eur = decimal_of(*rate);
        if (quote_code.empty() || per_eur.signum() <= 0) {
            continue;
        }
        rates.per_eur.insert_or_assign(text::to_upper_invariant(quote_code), per_eur);
        if (const std::optional<std::string_view> date_text = optional_string(row, "date")) {
            if (const std::optional<Date> date = parse_row_date(*date_text); date && *date > latest) {
                latest = *date;
            }
        }
    }
    rates.date = latest;
    return rates;
}

FxRates parse_fawaz(std::string_view json_text, std::string_view source_name)
{
    const json::Value root = json::parse(json_text);
    const json::Value* eur = root.find("eur"); // json::TypeError unless the root is an object
    if (eur == nullptr || !eur->is_object()) {
        throw FormatError(std::format("{}: expected an object named 'eur'.", source_name));
    }
    FxRates rates;
    rates.source = std::string{source_name};
    if (const std::optional<std::string_view> date_text = optional_string(root, "date")) {
        if (const std::optional<Date> date = parse_row_date(*date_text)) {
            rates.date = *date;
        }
    }
    for (const json::Member& member : eur->as_object()) {
        if (!is_three_letter_code(member.name())) {
            continue;
        }
        const Decimal per_eur = decimal_of(member.value());
        if (per_eur.signum() <= 0) {
            continue;
        }
        rates.per_eur.insert_or_assign(text::to_upper_invariant(member.name()), per_eur);
    }
    return rates;
}

FxRatesPtr built_in_rates()
{
    static const FxRatesPtr instance = make_built_in_rates();
    return instance;
}

std::string serialize_rates_cache(const FxRates& rates)
{
    json::Writer writer;
    writer.begin_object();
    writer.key("Date").string(rates.date.iso());
    writer.key("Source").string(rates.source);
    writer.key("PerEur").begin_object();
    for (const auto& [code, per_eur] : rates.per_eur) {
        writer.key(code).number_lexeme(per_eur.to_string());
    }
    writer.end_object();
    writer.end_object();
    return writer.take();
}

FxRates parse_rates_cache(std::string_view json_text)
{
    const json::Value root = json::parse(json_text);
    if (root.is_null()) {
        throw FormatError("Empty FX cache.");
    }
    const std::optional<std::string_view> date_text = optional_string(root, "Date");
    if (!date_text) {
        throw FormatError("FX cache has no date.");
    }
    const std::optional<Date> date = Date::parse_iso(*date_text);
    if (!date) {
        throw FormatError("FX cache date is not yyyy-MM-dd.");
    }
    const json::Value* per_eur = root.find("PerEur");
    if (per_eur == nullptr || per_eur->is_null()) {
        throw FormatError("FX cache has no rates.");
    }
    FxRates rates;
    for (const json::Member& member : per_eur->as_object()) {
        rates.per_eur.insert_or_assign(text::to_upper_invariant(member.name()), decimal_of(member.value()));
    }
    if (rates.per_eur.empty()) {
        throw FormatError("FX cache has no rates.");
    }
    const std::optional<std::string_view> source = optional_string(root, "Source");
    rates.source = source ? std::string{*source} : std::string{kCacheSourceFallback};
    rates.date = *date;
    rates.is_approximate = false;
    return rates;
}

} // namespace wizz::fx_rates
