#include "wizz/core/locale.hpp"

#include "wizz/core/text.hpp"

#include <windows.h>

#include <icu.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace wizz {
namespace {

using text::UnicodeCategory;
using text::utf16_to_utf8;

// ---------------------------------------------------------------------------
// ICU plumbing
// ---------------------------------------------------------------------------

template <auto Close>
struct IcuCloser {
    template <typename T>
    void operator()(T* handle) const noexcept
    {
        Close(handle);
    }
};
using CollatorHandle = std::unique_ptr<UCollator, IcuCloser<&ucol_close>>;
using CollationElementsHandle = std::unique_ptr<UCollationElements, IcuCloser<&ucol_closeElements>>;
using NumberFormatHandle = std::unique_ptr<UNumberFormat, IcuCloser<&unum_close>>;
using DateFormatHandle = std::unique_ptr<UDateFormat, IcuCloser<&udat_close>>;

void check(UErrorCode error, const char* what)
{
    if (U_FAILURE(error)) {
        throw std::runtime_error(std::format("ICU {} failed: {}", what, u_errorName(error)));
    }
}

[[nodiscard]] int32_t icu_length(std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
        throw std::length_error("string too long for ICU");
    }
    return static_cast<int32_t>(size);
}

/// Reads an ICU string result, retrying once with the size ICU reports.
/// `fill(buffer, capacity, &error)` returns the full length.
template <typename Fill>
[[nodiscard]] std::u16string read_icu_string(const char* what, Fill fill)
{
    std::u16string buffer(64, u'\0');
    UErrorCode error = U_ZERO_ERROR;
    int32_t length = fill(buffer.data(), icu_length(buffer.size()), &error);
    if (error == U_BUFFER_OVERFLOW_ERROR) {
        buffer.resize(static_cast<std::size_t>(length));
        error = U_ZERO_ERROR;
        length = fill(buffer.data(), length, &error);
    }
    check(error, what);
    buffer.resize(static_cast<std::size_t>(length));
    return buffer;
}

/// BCP-47 name ("de-DE") → ICU locale ID ("de_DE"). Strict: the whole name must parse.
[[nodiscard]] std::string icu_locale_id(std::string_view name)
{
    const std::string tag{name};
    std::array<char, ULOC_FULLNAME_CAPACITY> buffer{};
    int32_t parsed = 0;
    UErrorCode error = U_ZERO_ERROR;
    const int32_t length = uloc_forLanguageTag(tag.c_str(), buffer.data(), icu_length(buffer.size()), &parsed, &error);
    if (tag.empty() || U_FAILURE(error) || error == U_STRING_NOT_TERMINATED_WARNING ||
        parsed != icu_length(tag.size()) || length <= 0) {
        throw std::invalid_argument(std::format("invalid locale name \"{}\"", name));
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

/// The ICU ID with "@calendar=gregorian": .NET formats with the Gregorian
/// calendar's names (CalendarData.ICU EnumSymbols).
[[nodiscard]] std::string gregorian_locale_id(const std::string& id)
{
    std::array<char, ULOC_FULLNAME_CAPACITY> buffer{};
    if (id.size() >= buffer.size()) {
        throw std::invalid_argument("locale ID too long");
    }
    std::ranges::copy(id, buffer.begin());
    UErrorCode error = U_ZERO_ERROR;
    uloc_setKeywordValue("calendar", "gregorian", buffer.data(), icu_length(buffer.size()), &error);
    check(error, "uloc_setKeywordValue");
    return buffer.data();
}

// ---------------------------------------------------------------------------
// Number symbols (CultureData.GetNFIValues on ICU)
// ---------------------------------------------------------------------------

[[nodiscard]] NumberSymbols load_number_symbols(const std::string& id)
{
    UErrorCode error = U_ZERO_ERROR;
    const NumberFormatHandle format{unum_open(UNUM_DECIMAL, nullptr, 0, id.c_str(), nullptr, &error)};
    check(error, "unum_open");
    const auto symbol = [&](UNumberFormatSymbol which) {
        return utf16_to_utf8(read_icu_string("unum_getSymbol", [&](UChar* buffer, int32_t capacity, UErrorCode* status) {
            return unum_getSymbol(format.get(), which, buffer, capacity, status);
        }));
    };
    NumberSymbols symbols;
    symbols.decimal_separator = symbol(UNUM_DECIMAL_SEPARATOR_SYMBOL);
    symbols.group_separator = symbol(UNUM_GROUPING_SEPARATOR_SYMBOL);
    symbols.negative_sign = symbol(UNUM_MINUS_SIGN_SYMBOL);
    // GlobalizationNative_GetLocaleInfoGroupingSizes + CultureData.IcuGetLocaleInfo(LocaleGroupingData).
    const int primary = unum_getAttribute(format.get(), UNUM_GROUPING_SIZE);
    const int secondary = unum_getAttribute(format.get(), UNUM_SECONDARY_GROUPING_SIZE);
    symbols.group_sizes = secondary == 0 ? std::vector<int>{primary} : std::vector<int>{primary, secondary};
    return symbols;
}

/// CultureData.ConvertWin32GroupString: "3;0" → {3}, "3;2;0" → {3, 2}, "3" → {3, 0}.
[[nodiscard]] std::vector<int> convert_win32_group_string(std::u16string_view s)
{
    if (s.empty()) {
        return {3};
    }
    if (s.front() == u'0') {
        return {0};
    }
    std::vector<int> values(s.back() == u'0' ? s.size() / 2 : s.size() / 2 + 2, 0);
    for (std::size_t i = 0, j = 0; i < s.size() && j < values.size(); i += 2, ++j) {
        if (s[i] < u'1' || s[i] > u'9') {
            return {3};
        }
        values[j] = s[i] - u'0';
    }
    return values;
}

// ---------------------------------------------------------------------------
// Time separator (CultureData.TimeSeparator)
// ---------------------------------------------------------------------------

/// CultureData.ConvertIcuTimeFormatString: keeps quoted text, ':', '.', H, h,
/// m, s; space and NBSP become ' '; the first 'a' becomes "tt"; drops the rest.
[[nodiscard]] std::u16string convert_icu_time_pattern(std::u16string_view icu)
{
    std::u16string result;
    bool am_pm_added = false;
    for (std::size_t i = 0; i < icu.size(); ++i) {
        switch (const char16_t c = icu[i]) {
        case u'\'':
            result += icu[i++];
            while (i < icu.size()) {
                result += icu[i];
                if (icu[i] == u'\'') {
                    break;
                }
                ++i;
            }
            break;
        case u':':
        case u'.':
        case u'H':
        case u'h':
        case u'm':
        case u's':
            result += c;
            break;
        case u' ':
        case u' ':
            result += u' ';
            break;
        case u'a':
            if (!am_pm_added) {
                am_pm_added = true;
                result += u"tt";
            }
            break;
        default:
            break;
        }
    }
    return result;
}

/// CultureData.ReescapeWin32String: Win32 '' inside quotes → \', \ → \\.
[[nodiscard]] std::u16string reescape_win32_string(std::u16string_view s)
{
    std::u16string result;
    bool in_quote = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == u'\'') {
            if (in_quote && i + 1 < s.size() && s[i + 1] == u'\'') {
                result += u"\\'";
                ++i;
                continue;
            }
            in_quote = !in_quote;
        } else if (s[i] == u'\\') {
            result += u"\\\\";
            continue;
        }
        result += s[i];
    }
    return result;
}

constexpr std::u16string_view kTimeParts = u"Hhms";

/// CultureData.IndexOfTimePart.
[[nodiscard]] std::size_t index_of_time_part(std::u16string_view format, std::size_t start)
{
    bool in_quote = false;
    for (std::size_t i = start; i < format.size(); ++i) {
        if (!in_quote && kTimeParts.contains(format[i])) {
            return i;
        }
        if (format[i] == u'\\') {
            if (i + 1 < format.size() && (format[i + 1] == u'\'' || format[i + 1] == u'\\')) {
                ++i;
            }
        } else if (format[i] == u'\'') {
            in_quote = !in_quote;
        }
    }
    return std::u16string_view::npos;
}

/// CultureData.UnescapeNlsString over [start, end] (inclusive).
[[nodiscard]] std::u16string unescape_nls_string(std::u16string_view s, std::size_t start, std::size_t end)
{
    std::u16string result;
    for (std::size_t i = start; i < s.size() && i <= end; ++i) {
        if (s[i] == u'\'') {
            continue;
        }
        if (s[i] == u'\\') {
            ++i;
            if (i < s.size()) {
                result += s[i];
            }
            continue;
        }
        result += s[i];
    }
    return result;
}

/// CultureData.GetTimeSeparator: the text between the first time part and the next one.
[[nodiscard]] std::u16string time_separator_of(std::u16string_view format)
{
    std::size_t index = index_of_time_part(format, 0);
    if (index == std::u16string_view::npos) {
        return {};
    }
    const char16_t part = format[index];
    do {
        ++index;
    } while (index < format.size() && format[index] == part);
    const std::size_t separator_start = index;
    const std::size_t separator_end = index_of_time_part(format, separator_start);
    if (separator_end == std::u16string_view::npos || separator_end == separator_start) {
        return {};
    }
    return unescape_nls_string(format, separator_start, separator_end - 1);
}

[[nodiscard]] std::string load_time_separator(const std::string& id, std::string_view name)
{
    // .NET special case: fr-CA's "HH 'h' mm 'min' ss 's'" has no derivable separator.
    if (text::ordinal_ignore_case_equals(name, "fr-CA")) {
        return ":";
    }
    // GlobalizationNative_GetLocaleTimeFormat(shortFormat: false) = the medium time style.
    UErrorCode error = U_ZERO_ERROR;
    const DateFormatHandle format{udat_open(UDAT_MEDIUM, UDAT_NONE, id.c_str(), nullptr, 0, nullptr, 0, &error)};
    check(error, "udat_open");
    const std::u16string pattern =
        read_icu_string("udat_toPattern", [&](UChar* buffer, int32_t capacity, UErrorCode* status) {
            return udat_toPattern(format.get(), false, buffer, capacity, status);
        });
    return utf16_to_utf8(time_separator_of(convert_icu_time_pattern(pattern)));
}

// ---------------------------------------------------------------------------
// Month and weekday names (CalendarData.ICU, Gregorian)
// ---------------------------------------------------------------------------

// pal_calendarData.c: AbbreviatedMonthGenitiveNames come from UDAT_SHORT_MONTHS,
// AbbreviatedMonthNames from UDAT_STANDALONE_SHORT_MONTHS and AbbreviatedDayNames
// from UDAT_STANDALONE_SHORT_WEEKDAYS (de: "Fr", not the format form "Fr.").
struct CalendarNames {
    std::array<std::string, 12> months_format;
    std::array<std::string, 12> months_standalone;
    std::array<std::string, 7> days; // Sunday first
};

[[nodiscard]] CalendarNames load_calendar_names(const std::string& id)
{
    const std::string gregorian = gregorian_locale_id(id);
    UErrorCode error = U_ZERO_ERROR;
    const DateFormatHandle format{
        udat_open(UDAT_DEFAULT, UDAT_DEFAULT, gregorian.c_str(), nullptr, 0, nullptr, 0, &error)};
    check(error, "udat_open");
    const auto symbol = [&](UDateFormatSymbolType type, int32_t index) {
        return utf16_to_utf8(read_icu_string("udat_getSymbols", [&](UChar* buffer, int32_t capacity, UErrorCode* status) {
            return udat_getSymbols(format.get(), type, index, buffer, capacity, status);
        }));
    };
    CalendarNames names;
    for (int32_t month = 0; month < 12; ++month) {
        names.months_format[static_cast<std::size_t>(month)] = symbol(UDAT_SHORT_MONTHS, month);
        names.months_standalone[static_cast<std::size_t>(month)] = symbol(UDAT_STANDALONE_SHORT_MONTHS, month);
    }
    for (int32_t day = 0; day < 7; ++day) {
        names.days[static_cast<std::size_t>(day)] = symbol(UDAT_STANDALONE_SHORT_WEEKDAYS, UCAL_SUNDAY + day);
    }
    return names;
}

// ---------------------------------------------------------------------------
// Collators (System.Globalization.Native pal_collation.c)
// ---------------------------------------------------------------------------

/// Code points whose half-/full-width counterpart .NET keeps distinct below
/// tertiary strength (g_HalfFullHigherChars): full-width ASCII except U+FF3C,
/// full-width signs, half-width katakana and half-width hangul. The "lower"
/// partner of each is its compatibility decomposition.
constexpr std::array<std::pair<char16_t, char16_t>, 9> kHalfFullHigherRanges{{
    {u'！', u'［'},
    {u'］', u'～'},
    {u'￠', u'￦'},
    {u'｡', u'ﾝ'},
    {u'ﾠ', u'ﾾ'},
    {u'ￂ', u'ￇ'},
    {u'ￊ', u'ￏ'},
    {u'ￒ', u'ￗ'},
    {u'ￚ', u'ￜ'},
}};

/// pal_collation.c NeedsEscape: ASCII punctuation must be escaped in rules.
[[nodiscard]] constexpr bool needs_rule_escape(char16_t c) noexcept
{
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

/// pal_collation.c GetCustomRules for a strength below tertiary without
/// IgnoreKanaType/IgnoreWidth: hiragana/katakana and half/full-width pairs
/// become primary differences ("&a<b"), so ignoring case does not also ignore
/// kana type and width.
[[nodiscard]] std::u16string build_kana_and_width_rules()
{
    std::u16string rules;
    for (char16_t hiragana = 0x3041; hiragana <= 0x309E; ++hiragana) {
        if (hiragana <= 0x3096 || hiragana >= 0x309D) {
            rules += u'&';
            rules += hiragana;
            rules += u'<';
            rules += static_cast<char16_t>(hiragana + (0x30A1 - 0x3041));
        }
    }
    UErrorCode error = U_ZERO_ERROR;
    const UNormalizer2* nfkc = unorm2_getNFKCInstance(&error);
    check(error, "unorm2_getNFKCInstance");
    for (const auto& [first, last] : kHalfFullHigherRanges) {
        for (char16_t higher = first; higher <= last; ++higher) {
            std::array<UChar, 4> lower{};
            const int32_t length = unorm2_getRawDecomposition(nfkc, higher, lower.data(), icu_length(lower.size()), &error);
            check(error, "unorm2_getRawDecomposition");
            if (length != 1) {
                continue;
            }
            rules += u'&';
            if (needs_rule_escape(lower[0])) {
                rules += u'\\';
            }
            rules += lower[0];
            rules += u'<';
            rules += higher;
        }
    }
    // Full-width capitals stay case variants of the full-width small letters
    // (FullWidthAlphabetRangeLength), so IgnoreCase still equates "Ａ" and "ａ".
    for (char16_t capital = u'Ａ'; capital <= u'Ｚ'; ++capital) {
        rules += u'&';
        rules += static_cast<char16_t>(capital + 0x20);
        rules += u"<<<";
        rules += capital;
    }
    return rules;
}

[[nodiscard]] const std::u16string& kana_and_width_rules()
{
    static const std::u16string rules = build_kana_and_width_rules();
    return rules;
}

/// CloneCollatorWithOptions for IgnoreCase (secondary) and IgnoreCase |
/// IgnoreNonSpace (primary): locale tailoring + custom rules, reopened at the
/// lower strength. Case level stays off because case is ignored.
[[nodiscard]] CollatorHandle open_ignore_case_collator(const UCollator* base, UColAttributeValue strength)
{
    int32_t locale_rules_length = 0;
    const UChar* locale_rules = ucol_getRules(base, &locale_rules_length);
    std::u16string rules;
    if (locale_rules != nullptr) {
        rules.assign(locale_rules, static_cast<std::size_t>(locale_rules_length));
    }
    rules += kana_and_width_rules();
    UErrorCode error = U_ZERO_ERROR;
    CollatorHandle collator{
        ucol_openRules(rules.data(), icu_length(rules.size()), UCOL_DEFAULT, strength, nullptr, &error)};
    check(error, "ucol_openRules");
    ucol_setAttribute(collator.get(), UCOL_STRENGTH, strength, &error);
    check(error, "ucol_setAttribute");
    return collator;
}

/// A collator with its own lock: one UCollator is never used by two threads at once.
struct GuardedCollator {
    std::mutex mutex;
    CollatorHandle handle;
};

// Collation element masks (pal_collation.c; not exported by the SDK's icu.h).
constexpr std::uint32_t kPrimaryOrderMask = 0xFFFF0000u;
constexpr std::uint32_t kSecondaryOrderMask = 0x0000FF00u;
constexpr std::uint32_t kTertiaryOrderMask = 0x000000FFu;
constexpr int32_t kIgnorable = 0;

/// pal_collation.c SimpleAffix_Iterators, forward direction (StartsWith).
[[nodiscard]] bool simple_prefix_match(const UCollator* collator, std::u16string_view pattern,
                                       std::u16string_view source)
{
    static constexpr char16_t kEmpty[] = u"";
    UErrorCode error = U_ZERO_ERROR;
    const CollationElementsHandle pattern_elements{ucol_openElements(
        collator, pattern.empty() ? kEmpty : pattern.data(), icu_length(pattern.size()), &error)};
    check(error, "ucol_openElements");
    const CollationElementsHandle source_elements{
        ucol_openElements(collator, source.empty() ? kEmpty : source.data(), icu_length(source.size()), &error)};
    check(error, "ucol_openElements");

    const UColAttributeValue strength = ucol_getStrength(collator);
    const std::uint32_t mask = strength == UCOL_PRIMARY     ? kPrimaryOrderMask
                               : strength == UCOL_SECONDARY ? kPrimaryOrderMask | kSecondaryOrderMask
                                                            : kPrimaryOrderMask | kSecondaryOrderMask | kTertiaryOrderMask;
    bool move_pattern = true;
    bool move_source = true;
    int32_t pattern_element = kIgnorable;
    int32_t source_element = kIgnorable;
    while (true) {
        if (move_pattern) {
            pattern_element = ucol_next(pattern_elements.get(), &error);
        }
        if (move_source) {
            source_element = ucol_next(source_elements.get(), &error);
        }
        check(error, "ucol_next");
        move_pattern = true;
        move_source = true;

        if (pattern_element == UCOL_NULLORDER) {
            // The prefix is exhausted. It matches unless the source continues with
            // a combining mark ("ö" does not start with "o").
            const auto element = static_cast<std::uint32_t>(source_element);
            return source_element == UCOL_NULLORDER || source_element == kIgnorable ||
                   (element & kPrimaryOrderMask) != 0 || (element & kSecondaryOrderMask) == 0;
        }
        if (pattern_element == kIgnorable) {
            move_source = false;
        } else if (source_element == kIgnorable) {
            move_pattern = false;
        } else if ((static_cast<std::uint32_t>(pattern_element) & mask) !=
                   (static_cast<std::uint32_t>(source_element) & mask)) {
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// Culture casing (pal_casing.c ChangeCase / ChangeCaseTurkish)
// ---------------------------------------------------------------------------

[[nodiscard]] char32_t culture_upper(char32_t c, bool turkish) noexcept
{
    return turkish && c == U'i' ? U'İ' : static_cast<char32_t>(u_toupper(static_cast<UChar32>(c)));
}

[[nodiscard]] char32_t culture_lower(char32_t c, bool turkish) noexcept
{
    return turkish && c == U'I' ? U'ı' : static_cast<char32_t>(u_tolower(static_cast<UChar32>(c)));
}

[[nodiscard]] constexpr bool is_high_surrogate(char16_t c) noexcept { return c >= 0xD800 && c <= 0xDBFF; }
[[nodiscard]] constexpr bool is_low_surrogate(char16_t c) noexcept { return c >= 0xDC00 && c <= 0xDFFF; }

/// Code point at s[i] and its UTF-16 length (a lone surrogate is its own code point).
[[nodiscard]] std::pair<char32_t, std::size_t> code_point_at(std::u16string_view s, std::size_t i) noexcept
{
    if (is_high_surrogate(s[i]) && i + 1 < s.size() && is_low_surrogate(s[i + 1])) {
        return {0x10000 + ((static_cast<char32_t>(s[i]) - 0xD800) << 10) + (static_cast<char32_t>(s[i + 1]) - 0xDC00),
                2};
    }
    return {s[i], 1};
}

void append_code_point(std::u16string& out, char32_t c)
{
    if (c < 0x10000) {
        out += static_cast<char16_t>(c);
    } else {
        out += static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10));
        out += static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
    }
}

/// Simple, length-preserving culture case change of a UTF-16 string.
template <typename Map>
[[nodiscard]] std::u16string change_case(std::u16string_view s, Map map)
{
    std::u16string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto [c, length] = code_point_at(s, i);
        append_code_point(out, map(c));
        i += length;
    }
    return out;
}

// ---------------------------------------------------------------------------
// TextInfo.ToTitleCase (02 §5.6)
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool is_letter_category(UnicodeCategory category) noexcept
{
    return category <= UnicodeCategory::other_letter;
}

/// TextInfo.IsWordSeparator: Zs Zl Zp Cc Cf Pc Pd Ps Pe Pi Pf Po Sm Sc Sk So.
[[nodiscard]] constexpr bool is_word_separator(UnicodeCategory category) noexcept
{
    switch (category) {
    case UnicodeCategory::space_separator:
    case UnicodeCategory::line_separator:
    case UnicodeCategory::paragraph_separator:
    case UnicodeCategory::control:
    case UnicodeCategory::format:
    case UnicodeCategory::connector_punctuation:
    case UnicodeCategory::dash_punctuation:
    case UnicodeCategory::open_punctuation:
    case UnicodeCategory::close_punctuation:
    case UnicodeCategory::initial_quote_punctuation:
    case UnicodeCategory::final_quote_punctuation:
    case UnicodeCategory::other_punctuation:
    case UnicodeCategory::math_symbol:
    case UnicodeCategory::currency_symbol:
    case UnicodeCategory::modifier_symbol:
    case UnicodeCategory::other_symbol:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::u16string title_case_utf16(std::u16string_view s, bool turkish, bool dutch)
{
    std::u16string result;
    result.reserve(s.size());
    std::optional<std::u16string> lower; // the whole string lower-cased, computed on demand
    const auto append_word_tail = [&](std::size_t start, std::size_t count, bool lower_case) {
        if (lower_case) {
            if (!lower) {
                lower = change_case(s, [&](char32_t c) { return culture_lower(c, turkish); });
            }
            result.append(*lower, start, count);
        } else {
            result.append(s.substr(start, count));
        }
    };
    const auto category_at = [&](std::size_t i) {
        const auto [c, length] = code_point_at(s, i);
        return std::pair{text::general_category(c), length};
    };
    // AddNonLetter: appends one character (1 or 2 units), returns the index of its last unit.
    const auto add_non_letter = [&](std::size_t i, std::size_t length) {
        result.append(s.substr(i, length));
        return i + length - 1;
    };
    // AddTitlecaseLetter: title-cases the word's first letter, returns the index of its last unit.
    const auto add_titlecase_letter = [&](std::size_t i, std::size_t length) {
        if (length == 2) {
            append_code_point(result, culture_upper(code_point_at(s, i).first, turkish));
            return i + 1;
        }
        switch (s[i]) {
        case u'Ǆ':
        case u'ǅ':
        case u'ǆ':
            result += u'ǅ'; // ǅ
            break;
        case u'Ǉ':
        case u'ǈ':
        case u'ǉ':
            result += u'ǈ'; // ǈ
            break;
        case u'Ǌ':
        case u'ǋ':
        case u'ǌ':
            result += u'ǋ'; // ǋ
            break;
        case u'Ǳ':
        case u'ǲ':
        case u'ǳ':
            result += u'ǲ'; // ǲ
            break;
        default:
            append_code_point(result, culture_upper(s[i], turkish));
            break;
        }
        return i;
    };

    for (std::size_t i = 0; i < s.size(); ++i) {
        auto [category, length] = category_at(i);
        if (!is_letter_category(category)) {
            i = add_non_letter(i, length);
            continue;
        }
        if (dutch && i + 1 < s.size() && (s[i] == u'i' || s[i] == u'I') && (s[i + 1] == u'j' || s[i + 1] == u'J')) {
            result += u"IJ";
            i += 2;
        } else {
            i = add_titlecase_letter(i, length) + 1;
        }
        // The rest of the word is lower-cased unless it has no lower-case letter
        // (acronyms stay); an apostrophe continues the word and forces lower case.
        std::size_t lowercase_start = i;
        bool has_lower_case = category == UnicodeCategory::lowercase_letter;
        while (i < s.size()) {
            std::tie(category, length) = category_at(i);
            if (is_letter_category(category)) {
                has_lower_case = has_lower_case || category == UnicodeCategory::lowercase_letter;
                i += length;
            } else if (s[i] == u'\'') {
                ++i;
                append_word_tail(lowercase_start, i - lowercase_start, has_lower_case);
                lowercase_start = i;
                has_lower_case = true;
            } else if (!is_word_separator(category)) {
                i += length; // marks, numbers, private use, surrogates and unassigned continue the word
            } else {
                break;
            }
        }
        if (i > lowercase_start) {
            append_word_tail(lowercase_start, i - lowercase_start, has_lower_case);
        }
        if (i < s.size()) {
            i = add_non_letter(i, length);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// .NET custom date/time format (DateTimeFormat.FormatCustomized subset)
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t repeat_count(std::string_view pattern, std::size_t i) noexcept
{
    std::size_t n = 1;
    while (i + n < pattern.size() && pattern[i + n] == pattern[i]) {
        ++n;
    }
    return n;
}

/// DateTimeFormat.IsUseGenitiveForm: is there a "d" or "dd" token before or after MMM?
[[nodiscard]] bool is_use_genitive_form(std::string_view pattern, std::size_t index, std::size_t token_length) noexcept
{
    // Look back for the nearest 'd' and count the run it ends.
    std::ptrdiff_t i = static_cast<std::ptrdiff_t>(index) - 1;
    while (i >= 0 && pattern[static_cast<std::size_t>(i)] != 'd') {
        --i;
    }
    if (i >= 0) {
        int repeat = 0;
        while (--i >= 0 && pattern[static_cast<std::size_t>(i)] == 'd') {
            ++repeat;
        }
        if (repeat <= 1) {
            return true;
        }
    }
    // Look ahead for the next 'd' run.
    std::size_t j = index + token_length;
    while (j < pattern.size() && pattern[j] != 'd') {
        ++j;
    }
    if (j < pattern.size()) {
        int repeat = 0;
        while (++j < pattern.size() && pattern[j] == 'd') {
            ++repeat;
        }
        return repeat <= 1;
    }
    return false;
}

[[noreturn]] void throw_bad_pattern(std::string_view pattern, std::string_view why)
{
    throw std::invalid_argument(std::format("unsupported date/time pattern \"{}\": {}", pattern, why));
}

void append_digits(std::string& out, long long value, std::size_t min_length)
{
    std::format_to(std::back_inserter(out), "{:0{}}", value, min_length);
}

} // namespace

// ---------------------------------------------------------------------------
// Locale
// ---------------------------------------------------------------------------

struct Locale::Impl {
    /// Loads everything from ICU for `locale_name` (throws std::invalid_argument
    /// for a malformed name, std::runtime_error for an ICU failure).
    explicit Impl(std::string_view locale_name);

    std::string name;
    NumberSymbols numbers;
    std::string time_separator;
    CalendarNames names;
    bool turkish_casing = false; // TextInfo.NeedsTurkishCasing
    bool dutch = false;          // TextInfo.ToTitleCase "nl-" special case
    /// Indexed by CollationStrength (primary, secondary, tertiary).
    mutable std::array<GuardedCollator, 3> collators;

    [[nodiscard]] GuardedCollator& collator(CollationStrength strength) const
    {
        return collators.at(static_cast<std::size_t>(strength));
    }

    void append_formatted(std::string& out, LocalDateTime time, std::string_view pattern) const;
};

Locale::Impl::Impl(std::string_view locale_name) : name(locale_name)
{
    const std::string id = icu_locale_id(locale_name);
    numbers = load_number_symbols(id);
    time_separator = load_time_separator(id, locale_name);
    names = load_calendar_names(id);

    // CompareOptions.None uses the locale collator as opened (pal_collation.c).
    UErrorCode error = U_ZERO_ERROR;
    CollatorHandle base{ucol_open(id.c_str(), &error)};
    check(error, "ucol_open");
    collator(CollationStrength::secondary).handle = open_ignore_case_collator(base.get(), UCOL_SECONDARY);
    collator(CollationStrength::primary).handle = open_ignore_case_collator(base.get(), UCOL_PRIMARY);
    collator(CollationStrength::tertiary).handle = std::move(base);

    // TextInfo.NeedsTurkishCasing: "ı" equals "I" under IgnoreCase (tr, az).
    constexpr std::u16string_view dotless_i = u"ı";
    constexpr std::u16string_view capital_i = u"I";
    turkish_casing = ucol_strcoll(collator(CollationStrength::secondary).handle.get(), dotless_i.data(),
                                  icu_length(dotless_i.size()), capital_i.data(),
                                  icu_length(capital_i.size())) == UCOL_EQUAL;
    dutch = locale_name.size() >= 3 && text::ordinal_ignore_case_equals(locale_name.substr(0, 3), "nl-");
}

void Locale::Impl::append_formatted(std::string& out, LocalDateTime time, std::string_view pattern) const
{
    const Date date = date_of(time);
    const std::chrono::hh_mm_ss<Ticks> clock{time_of_day(time)};
    for (std::size_t i = 0; i < pattern.size();) {
        const char ch = pattern[i];
        std::size_t token_length = 1;
        switch (ch) {
        case 'd':
            token_length = repeat_count(pattern, i);
            if (token_length <= 2) {
                append_digits(out, date.day(), token_length);
            } else if (token_length == 3) {
                out += names.days.at(date.weekday().c_encoding());
            } else {
                throw_bad_pattern(pattern, "full day names (dddd) are not supported");
            }
            break;
        case 'M':
            token_length = repeat_count(pattern, i);
            if (token_length <= 2) {
                append_digits(out, date.month(), token_length);
            } else if (token_length == 3) {
                const auto& months =
                    is_use_genitive_form(pattern, i, token_length) ? names.months_format : names.months_standalone;
                out += months.at(date.month() - 1);
            } else {
                throw_bad_pattern(pattern, "full month names (MMMM) are not supported");
            }
            break;
        case 'y':
            token_length = repeat_count(pattern, i);
            append_digits(out, token_length <= 2 ? date.year() % 100 : date.year(), token_length);
            break;
        case 'H':
            token_length = repeat_count(pattern, i);
            append_digits(out, clock.hours().count(), std::min<std::size_t>(token_length, 2));
            break;
        case 'm':
            token_length = repeat_count(pattern, i);
            append_digits(out, clock.minutes().count(), std::min<std::size_t>(token_length, 2));
            break;
        case 's':
            token_length = repeat_count(pattern, i);
            append_digits(out, clock.seconds().count(), std::min<std::size_t>(token_length, 2));
            break;
        case ':':
            out += time_separator;
            break;
        case '\'':
        case '"': {
            // ParseQuoteString: literal text up to the matching quote; '\' escapes.
            std::size_t pos = i + 1;
            bool closed = false;
            while (pos < pattern.size()) {
                const char c = pattern[pos++];
                if (c == ch) {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (pos >= pattern.size()) {
                        throw_bad_pattern(pattern, "'\\' at the end");
                    }
                    out += pattern[pos++];
                } else {
                    out += c;
                }
            }
            if (!closed) {
                throw_bad_pattern(pattern, "unterminated quote");
            }
            token_length = pos - i;
            break;
        }
        case '\\':
            if (i + 1 >= pattern.size()) {
                throw_bad_pattern(pattern, "'\\' at the end");
            }
            out += pattern[i + 1];
            token_length = 2;
            break;
        case '%':
            // "%x" formats x as a one-character pattern.
            if (i + 1 >= pattern.size() || pattern[i + 1] == '%') {
                throw_bad_pattern(pattern, "invalid '%'");
            }
            append_formatted(out, time, pattern.substr(i + 1, 1));
            token_length = 2;
            break;
        case 'f':
        case 'F':
        case 'g':
        case 'h':
        case 'K':
        case 't':
        case 'z':
        case '/':
            throw_bad_pattern(pattern, std::format("specifier '{}' is not supported", ch));
        default:
            out += ch;
            break;
        }
        i += token_length;
    }
}

Locale::Locale(PassKey, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Locale::~Locale() = default;

namespace {

/// Reads a Windows user-locale value (GetLocaleInfoEx with user overrides).
[[nodiscard]] std::optional<std::u16string> user_locale_info(LCTYPE type)
{
    std::array<wchar_t, 128> buffer{};
    const int length = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buffer.data(), static_cast<int>(buffer.size()));
    if (length <= 0) {
        return std::nullopt;
    }
    return std::u16string(buffer.begin(), buffer.begin() + (length - 1));
}

/// .NET reads these through NLS for the user's own culture (CultureData
/// ShouldUseUserOverrideNlsData), so Control Panel customisations apply.
void apply_user_overrides(NumberSymbols& numbers, std::string& time_separator)
{
    if (const auto value = user_locale_info(LOCALE_SDECIMAL)) {
        numbers.decimal_separator = utf16_to_utf8(*value);
    }
    if (const auto value = user_locale_info(LOCALE_STHOUSAND)) {
        numbers.group_separator = utf16_to_utf8(*value);
    }
    if (const auto value = user_locale_info(LOCALE_SNEGATIVESIGN)) {
        numbers.negative_sign = utf16_to_utf8(*value);
    }
    if (const auto value = user_locale_info(LOCALE_SGROUPING)) {
        numbers.group_sizes = convert_win32_group_string(*value);
    }
    if (const auto value = user_locale_info(LOCALE_STIMEFORMAT)) {
        time_separator = utf16_to_utf8(time_separator_of(reescape_win32_string(*value)));
    }
}

} // namespace

std::shared_ptr<const Locale> Locale::create(std::string_view name)
{
    return std::make_shared<const Locale>(PassKey{}, std::make_unique<Impl>(name));
}

std::shared_ptr<const Locale> Locale::user_default()
{
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> buffer{};
    const int length = GetUserDefaultLocaleName(buffer.data(), static_cast<int>(buffer.size()));
    if (length > 1) {
        // Drop a Windows sort suffix such as "de-DE_phoneb".
        std::u16string name(buffer.begin(), buffer.begin() + (length - 1));
        name.resize(std::min(name.size(), name.find(u'_')));
        try {
            auto impl = std::make_unique<Impl>(utf16_to_utf8(name));
            apply_user_overrides(impl->numbers, impl->time_separator);
            return std::make_shared<const Locale>(PassKey{}, std::move(impl));
        } catch (const std::invalid_argument&) {
            // Not a BCP-47 name ICU understands: fall back to en-US below.
        }
    }
    return en_us();
}

const std::shared_ptr<const Locale>& Locale::en_us()
{
    static const std::shared_ptr<const Locale> instance = create("en-US");
    return instance;
}

const std::string& Locale::name() const noexcept
{
    return impl_->name;
}

const NumberSymbols& Locale::number_symbols() const noexcept
{
    return impl_->numbers;
}

const std::string& Locale::time_separator() const noexcept
{
    return impl_->time_separator;
}

const std::string& Locale::month_abbrev(unsigned month, NameContext context) const
{
    if (month < 1 || month > 12) {
        throw std::out_of_range(std::format("month {} is not in 1..12", month));
    }
    const auto& months = context == NameContext::format ? impl_->names.months_format : impl_->names.months_standalone;
    return months[month - 1];
}

const std::string& Locale::day_abbrev(std::chrono::weekday weekday) const
{
    if (!weekday.ok()) {
        throw std::out_of_range("invalid weekday");
    }
    return impl_->names.days[weekday.c_encoding()];
}

std::string Locale::format_date(Date date, std::string_view pattern) const
{
    return format_time(at_midnight(date), pattern);
}

std::string Locale::format_time(LocalDateTime time, std::string_view pattern) const
{
    std::string out;
    impl_->append_formatted(out, time, pattern);
    return out;
}

int Locale::compare(std::string_view a, std::string_view b, CollationStrength strength) const
{
    GuardedCollator& collator = impl_->collator(strength);
    const std::scoped_lock lock{collator.mutex};
    UErrorCode error = U_ZERO_ERROR;
    // Never pass a null pointer to ICU, even with length 0 (ICU-9396).
    const UCollationResult result =
        ucol_strcollUTF8(collator.handle.get(), a.empty() ? "" : a.data(), icu_length(a.size()),
                         b.empty() ? "" : b.data(), icu_length(b.size()), &error);
    check(error, "ucol_strcollUTF8");
    return static_cast<int>(result);
}

bool Locale::starts_with(std::string_view text, std::string_view prefix, CollationStrength strength) const
{
    if (prefix.empty()) {
        return true; // CompareInfo.IsPrefix: the empty string is a prefix of everything
    }
    if (strength == CollationStrength::primary) {
        throw std::invalid_argument("starts_with supports tertiary and secondary strength only");
    }
    const std::u16string source = text::utf8_to_utf16(text);
    const std::u16string pattern = text::utf8_to_utf16(prefix);
    GuardedCollator& collator = impl_->collator(strength);
    const std::scoped_lock lock{collator.mutex};
    return simple_prefix_match(collator.handle.get(), pattern, source);
}

std::string Locale::title_case(std::string_view text) const
{
    return utf16_to_utf8(title_case_utf16(text::utf8_to_utf16(text), impl_->turkish_casing, impl_->dutch));
}

std::string Locale::to_upper(std::string_view text) const
{
    const bool turkish = impl_->turkish_casing;
    return utf16_to_utf8(change_case(text::utf8_to_utf16(text), [&](char32_t c) { return culture_upper(c, turkish); }));
}

} // namespace wizz
