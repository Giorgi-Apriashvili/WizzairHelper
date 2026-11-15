#pragma once
// Exchange rates: EUR-pivot conversion (03 §7.1), the provider parsers, the
// `fx.json` disk-cache shape and the built-in fallback table (03 §7.2), with the
// table-level validation of DECISIONS D-13 / 07 §4 (CT-LATENT-01).
//
// Pure functions over the `FxRates` value of domain.hpp. The network service
// (source order, notices, memory and disk caching) lives in the services library
// and calls these. Money never passes through double: every rate is parsed from
// its JSON number lexeme into a `Decimal` (06 §3).
//
// Error contract of the parsers — the service treats every one of them as
// "this source is unusable, try the next one" (03 §7.2 catch list):
//   - json::ParseError   malformed JSON text (JsonException);
//   - json::TypeError    a value of the wrong JSON kind (InvalidOperationException),
//                        e.g. an array element that is not an object;
//   - json::FormatError  a number that does not fit a decimal (FormatException);
//   - wizz::FormatError  a structural problem: wrong root shape, a missing
//                        required member (KeyNotFoundException), an empty cache.
// Catching `json::Error` and `wizz::FormatError` covers all of them.

#include "wizz/core/decimal.hpp"
#include "wizz/core/domain.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace wizz::fx_rates {

/// `Source` of a Frankfurter v2 table (03 §7.2 Sources #1).
inline constexpr std::string_view kFrankfurterSource = "Frankfurter";
/// Default `Source` of `parse_fawaz` when no name is passed (CT-FX-04 [D]).
inline constexpr std::string_view kDefaultFawazSource = "currency-api";
/// `Source` of the built-in table (03 §7.2, CT-FX-02).
inline constexpr std::string_view kBuiltInSource = "built-in table";
/// `Source` of a disk cache that has no `Source` member (03 §7.2).
inline constexpr std::string_view kCacheSourceFallback = "cache";

/// FxRates.Supports (03 §7.1): `code` equals "EUR" (OrdinalIgnoreCase), or the
/// table holds upper(code). An `EUR` key in the table is kept and harmless.
[[nodiscard]] bool supports(const FxRates& rates, std::string_view code);

/// FxRates.Convert (03 §7.1), exactly:
/// - both codes are upper-cased (ToUpperInvariant);
/// - equal codes return `amount` unchanged, even for an unknown code, without rounding;
/// - rate(EUR) = 1, any other missing code gives nullopt;
/// - a source rate <= 0 gives nullopt (a target rate <= 0 is not guarded);
/// - otherwise `amount / rate(from) * rate(to)` in System.Decimal arithmetic,
///   dividing FIRST (19290 / 366.3 = 52.661752661752661752661752662).
/// Rate lookups are case-insensitive, also for hand-built tables with lower-case
/// keys. Decimal overflow (std::overflow_error) propagates, as in .NET.
[[nodiscard]] std::optional<Decimal> convert(const FxRates& rates, const Decimal& amount, std::string_view from,
                                             std::string_view to);

/// ParseFrankfurterV2 (03 §7.2) of `[{"date":"2026-11-02","base":"EUR","quote":"GEL","rate":2.8972}, ...]`.
/// Result: Source "Frankfurter", Date = the latest row date that parses
/// (0001-01-01 if none), not approximate.
/// Whole-table rejections (D-13), checked per row in this order:
/// - root not an array → FormatError("Frankfurter v2: expected an array.");
/// - an element that is not an object → json::TypeError;
/// - `base` of another JSON kind than string or null → json::TypeError;
/// - (rows whose `base` is missing, null or not "EUR" (OrdinalIgnoreCase) are skipped here);
/// - `quote` missing → FormatError; not a string (null included) → json::TypeError;
/// - `rate` missing → FormatError; not a number → json::TypeError; too large for a
///   decimal → json::FormatError;
/// - (rows with an empty `quote` or a `rate` <= 0 are skipped here);
/// - `date` of a kept row of another JSON kind than string or null → json::TypeError.
/// Row-level skips: `base` absent / null / not EUR, an empty `quote`, `rate` <= 0.
/// Kept rows store `PerEur[upper(quote)] = rate` (a later duplicate overwrites)
/// and only they advance the date; a missing, null or unparsable `date` (D-22
/// forms, date part taken) is ignored. The `EUR` row (rate 1.0) is kept like any other.
[[nodiscard]] FxRates parse_frankfurter_v2(std::string_view json);

/// ParseFawaz (03 §7.2) of `{"date":"2026-11-01","eur":{"aed":4.11,...}}`.
/// Result: Source `source_name`, Date = `date` (0001-01-01 when missing, null or
/// unparsable), not approximate.
/// Whole-table rejections (D-13): root not an object → json::TypeError; `eur`
/// missing or not an object → FormatError; `date` neither string nor null →
/// json::TypeError; for a property whose name is a code (below), a value that is
/// not a number → json::TypeError and a number too large for a decimal →
/// json::FormatError.
/// Row-level skips: a name that is not exactly 3 UTF-16 code units which all
/// satisfy char.IsLetter (crypto codes such as "btc" are kept, "1inch" or "aave"
/// are skipped whatever their value), and a value <= 0. Kept properties store
/// `PerEur[upper(name)] = value`; a later duplicate overwrites.
[[nodiscard]] FxRates parse_fawaz(std::string_view json, std::string_view source_name = kDefaultFawazSource);

/// CurrencyService.BuiltIn (03 §7.2): 34 codes (EUR implicit), Date 2026-11-02,
/// Source "built-in table", approximate. Every call returns the SAME instance
/// (CT-FX-05 `Assert.Same`); it is immutable and safe to share across threads.
[[nodiscard]] FxRatesPtr built_in_rates();

/// The `fx.json` disk-cache text (03 §7.2, System.Text.Json defaults): compact
/// `{"Date":"yyyy-MM-dd","Source":...,"PerEur":{"AED":4.1061,...}}`. Each rate is
/// written from its exact decimal text, so `"EUR":1.0` keeps its scale.
/// `is_approximate` is not stored. Keys come out in ordinal order (the order of
/// `FxRates::per_eur`); JSON object order is not significant on read.
[[nodiscard]] std::string serialize_rates_cache(const FxRates& rates);

/// Reads an `fx.json` disk cache (03 §7.2); member names are case-sensitive.
/// - the literal `null` → FormatError("Empty FX cache.");
/// - root not an object, `Date`/`Source` not strings, `PerEur` not an object or a
///   rate that is not a number → json::TypeError; a rate too large → json::FormatError;
/// - `Date` missing or null → FormatError("FX cache has no date."); not exactly
///   `yyyy-MM-dd` → FormatError("FX cache date is not yyyy-MM-dd.");
/// - `PerEur` missing, null or empty → FormatError("FX cache has no rates.");
/// - `Source` missing or null → "cache".
/// Keys are upper-cased (the table is case-insensitive); the result is never
/// approximate (the caller flags a stale copy). Any of these errors means "no cache".
[[nodiscard]] FxRates parse_rates_cache(std::string_view json);

} // namespace wizz::fx_rates
