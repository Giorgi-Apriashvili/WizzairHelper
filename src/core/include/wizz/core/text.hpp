#pragma once
// Unicode text rules with .NET semantics (02 §4, §5.3, §5.7, 03 §0, 07 §1).
//
// Strings are UTF-8 everywhere; the UTF-16 overloads exist because the spec
// counts lengths and indexes characters in UTF-16 code units (02 §4.1, §5.7,
// §5.8). Character properties come from the Windows ICU (icu.dll), which is
// what .NET uses on Windows 10+; the white-space set is the fixed list of 02 §4.1.
//
// A default-constructed (empty) string view plays the role of a .NET `null`
// string wherever the spec distinguishes it (CT-TXT-07).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wizz::text {

/// U+FFFD, substituted for every ill-formed UTF-8 subsequence or lone surrogate.
inline constexpr char32_t kReplacementCharacter = U'�';

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

/// UTF-8 → UTF-16. Each maximal ill-formed subsequence becomes one U+FFFD
/// (Unicode "best practice", as .NET's UTF8Encoding decodes).
[[nodiscard]] std::u16string utf8_to_utf16(std::string_view utf8);
/// UTF-16 → UTF-8. A lone surrogate becomes U+FFFD.
[[nodiscard]] std::string utf16_to_utf8(std::u16string_view utf16);
/// Number of UTF-16 code units `utf8_to_utf16(utf8)` would produce (.NET `string.Length`).
[[nodiscard]] std::size_t utf16_length(std::string_view utf8) noexcept;

// ---------------------------------------------------------------------------
// Character classification (.NET System.Char / CharUnicodeInfo)
// ---------------------------------------------------------------------------

/// .NET `UnicodeCategory`, same order and numeric values.
enum class UnicodeCategory : std::uint8_t {
    uppercase_letter,          // Lu
    lowercase_letter,          // Ll
    titlecase_letter,          // Lt
    modifier_letter,           // Lm
    other_letter,              // Lo
    nonspacing_mark,           // Mn
    spacing_combining_mark,    // Mc
    enclosing_mark,            // Me
    decimal_digit_number,      // Nd
    letter_number,             // Nl
    other_number,              // No
    space_separator,           // Zs
    line_separator,            // Zl
    paragraph_separator,       // Zp
    control,                   // Cc
    format,                    // Cf
    surrogate,                 // Cs
    private_use,               // Co
    connector_punctuation,     // Pc
    dash_punctuation,          // Pd
    open_punctuation,          // Ps
    close_punctuation,         // Pe
    initial_quote_punctuation, // Pi
    final_quote_punctuation,   // Pf
    other_punctuation,         // Po
    math_symbol,               // Sm
    currency_symbol,           // Sc
    modifier_symbol,           // Sk
    other_symbol,              // So
    other_not_assigned,        // Cn
};

/// General category of a code point. A surrogate code unit (U+D800..U+DFFF)
/// passed on its own is `surrogate`, as for .NET `char`.
[[nodiscard]] UnicodeCategory general_category(char32_t c) noexcept;

/// .NET `char.IsWhiteSpace`: exactly U+0009–U+000D, U+0020, U+0085, U+00A0,
/// U+1680, U+2000–U+200A, U+2028, U+2029, U+202F, U+205F, U+3000 (02 §4.1; the
/// Unicode White_Space property). U+200B is not white space.
[[nodiscard]] constexpr bool is_white_space(char32_t c) noexcept
{
    return (c >= 0x0009 && c <= 0x000D) || c == 0x0020 || c == 0x0085 || c == 0x00A0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

/// .NET `char.IsLetter`: categories Lu, Ll, Lt, Lm, Lo.
[[nodiscard]] bool is_letter(char32_t c) noexcept;
/// .NET `char.IsDigit`: category Nd.
[[nodiscard]] bool is_digit(char32_t c) noexcept;
/// .NET `char.IsLetterOrDigit`: L* or Nd (ICU `u_isalnum`, 02 §5.7).
[[nodiscard]] bool is_letter_or_digit(char32_t c) noexcept;

// ---------------------------------------------------------------------------
// Trimming (.NET string.Trim / TrimStart / TrimEnd without arguments)
// ---------------------------------------------------------------------------

/// Removes `is_white_space` characters at both ends. Returns a view into `s`.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept;
[[nodiscard]] std::string_view trim_start(std::string_view s) noexcept;
[[nodiscard]] std::string_view trim_end(std::string_view s) noexcept;
[[nodiscard]] std::u16string_view trim(std::u16string_view s) noexcept;
[[nodiscard]] std::u16string_view trim_start(std::u16string_view s) noexcept;
[[nodiscard]] std::u16string_view trim_end(std::u16string_view s) noexcept;

/// .NET `string.IsNullOrWhiteSpace` (empty, or white space only).
[[nodiscard]] bool is_null_or_white_space(std::string_view s) noexcept;
[[nodiscard]] bool is_null_or_white_space(std::u16string_view s) noexcept;

// ---------------------------------------------------------------------------
// Invariant casing (.NET-on-ICU ToUpperInvariant / ToLowerInvariant)
// ---------------------------------------------------------------------------
// Simple (1:1) Unicode case mapping, so lengths never change ("ß" stays "ß").
// Like .NET on Windows, U+0131 (ı) upper-cases to itself and U+0130 (İ)
// lower-cases to itself under the invariant culture.

/// `char.ToUpperInvariant`: one UTF-16 code unit; surrogates are returned unchanged.
[[nodiscard]] char16_t to_upper_invariant(char16_t c) noexcept;
/// `char.ToLowerInvariant`: one UTF-16 code unit; surrogates are returned unchanged.
[[nodiscard]] char16_t to_lower_invariant(char16_t c) noexcept;
/// `string.ToUpperInvariant`: maps every code point, including supplementary ones.
[[nodiscard]] std::string to_upper_invariant(std::string_view s);
/// `string.ToLowerInvariant`: maps every code point, including supplementary ones.
[[nodiscard]] std::string to_lower_invariant(std::string_view s);

// ---------------------------------------------------------------------------
// Ordinal, case-insensitive comparison (.NET StringComparison.OrdinalIgnoreCase)
// ---------------------------------------------------------------------------
// Used for every IATA / MAC / currency code key (03 §0 `ci`, 07 §1). Both sides
// are upper-cased code point by code point with the simple mapping (ı and ſ
// excepted, as in .NET's OrdinalCasing), so "Kraków" equals "KRAKÓW".

[[nodiscard]] bool ordinal_ignore_case_equals(std::string_view a, std::string_view b) noexcept;
/// Three-way comparison in .NET OrdinalIgnoreCase order (upper-cased code
/// points; a proper prefix sorts first): <0, 0, >0.
[[nodiscard]] int ordinal_ignore_case_compare(std::string_view a, std::string_view b) noexcept;

/// Hash consistent with `ordinal_ignore_case_equals` (transparent: accepts any string view).
struct OrdinalIgnoreCaseHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept;
};

/// Equality predicate for unordered containers keyed by codes.
struct OrdinalIgnoreCaseEqual {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept
    {
        return ordinal_ignore_case_equals(a, b);
    }
};

/// Strict weak ordering for ordered containers keyed by codes.
struct OrdinalIgnoreCaseLess {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept
    {
        return ordinal_ignore_case_compare(a, b) < 0;
    }
};

// ---------------------------------------------------------------------------
// TextNormalizer.Key (02 §4)
// ---------------------------------------------------------------------------

/// Accent-, case- and white-space-insensitive name key, the exact 02 §4.1
/// algorithm: trim; NFD; per UTF-16 unit drop Mn, lower-case (invariant,
/// simple), collapse white-space runs to one ' ' (never leading), map
/// ł đ ø ß æ œ ı þ → l d o ss ae oe i th; trim the end. Null / white space → "".
/// Keys are compared ordinally; their lengths are UTF-16 code-unit counts.
[[nodiscard]] std::u16string normalize_key(std::u16string_view text);
/// UTF-8 convenience overload of `normalize_key`.
[[nodiscard]] std::string normalize_key(std::string_view text);

} // namespace wizz::text
