#include "wizz/core/text.hpp"

#include <icu.h>

#include <stdexcept>
#include <string>

namespace wizz::text {
namespace {

// ---------------------------------------------------------------------------
// UTF-8 / UTF-16 primitives
// ---------------------------------------------------------------------------

struct Decoded {
    char32_t code_point;
    std::size_t length; // bytes consumed (≥ 1)
};

/// Decodes one code point at `s[i]` (i < s.size()). An ill-formed sequence
/// yields U+FFFD and consumes its maximal subpart (Unicode Table 3-7).
[[nodiscard]] constexpr Decoded decode_utf8(std::string_view s, std::size_t i) noexcept
{
    const auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) {
        return {lead, 1};
    }
    std::size_t trailing = 0;
    char32_t code_point = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        trailing = 1;
        code_point = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        trailing = 2;
        code_point = lead & 0x0Fu;
        low = lead == 0xE0 ? 0xA0 : 0x80;  // no overlongs
        high = lead == 0xED ? 0x9F : 0xBF; // no surrogates
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        trailing = 3;
        code_point = lead & 0x07u;
        low = lead == 0xF0 ? 0x90 : 0x80;  // no overlongs
        high = lead == 0xF4 ? 0x8F : 0xBF; // ≤ U+10FFFF
    } else {
        return {kReplacementCharacter, 1};
    }
    std::size_t length = 1;
    for (std::size_t k = 0; k < trailing; ++k) {
        if (i + length >= s.size()) {
            return {kReplacementCharacter, length};
        }
        const auto byte = static_cast<unsigned char>(s[i + length]);
        if (byte < low || byte > high) {
            return {kReplacementCharacter, length};
        }
        code_point = (code_point << 6) | (byte & 0x3Fu);
        ++length;
        low = 0x80;
        high = 0xBF;
    }
    return {code_point, length};
}

/// Calls `fn(code_point)` for every code point of `s` (ill-formed parts → U+FFFD).
template <typename Fn>
constexpr void for_each_code_point(std::string_view s, Fn&& fn)
{
    for (std::size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(s, i);
        fn(d.code_point);
        i += d.length;
    }
}

[[nodiscard]] constexpr bool is_surrogate(char32_t c) noexcept { return c >= 0xD800 && c <= 0xDFFF; }
[[nodiscard]] constexpr bool is_high_surrogate(char32_t c) noexcept { return c >= 0xD800 && c <= 0xDBFF; }
[[nodiscard]] constexpr bool is_low_surrogate(char32_t c) noexcept { return c >= 0xDC00 && c <= 0xDFFF; }

void append_utf8(std::string& out, char32_t c)
{
    if (is_surrogate(c) || c > 0x10FFFF) {
        c = kReplacementCharacter;
    }
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

void append_utf16(std::u16string& out, char32_t c)
{
    if (c < 0x10000) {
        out.push_back(static_cast<char16_t>(c));
    } else {
        c -= 0x10000;
        out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
    }
}

/// Byte length of the white-space character that ends `s`, or 0. Every
/// white-space character is in the BMP, and its encoding starts with a byte
/// that cannot continue a preceding sequence, so a suffix match agrees with
/// forward decoding.
[[nodiscard]] std::size_t trailing_white_space_length(std::string_view s) noexcept
{
    const auto byte = [&](std::size_t from_end) { return static_cast<unsigned char>(s[s.size() - from_end]); };
    if (s.empty()) {
        return 0;
    }
    if (byte(1) < 0x80) {
        return is_white_space(byte(1)) ? 1 : 0;
    }
    if (s.size() >= 2 && byte(2) >= 0xC2 && byte(2) <= 0xDF) {
        const char32_t c = ((byte(2) & 0x1Fu) << 6) | (byte(1) & 0x3Fu);
        return (byte(1) & 0xC0) == 0x80 && is_white_space(c) ? 2 : 0;
    }
    if (s.size() >= 3 && (byte(3) & 0xF0) == 0xE0 && (byte(2) & 0xC0) == 0x80 && (byte(1) & 0xC0) == 0x80) {
        const char32_t c = ((byte(3) & 0x0Fu) << 12) | ((byte(2) & 0x3Fu) << 6) | (byte(1) & 0x3Fu);
        return is_white_space(c) ? 3 : 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Case mapping
// ---------------------------------------------------------------------------

/// Invariant upper case of one code point (.NET ChangeCaseInvariant: ı stays ı).
[[nodiscard]] char32_t upper_invariant(char32_t c) noexcept
{
    if (c < 0x80) {
        return c >= 'a' && c <= 'z' ? c - 0x20 : c;
    }
    return c == 0x0131 ? c : static_cast<char32_t>(u_toupper(static_cast<UChar32>(c)));
}

/// Invariant lower case of one code point (.NET ChangeCaseInvariant: İ stays İ).
[[nodiscard]] char32_t lower_invariant(char32_t c) noexcept
{
    if (c < 0x80) {
        return c >= 'A' && c <= 'Z' ? c + 0x20 : c;
    }
    return c == 0x0130 ? c : static_cast<char32_t>(u_tolower(static_cast<UChar32>(c)));
}

/// .NET OrdinalCasing.ToUpper: simple upper case, except that ı (U+0131) and
/// ſ (U+017F) do not map to ASCII.
[[nodiscard]] char32_t ordinal_upper(char32_t c) noexcept
{
    if (c < 0x80) {
        return c >= 'a' && c <= 'z' ? c - 0x20 : c;
    }
    if (c == 0x0131 || c == 0x017F) {
        return c;
    }
    return static_cast<char32_t>(u_toupper(static_cast<UChar32>(c)));
}

template <typename Map>
[[nodiscard]] std::string map_code_points(std::string_view s, Map map)
{
    std::string out;
    out.reserve(s.size());
    for_each_code_point(s, [&](char32_t c) { append_utf8(out, map(c)); });
    return out;
}

// ---------------------------------------------------------------------------
// TextNormalizer.Key helpers
// ---------------------------------------------------------------------------

[[noreturn]] void throw_icu_error(const char* what, UErrorCode error)
{
    throw std::runtime_error(std::string{what} + " failed: " + u_errorName(error));
}

/// Canonical decomposition (NFD, not NFKD) through ICU (02 §4.3 step 3).
[[nodiscard]] std::u16string decompose_nfd(std::u16string_view s)
{
    UErrorCode error = U_ZERO_ERROR;
    const UNormalizer2* nfd = unorm2_getNFDInstance(&error);
    if (U_FAILURE(error)) {
        throw_icu_error("unorm2_getNFDInstance", error);
    }
    const auto length = static_cast<int32_t>(s.size());
    if (unorm2_spanQuickCheckYes(nfd, s.data(), length, &error) == length && U_SUCCESS(error)) {
        return std::u16string{s};
    }
    error = U_ZERO_ERROR;
    const int32_t needed = unorm2_normalize(nfd, s.data(), length, nullptr, 0, &error);
    if (U_FAILURE(error) && error != U_BUFFER_OVERFLOW_ERROR) {
        throw_icu_error("unorm2_normalize", error);
    }
    std::u16string out(static_cast<std::size_t>(needed), u'\0');
    error = U_ZERO_ERROR;
    unorm2_normalize(nfd, s.data(), length, out.data(), needed, &error);
    if (U_FAILURE(error)) {
        throw_icu_error("unorm2_normalize", error);
    }
    return out;
}

/// The 8-letter MAP of 02 §4.1, applied after lower-casing.
void append_key_char(std::u16string& out, char16_t c)
{
    switch (c) {
    case u'ł': out += u'l'; break;   // ł
    case u'đ': out += u'd'; break;   // đ
    case u'ø': out += u'o'; break;   // ø
    case u'ß': out += u"ss"; break;  // ß
    case u'æ': out += u"ae"; break;  // æ
    case u'œ': out += u"oe"; break;  // œ
    case u'ı': out += u'i'; break;   // ı
    case u'þ': out += u"th"; break;  // þ
    default: out += c; break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

std::u16string utf8_to_utf16(std::string_view utf8)
{
    std::u16string out;
    out.reserve(utf8.size());
    for_each_code_point(utf8, [&](char32_t c) { append_utf16(out, c); });
    return out;
}

std::string utf16_to_utf8(std::u16string_view utf16)
{
    std::string out;
    out.reserve(utf16.size() * 3);
    for (std::size_t i = 0; i < utf16.size(); ++i) {
        char32_t c = utf16[i];
        if (is_high_surrogate(c) && i + 1 < utf16.size() && is_low_surrogate(utf16[i + 1])) {
            c = 0x10000 + ((c - 0xD800) << 10) + (utf16[i + 1] - 0xDC00u);
            ++i;
        }
        append_utf8(out, c); // a lone surrogate becomes U+FFFD
    }
    return out;
}

std::size_t utf16_length(std::string_view utf8) noexcept
{
    std::size_t length = 0;
    for_each_code_point(utf8, [&](char32_t c) { length += c >= 0x10000 ? 2 : 1; });
    return length;
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

UnicodeCategory general_category(char32_t c) noexcept
{
    switch (static_cast<UCharCategory>(u_charType(static_cast<UChar32>(c)))) {
    case U_UPPERCASE_LETTER: return UnicodeCategory::uppercase_letter;
    case U_LOWERCASE_LETTER: return UnicodeCategory::lowercase_letter;
    case U_TITLECASE_LETTER: return UnicodeCategory::titlecase_letter;
    case U_MODIFIER_LETTER: return UnicodeCategory::modifier_letter;
    case U_OTHER_LETTER: return UnicodeCategory::other_letter;
    case U_NON_SPACING_MARK: return UnicodeCategory::nonspacing_mark;
    case U_COMBINING_SPACING_MARK: return UnicodeCategory::spacing_combining_mark;
    case U_ENCLOSING_MARK: return UnicodeCategory::enclosing_mark;
    case U_DECIMAL_DIGIT_NUMBER: return UnicodeCategory::decimal_digit_number;
    case U_LETTER_NUMBER: return UnicodeCategory::letter_number;
    case U_OTHER_NUMBER: return UnicodeCategory::other_number;
    case U_SPACE_SEPARATOR: return UnicodeCategory::space_separator;
    case U_LINE_SEPARATOR: return UnicodeCategory::line_separator;
    case U_PARAGRAPH_SEPARATOR: return UnicodeCategory::paragraph_separator;
    case U_CONTROL_CHAR: return UnicodeCategory::control;
    case U_FORMAT_CHAR: return UnicodeCategory::format;
    case U_SURROGATE: return UnicodeCategory::surrogate;
    case U_PRIVATE_USE_CHAR: return UnicodeCategory::private_use;
    case U_CONNECTOR_PUNCTUATION: return UnicodeCategory::connector_punctuation;
    case U_DASH_PUNCTUATION: return UnicodeCategory::dash_punctuation;
    case U_START_PUNCTUATION: return UnicodeCategory::open_punctuation;
    case U_END_PUNCTUATION: return UnicodeCategory::close_punctuation;
    case U_INITIAL_PUNCTUATION: return UnicodeCategory::initial_quote_punctuation;
    case U_FINAL_PUNCTUATION: return UnicodeCategory::final_quote_punctuation;
    case U_OTHER_PUNCTUATION: return UnicodeCategory::other_punctuation;
    case U_MATH_SYMBOL: return UnicodeCategory::math_symbol;
    case U_CURRENCY_SYMBOL: return UnicodeCategory::currency_symbol;
    case U_MODIFIER_SYMBOL: return UnicodeCategory::modifier_symbol;
    case U_OTHER_SYMBOL: return UnicodeCategory::other_symbol;
    default: return UnicodeCategory::other_not_assigned;
    }
}

bool is_letter(char32_t c) noexcept
{
    return general_category(c) <= UnicodeCategory::other_letter;
}

bool is_digit(char32_t c) noexcept
{
    return general_category(c) == UnicodeCategory::decimal_digit_number;
}

bool is_letter_or_digit(char32_t c) noexcept
{
    const UnicodeCategory category = general_category(c);
    return category <= UnicodeCategory::other_letter || category == UnicodeCategory::decimal_digit_number;
}

// ---------------------------------------------------------------------------
// Trimming
// ---------------------------------------------------------------------------

std::string_view trim_start(std::string_view s) noexcept
{
    std::size_t i = 0;
    while (i < s.size()) {
        const Decoded d = decode_utf8(s, i);
        if (!is_white_space(d.code_point)) {
            break;
        }
        i += d.length;
    }
    return s.substr(i);
}

std::string_view trim_end(std::string_view s) noexcept
{
    while (const std::size_t n = trailing_white_space_length(s)) {
        s.remove_suffix(n);
    }
    return s;
}

std::string_view trim(std::string_view s) noexcept
{
    return trim_end(trim_start(s));
}

std::u16string_view trim_start(std::u16string_view s) noexcept
{
    while (!s.empty() && is_white_space(s.front())) {
        s.remove_prefix(1);
    }
    return s;
}

std::u16string_view trim_end(std::u16string_view s) noexcept
{
    while (!s.empty() && is_white_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::u16string_view trim(std::u16string_view s) noexcept
{
    return trim_end(trim_start(s));
}

bool is_null_or_white_space(std::string_view s) noexcept
{
    return trim_start(s).empty();
}

bool is_null_or_white_space(std::u16string_view s) noexcept
{
    return trim_start(s).empty();
}

// ---------------------------------------------------------------------------
// Invariant casing
// ---------------------------------------------------------------------------

char16_t to_upper_invariant(char16_t c) noexcept
{
    return is_surrogate(c) ? c : static_cast<char16_t>(upper_invariant(c));
}

char16_t to_lower_invariant(char16_t c) noexcept
{
    return is_surrogate(c) ? c : static_cast<char16_t>(lower_invariant(c));
}

std::string to_upper_invariant(std::string_view s)
{
    return map_code_points(s, upper_invariant);
}

std::string to_lower_invariant(std::string_view s)
{
    return map_code_points(s, lower_invariant);
}

// ---------------------------------------------------------------------------
// OrdinalIgnoreCase
// ---------------------------------------------------------------------------

int ordinal_ignore_case_compare(std::string_view a, std::string_view b) noexcept
{
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const Decoded da = decode_utf8(a, i);
        const Decoded db = decode_utf8(b, j);
        const char32_t ua = ordinal_upper(da.code_point);
        const char32_t ub = ordinal_upper(db.code_point);
        if (ua != ub) {
            return ua < ub ? -1 : 1;
        }
        i += da.length;
        j += db.length;
    }
    if (i < a.size()) {
        return 1;
    }
    return j < b.size() ? -1 : 0;
}

bool ordinal_ignore_case_equals(std::string_view a, std::string_view b) noexcept
{
    return ordinal_ignore_case_compare(a, b) == 0;
}

std::size_t OrdinalIgnoreCaseHash::operator()(std::string_view s) const noexcept
{
    // FNV-1a over the upper-cased code points.
    std::uint64_t hash = 14695981039346656037ull;
    for_each_code_point(s, [&](char32_t c) {
        hash ^= ordinal_upper(c);
        hash *= 1099511628211ull;
    });
    return static_cast<std::size_t>(hash);
}

// ---------------------------------------------------------------------------
// TextNormalizer.Key (02 §4.1)
// ---------------------------------------------------------------------------

std::u16string normalize_key(std::u16string_view text)
{
    const std::u16string_view trimmed = trim(text);
    if (trimmed.empty()) {
        return {};
    }
    const std::u16string decomposed = decompose_nfd(trimmed);
    std::u16string out;
    out.reserve(decomposed.size());
    bool last_was_space = false;
    // Surrogates need no special case: their category is Cs (not Mn), the
    // per-unit lower-casing leaves them alone and they are not white space.
    for (const char16_t raw : decomposed) {
        if (general_category(raw) == UnicodeCategory::nonspacing_mark) {
            continue; // Mc and Me are kept
        }
        const char16_t c = to_lower_invariant(raw);
        if (is_white_space(c)) {
            if (!last_was_space && !out.empty()) {
                out += u' ';
            }
            last_was_space = true;
            continue;
        }
        last_was_space = false;
        append_key_char(out, c);
    }
    out.resize(trim_end(out).size());
    return out;
}

std::string normalize_key(std::string_view text)
{
    return utf16_to_utf8(normalize_key(utf8_to_utf16(text)));
}

} // namespace wizz::text
