#pragma once
// The single locale service of 07 §1: month and weekday names, number
// separators, title case and collation all route through it. The GUI uses the
// Windows user locale; conformance tests and diagnostics use en-US.
// Backed by the Windows ICU (icu.dll) so results match .NET-on-ICU.

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
    std::vector<int> group_sizes{3};

    friend bool operator==(const NumberSymbols&, const NumberSymbols&) = default;
};

/// Collation strength. Tertiary = case- and accent-sensitive (CurrentCulture);
/// Secondary = case-insensitive, accent-sensitive (CurrentCultureIgnoreCase).
enum class CollationStrength { primary, secondary, tertiary };

class Locale {
public:
    /// ICU-backed locale from a BCP-47 / Windows locale name ("en-US", "de-DE", "tr-TR").
    [[nodiscard]] static std::shared_ptr<const Locale> create(std::string_view name);
    /// The Windows user locale (GetUserDefaultLocaleName). Falls back to en-US.
    [[nodiscard]] static std::shared_ptr<const Locale> user_default();
    /// Shared en-US instance used by tests and deterministic diagnostics.
    [[nodiscard]] static const std::shared_ptr<const Locale>& en_us();

    ~Locale();
    Locale(const Locale&) = delete;
    Locale& operator=(const Locale&) = delete;

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const NumberSymbols& number_symbols() const noexcept;
    /// Culture time separator (":" in en-US); used where .NET custom formats put ':'.
    [[nodiscard]] const std::string& time_separator() const noexcept;
    /// Abbreviated month name in format context ("Nov"); month in 1..12.
    [[nodiscard]] const std::string& month_abbrev(unsigned month) const;
    /// Abbreviated weekday name ("Fri").
    [[nodiscard]] const std::string& day_abbrev(std::chrono::weekday weekday) const;

    /// .NET custom date pattern subset: d, dd, ddd, M, MM, MMM, yyyy and literal
    /// characters (spaces, '-', '/', '.', ','). Examples: "ddd dd MMM yyyy",
    /// "d MMM", "dd MMM", "dd MMM yyyy", "ddd dd MMM", "yyyy-MM-dd".
    [[nodiscard]] std::string format_date(Date date, std::string_view pattern) const;
    /// .NET custom pattern subset for times: HH, mm, ss with ':' replaced by the
    /// culture time separator (e.g. log lines "HH:mm:ss").
    [[nodiscard]] std::string format_time(LocalDateTime time, std::string_view pattern) const;

    /// Linguistic comparison of two UTF-8 strings: <0, 0, >0.
    [[nodiscard]] int compare(std::string_view a, std::string_view b,
                              CollationStrength strength = CollationStrength::tertiary) const;
    /// TextInfo.ToTitleCase (02 §5.6) applied to UTF-8 text; culture-aware
    /// upper-casing of word starts (tr-TR: "islands" → "İslands").
    [[nodiscard]] std::string title_case(std::string_view text) const;
    /// Culture upper-case of a whole UTF-8 string (ToUpper(CultureInfo)).
    [[nodiscard]] std::string to_upper(std::string_view text) const;

private:
    struct Impl;
    explicit Locale(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace wizz
