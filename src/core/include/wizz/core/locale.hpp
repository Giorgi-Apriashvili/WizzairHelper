#pragma once
// The single locale service of 07 §1 (01 §16): month and weekday names, number
// separators, date/time patterns, title case, casing and collation all route
// through it. The GUI uses the Windows user locale; conformance tests and
// deterministic diagnostics use en-US.
//
// Backed by the Windows ICU (icu.dll) and configured the way .NET 5+ on
// Windows configures ICU, so results match the .NET predecessor:
//   * collation: CompareOptions.None / IgnoreCase / IgnoreCase|IgnoreNonSpace
//     collators built like System.Globalization.Native (pal_collation.c);
//   * names: format-context and stand-alone abbreviated months, stand-alone
//     abbreviated weekdays, Gregorian calendar;
//   * casing: simple per-code-point mapping with Turkish/Azeri i rules;
//   * user_default() also honours the user's Control Panel overrides of the
//     number symbols and time format, as .NET does for CurrentCulture.
//
// Thread safety: a Locale is immutable after construction and safe for
// concurrent const use. Each collator is guarded by its own mutex, because
// using one UCollator from several threads at once is not something we rely
// on (a clone per comparison would be far slower for sorting).

#include "wizz/core/time.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wizz {

/// Culture number symbols (.NET NumberFormatInfo subset).
struct NumberSymbols {
    std::string decimal_separator = ".";
    std::string group_separator = ",";
    std::string negative_sign = "-";
    /// NumberGroupSizes, e.g. {3} (en-US) or {3, 2} (en-IN).
    std::vector<int> group_sizes{3};

    friend bool operator==(const NumberSymbols&, const NumberSymbols&) = default;
};

/// Collation strength, mapped to .NET CompareOptions:
/// tertiary = None (CurrentCulture: case- and accent-sensitive),
/// secondary = IgnoreCase (CurrentCultureIgnoreCase: accent-sensitive),
/// primary = IgnoreCase | IgnoreNonSpace (case- and accent-insensitive).
enum class CollationStrength { primary, secondary, tertiary };

/// Which form of a month name: the format (genitive) form used next to a day
/// number, or the stand-alone form (.NET AbbreviatedMonthGenitiveNames vs
/// AbbreviatedMonthNames). They differ in some cultures, e.g. de "Nov." / "Nov".
enum class NameContext { format, standalone };

class Locale {
    struct Impl;
    struct PassKey {
        explicit PassKey() = default;
    };

public:
    /// Internal: use create(), user_default() or en_us().
    Locale(PassKey, std::unique_ptr<Impl> impl) noexcept;

    /// ICU-backed locale from a BCP-47 / Windows locale name ("en-US", "de-DE",
    /// "tr-TR"). Uses the locale's own data only (no user overrides).
    /// Throws std::invalid_argument for an empty or malformed name.
    [[nodiscard]] static std::shared_ptr<const Locale> create(std::string_view name);
    /// The Windows user locale (GetUserDefaultLocaleName) with the user's
    /// overrides of decimal/group separators, negative sign, digit grouping and
    /// time format. Falls back to en-US. Not cached: call once at start-up.
    [[nodiscard]] static std::shared_ptr<const Locale> user_default();
    /// Shared en-US instance used by tests and deterministic diagnostics.
    [[nodiscard]] static const std::shared_ptr<const Locale>& en_us();

    ~Locale();
    Locale(const Locale&) = delete;
    Locale& operator=(const Locale&) = delete;

    /// The name the locale was created from ("en-US").
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const NumberSymbols& number_symbols() const noexcept;
    /// Culture time separator (":" in en-US); what ':' means in a .NET custom
    /// date/time format (derived from the long time pattern, like .NET).
    [[nodiscard]] const std::string& time_separator() const noexcept;
    /// Abbreviated month name, month in 1..12 ("Nov"). Throws std::out_of_range.
    [[nodiscard]] const std::string& month_abbrev(unsigned month, NameContext context = NameContext::format) const;
    /// Abbreviated weekday name, .NET AbbreviatedDayNames (ICU stand-alone short
    /// weekdays): "Fri", de "Fr". Throws std::out_of_range.
    [[nodiscard]] const std::string& day_abbrev(std::chrono::weekday weekday) const;

    /// .NET custom date/time format (DateTime.ToString(pattern, culture)) for the
    /// subset the app uses: d, dd, ddd, M, MM, MMM, y…, H, HH, m, mm, s, ss,
    /// ':' (culture time separator), 'quoted' / "quoted" literals, \x escapes,
    /// %x single specifiers; every other character is literal. MMM takes the
    /// format form when a d/dd token is next to it (.NET IsUseGenitiveForm),
    /// else the stand-alone form. Examples (en-US): "ddd dd MMM yyyy" →
    /// "Fri 20 Nov 2026", "d MMM" → "10 Nov", "yyyy-MM-dd" → "2026-11-02".
    /// Throws std::invalid_argument for specifiers outside the subset (dddd,
    /// MMMM, h, t, f, F, g, K, z, '/') or a malformed pattern.
    [[nodiscard]] std::string format_date(Date date, std::string_view pattern) const;
    /// Same formatter applied to a date-time, e.g. "HH:mm:ss" (log lines) or
    /// "yyyy-MM-ddTHH:mm:ss".
    [[nodiscard]] std::string format_time(LocalDateTime time, std::string_view pattern) const;

    /// Linguistic comparison of two UTF-8 strings (.NET string.Compare with
    /// CurrentCulture[IgnoreCase]): <0, 0, >0.
    [[nodiscard]] int compare(std::string_view a, std::string_view b,
                              CollationStrength strength = CollationStrength::tertiary) const;
    /// .NET text.StartsWith(prefix, CurrentCulture[IgnoreCase]) (03 §5
    /// ReachableNames). An empty prefix matches. Supports tertiary and
    /// secondary strength; primary throws std::invalid_argument.
    [[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix,
                                   CollationStrength strength = CollationStrength::tertiary) const;
    /// TextInfo.ToTitleCase (02 §5.6) applied to UTF-8 text: words start at a
    /// letter, an apostrophe (U+0027) does not end a word, all-upper-case words
    /// are kept, digraphs ǆ ǉ ǌ ǳ become ǅ ǈ ǋ ǲ, nl "ij" becomes "IJ".
    /// Culture-aware (tr-TR: "islands" → "İslands").
    [[nodiscard]] std::string title_case(std::string_view text) const;
    /// Culture upper-case of a whole UTF-8 string (TextInfo.ToUpper), simple
    /// per-code-point mapping (tr/az: i → İ).
    [[nodiscard]] std::string to_upper(std::string_view text) const;

private:
    std::unique_ptr<Impl> impl_;
};

/// Strict weak ordering through Locale::compare, for std::set (a .NET
/// SortedSet with StringComparer.CurrentCulture[IgnoreCase]) and
/// std::ranges::stable_sort. The locale must outlive the comparator.
class CollationLess {
public:
    using is_transparent = void;
    explicit CollationLess(const Locale& locale, CollationStrength strength = CollationStrength::tertiary) noexcept
        : locale_(&locale), strength_(strength)
    {
    }
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const
    {
        return locale_->compare(a, b, strength_) < 0;
    }

private:
    const Locale* locale_;
    CollationStrength strength_;
};

} // namespace wizz
