#pragma once
// The pure, stateless parts of the Wizz Air client (04 §7.3, §7.4, §9.3, §9.4,
// §11.5; protocol-reference §3, §5): finding the API base URL in a wizzair.com
// page, validating the cached `api-url.txt`, building the timetableV2 request
// body, reading timetableV2 rows and validation codes, and the small text
// helpers the response classifier needs. No I/O and no shared state: every
// function is safe to call concurrently.

#include "wizz/core/domain.hpp"
#include "wizz/core/time.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wizz::protocol {

/// `System.Version` as `ExtractApiUrl`'s fallback uses it (04 §7.3).
///
/// Two to four '.'-separated components, each parsed like
/// `int.TryParse(s, NumberStyles.Integer, InvariantCulture)` and required to be
/// non-negative: optional ASCII white space around the digits, an optional
/// leading '+' or '-' ("-0" is 0), ASCII digits only, leading zeros allowed
/// ("01.2" is 1.2), Int32 range. Comparison is component-wise (Major, Minor,
/// Build, Revision) with an absent component counting as -1, so 29.10 < 29.10.0.
class SystemVersion {
public:
    /// `Version.TryParse(text)`: nullopt for fewer than 2 or more than 4
    /// components, an empty or non-numeric component, a negative value or one
    /// above 2147483647.
    [[nodiscard]] static std::optional<SystemVersion> try_parse(std::string_view text) noexcept;

    [[nodiscard]] constexpr std::int32_t major() const noexcept { return parts_[0]; }
    [[nodiscard]] constexpr std::int32_t minor() const noexcept { return parts_[1]; }
    /// -1 when absent.
    [[nodiscard]] constexpr std::int32_t build() const noexcept { return parts_[2]; }
    /// -1 when absent.
    [[nodiscard]] constexpr std::int32_t revision() const noexcept { return parts_[3]; }
    /// Number of components present (2..4).
    [[nodiscard]] constexpr int component_count() const noexcept
    {
        return parts_[2] < 0 ? 2 : parts_[3] < 0 ? 3 : 4;
    }

    /// `Version.ToString()`: the present components without leading zeros ("29.1.0").
    [[nodiscard]] std::string to_string() const;

    friend constexpr bool operator==(const SystemVersion&, const SystemVersion&) noexcept = default;
    /// `Version.CompareTo`: lexicographic over (Major, Minor, Build, Revision), absent = -1.
    friend constexpr std::strong_ordering operator<=>(const SystemVersion&, const SystemVersion&) noexcept = default;

private:
    constexpr explicit SystemVersion(std::array<std::int32_t, 4> parts) noexcept : parts_(parts) {}

    std::array<std::int32_t, 4> parts_{-1, -1, -1, -1};
};

/// `ExtractApiUrl(html)` (04 §7.3, protocol-reference §3): the Wizz API base URL
/// advertised by a wizzair.com page, or nullopt.
///
/// A linear hand scanner equivalent to the two case-sensitive .NET regexes
/// (each is deterministic, so a greedy scan is exact):
/// - primary `apiUrl\\?"?\s*:\s*\\?"(https:(?:\\?/){2}be\.wizzair\.com(?:\\?/)\d+(?:\.\d+)+(?:\\?/)Api)`:
///   the first match in document order wins and its group with every two-character
///   `\/` turned into `/` is returned; the version is not validated;
/// - fallback, only without a primary match,
///   `be\.wizzair\.com(?:\\?/)(\d+(?:\.\d+)+)(?:\\?/)Api`: of all non-overlapping
///   matches whose group passes `SystemVersion::try_parse`, the highest version
///   (the first on ties) is returned as `https://be.wizzair.com/<group text>/Api`
///   — always https, the original text kept ("29.01.0").
///
/// As in .NET, `\s` is `char.IsWhiteSpace` and `\d` any BMP decimal digit (Nd);
/// a supplementary-plane digit is a surrogate pair in .NET and never matches.
/// There is no boundary after `Api` (".../ApiX" still matches).
[[nodiscard]] std::optional<std::string> extract_api_url(std::string_view html);

/// `ApiUrlRegexStrict` (04 §7.4, DECISIONS D-15): true when `trimmed` matches
/// `^https://be\.wizzair\.com/\d+(\.\d+)+/Api$` in full, with ASCII digits.
/// Callers pass the contents of `api-url.txt` after `text::trim` (Unicode white
/// space); this function itself trims nothing.
[[nodiscard]] bool is_valid_cached_api_url(std::string_view trimmed) noexcept;

/// `BuildTimetableBody(from, to, start, end, priceType, adults)` (04 §9.3): the exact
/// compact JSON of a timetableV2 request with one `flightList` item:
/// `{"flightList":[{"departureStation":"<FROM>","arrivalStation":"<TO>","from":"yyyy-MM-dd","to":"yyyy-MM-dd"}],"priceType":"<regular|wdc>","adultCount":<n>,"childCount":0,"infantCount":0}`.
/// Codes are `ToUpperInvariant` and not trimmed; strings are escaped like
/// `JavaScriptEncoder.Default` ("bü<d" → `"BÜ<D"`); `adultCount` is
/// max(1, adults).
[[nodiscard]] std::string build_timetable_body(std::string_view from, std::string_view to, Date start, Date end,
                                               PriceType price_type, int adults);

/// `ParseTimetable(json)` (04 §9.4, protocol-reference §5): one `DayFare` per usable
/// `outboundFlights` row, in response order (neither sorted nor de-duplicated).
///
/// - A missing, null or non-array `outboundFlights` gives an empty list.
/// - Rows are skipped when `departureStation`/`arrivalStation` is not a JSON
///   string, when `departureDate` is not a string `parse_local_datetime` accepts,
///   or when the row is neither `priceType` "checkPrice" (OrdinalIgnoreCase,
///   → check_on_site with amount 0) nor priced above 0 (→ available, whatever its
///   priceType). The amount is read exactly from `price.amount` when that is a
///   JSON number; the currency from `price.currencyCode` (else "").
/// - `departureDates` keeps legacy V1 strings and V2 objects with a parsable
///   `date`, sorted ascending with duplicates kept. When `departureDate` is exactly
///   midnight and departures exist, the cheapest departure is the last entry whose
///   `isCheapestOfTheDay` is literally true, else the earliest departure.
/// - Codes and currency are `ToUpperInvariant`, never trimmed; `has_mac_flight`
///   is true only for the literal `true`.
///
/// Throws json::ParseError for text that is not JSON, json::TypeError when the
/// root or an `outboundFlights` element is not an object, and json::FormatError
/// ("One of the identified items was in an invalid format.", the .NET
/// `GetDecimal` text) when an amount does not fit a decimal (e.g. 1e40). The
/// client wraps all three as "Wizz Air sent a timetable that could not be read: …" (04 §9.2).
[[nodiscard]] std::vector<DayFare> parse_timetable(std::string_view body);

/// `ParseValidationCodes(json)` (04 §11.5): the string elements, in order and empty
/// strings included, of a root object's `validationCodes` array. Text that is not
/// JSON, a non-object root, or a missing / non-array member gives an empty list.
[[nodiscard]] std::vector<std::string> parse_validation_codes(std::string_view body);

/// `LooksLikeJson(body)` (04 §11.3, §11.5): after `TrimStart` (Unicode white space,
/// not U+FEFF) the first character is '{' or '['.
[[nodiscard]] bool looks_like_json(std::string_view body) noexcept;

/// Longest error-text excerpt, in UTF-16 code units (04 §11.5).
inline constexpr std::size_t kSnippetLength = 200;

/// `Snippet(body)` (04 §11.5): `body` unchanged when it has at most 200 UTF-16 code
/// units; otherwise its first 200 code units followed by "…" (U+2026), cut back to
/// 199 when the 200th unit would split a surrogate pair.
[[nodiscard]] std::string snippet(std::string_view body);

} // namespace wizz::protocol
