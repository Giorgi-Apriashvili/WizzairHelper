#pragma once
// Exact decimal arithmetic: a faithful emulation of .NET System.Decimal
// (06 §3, 03 §7.1). A value is (-1)^sign × mantissa × 10^-scale with a 96-bit
// unsigned mantissa and a scale of 0..28.
//
// - The scale is kept for printing ("30790.0" stays "30790.0", 04 §9.4) but is
//   ignored by comparison and hashing (1.0 == 1.00, 02 §2.7).
// - Results that do not fit are rounded half to even exactly where .NET rounds
//   them, so quotients match .NET digit for digit
//   (19290 / 366.3 = 52.661752661752661752661752662, 03 §7.1).
// - Money and FX rates never pass through double: parse them from the JSON
//   literal text with `from_json_number` (04 §9.4, 06 §3).
//
// Zero is always non-negative here. .NET can carry a negative zero, but no
// formatting or comparison ever exposes its sign.

#include "wizz/core/locale.hpp"

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace wizz {

/// Midpoint handling of `Decimal::round` (.NET MidpointRounding subset).
enum class Rounding {
    to_even,        ///< banker's rounding (MidpointRounding.ToEven)
    away_from_zero, ///< money rounding (MidpointRounding.AwayFromZero; 03 §6.2, 07 §2)
};

/// Why `Decimal::from_json_number` rejected a lexeme.
enum class DecimalParseError {
    invalid_syntax, ///< not an RFC 8259 JSON number
    out_of_range,   ///< magnitude beyond ~7.9e28: .NET GetDecimal throws FormatException (04 §9.4)
};

/// Raw representation, like decimal.GetBits:
/// value = (-1)^negative × (hi·2^64 + mid·2^32 + lo) × 10^-scale.
struct DecimalBits {
    std::uint32_t lo = 0;
    std::uint32_t mid = 0;
    std::uint32_t hi = 0;
    int scale = 0;
    bool negative = false;

    friend bool operator==(const DecimalBits&, const DecimalBits&) = default;
};

/// Integer types that convert implicitly to `Decimal` (C# int/long → decimal).
/// Excludes bool and the character types.
template <class T>
concept DecimalInteger =
    std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, wchar_t> &&
    !std::same_as<T, char8_t> && !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

class Decimal {
public:
    static constexpr int kMaxScale = 28;

    /// Zero with scale 0.
    constexpr Decimal() noexcept = default;

    /// Exact integer value with scale 0. Implicit, like C#'s int → decimal, so
    /// `per_person * adults` reads as in the spec (03 §6.2).
    template <DecimalInteger T>
    constexpr Decimal(T value) noexcept // NOLINT(google-explicit-constructor)
    {
        auto magnitude = static_cast<std::uint64_t>(value);
        if constexpr (std::is_signed_v<T>) {
            if (value < 0) {
                magnitude = 0 - magnitude;
                negative_ = true;
            }
        }
        lo_ = static_cast<std::uint32_t>(magnitude);
        mid_ = static_cast<std::uint32_t>(magnitude >> 32);
    }

    /// Value from raw parts (decimal(lo, mid, hi, isNegative, scale)).
    /// Throws std::invalid_argument unless 0 <= scale <= 28.
    [[nodiscard]] static Decimal from_bits(const DecimalBits& bits);

    /// decimal.MaxValue = 79228162514264337593543950335 (the `DECIMAL_MAX` sort key of 03 §8).
    [[nodiscard]] static constexpr Decimal max_value() noexcept
    {
        return Decimal{0xFFFF'FFFFu, 0xFFFF'FFFFu, 0xFFFF'FFFFu, 0, false};
    }
    /// decimal.MinValue = -79228162514264337593543950335.
    [[nodiscard]] static constexpr Decimal min_value() noexcept
    {
        return Decimal{0xFFFF'FFFFu, 0xFFFF'FFFFu, 0xFFFF'FFFFu, 0, true};
    }

    /// System.Text.Json `GetDecimal` on a JSON number lexeme (04 §9.4): exact,
    /// scale preserved ("30790.0" → 30790.0), exponents applied ("1.5e2" → 150),
    /// digits beyond the 28–29 that fit rounded half to even (including .NET's
    /// Utf8Parser quirk that a 30th significant digit counts as a non-zero tail).
    /// The lexeme must match the RFC 8259 number grammar exactly (no white space).
    [[nodiscard]] static std::expected<Decimal, DecimalParseError> from_json_number(std::string_view lexeme);

    /// Invariant plain text (decimal.Parse with NumberStyles.Float and the
    /// invariant culture): optional surrounding white space, optional leading
    /// '+'/'-', digits with an optional '.', optional exponent. Used for
    /// literals such as the built-in FX table (03 §7.2). Conversion and rounding
    /// are those of `from_json_number`. nullopt when malformed or out of range.
    [[nodiscard]] static std::optional<Decimal> parse(std::string_view text);

    [[nodiscard]] constexpr DecimalBits bits() const noexcept { return {lo_, mid_, hi_, scale_, negative_}; }
    /// Number of decimal places carried (0..28).
    [[nodiscard]] constexpr int scale() const noexcept { return scale_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return (lo_ | mid_ | hi_) == 0; }
    /// -1, 0 or 1.
    [[nodiscard]] constexpr int signum() const noexcept { return is_zero() ? 0 : (negative_ ? -1 : 1); }

    /// Math.Round(value, decimals, mode). The result keeps
    /// min(scale(), decimals) decimal places (52.661752… → 52.66, 5490.837 → 5491).
    /// Throws std::out_of_range unless 0 <= decimals <= 28.
    [[nodiscard]] Decimal round(int decimals, Rounding mode) const;

    /// decimal.ToString(CultureInfo.InvariantCulture): every carried decimal
    /// place, no exponent ("29.99", "30790.0", "-2.35", "0").
    [[nodiscard]] std::string to_string() const;
    /// decimal.ToString(culture): as `to_string()` with the culture's decimal
    /// separator and negative sign (the grids' "raw decimal default string", 01 §8.10).
    [[nodiscard]] std::string to_string(const NumberSymbols& symbols) const;

    /// decimal.ToString(pattern, culture) for .NET custom numeric patterns made of
    /// '#', '0', '.' and ',' ("#,##0", "#,##0.00", "0.##", "0"; 03 §6.4).
    /// Rounds half away from zero on the decimal digits, uses the culture's
    /// separators, group sizes and negative sign, and prints no sign when the
    /// value rounds to zero ((-0.004, "#,##0.00") → "0.00", 02 §2.7). As in .NET,
    /// a ',' right before the decimal point (or at the end) divides by 1000.
    /// Throws std::invalid_argument for other characters or a pattern without
    /// any '#'/'0' placeholder.
    [[nodiscard]] std::string format(std::string_view pattern, const NumberSymbols& symbols) const;

    /// (double)value, as .NET converts it. Diagnostics only — never for money.
    [[nodiscard]] double to_double() const noexcept;

    /// Consistent with ==: equal values hash equally regardless of scale.
    [[nodiscard]] std::size_t hash() const noexcept;

    /// .NET decimal operators: exact when the result fits, otherwise rounded
    /// half to even to the most digits that fit (scale <= 28). Throw
    /// std::overflow_error when the magnitude exceeds max_value(); division by
    /// zero throws std::domain_error. Exact quotients keep the natural scale
    /// (1.00 / 2 = 0.50); inexact ones drop trailing zeros (1 / 4 = 0.25).
    [[nodiscard]] friend Decimal operator+(Decimal a, Decimal b);
    [[nodiscard]] friend Decimal operator-(Decimal a, Decimal b);
    [[nodiscard]] friend Decimal operator*(Decimal a, Decimal b);
    [[nodiscard]] friend Decimal operator/(Decimal a, Decimal b);
    [[nodiscard]] friend constexpr Decimal operator-(Decimal a) noexcept
    {
        return Decimal{a.lo_, a.mid_, a.hi_, a.scale_, !a.negative_};
    }

    Decimal& operator+=(Decimal other) { return *this = *this + other; }
    Decimal& operator-=(Decimal other) { return *this = *this - other; }
    Decimal& operator*=(Decimal other) { return *this = *this * other; }
    Decimal& operator/=(Decimal other) { return *this = *this / other; }

    /// Numeric comparison; the scale is ignored (30790 == 30790.0).
    [[nodiscard]] friend bool operator==(Decimal a, Decimal b) noexcept;
    [[nodiscard]] friend std::weak_ordering operator<=>(Decimal a, Decimal b) noexcept;

private:
    constexpr Decimal(std::uint32_t lo, std::uint32_t mid, std::uint32_t hi, int scale, bool negative) noexcept
        : lo_{lo}, mid_{mid}, hi_{hi}, scale_{static_cast<std::uint8_t>(scale)},
          negative_{negative && (lo | mid | hi) != 0}
    {
    }

    std::uint32_t lo_ = 0;
    std::uint32_t mid_ = 0;
    std::uint32_t hi_ = 0;
    std::uint8_t scale_ = 0;
    bool negative_ = false;
};

/// Writes `value.to_string()` (invariant); lets test frameworks and logs print decimals.
std::ostream& operator<<(std::ostream& out, const Decimal& value);

} // namespace wizz

namespace std {
template <>
struct hash<wizz::Decimal> {
    [[nodiscard]] std::size_t operator()(const wizz::Decimal& value) const noexcept { return value.hash(); }
};
} // namespace std
