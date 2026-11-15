#include "wizz/core/decimal.hpp"

// The arithmetic follows .NET's DecCalc (VarDecAdd/VarDecMul/VarDecDiv,
// ScaleResult, InternalRound) and the parser follows Utf8Parser +
// Number.TryNumberToDecimal, but on a small fixed-width integer instead of
// hand-unrolled 32-bit word arithmetic. Every rounding decision is taken on the
// exact value at the same point as in .NET, so results match bit for bit.

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wizz {
namespace {

constexpr int kMaxScale = Decimal::kMaxScale;

/// .NET scales by at most 10^9 (one 32-bit word) per step.
constexpr int kMaxPowerStep = 9;
constexpr std::array<std::uint32_t, kMaxPowerStep + 1> kPow10{
    1u, 10u, 100u, 1'000u, 10'000u, 100'000u, 1'000'000u, 10'000'000u, 100'000'000u, 1'000'000'000u};

[[noreturn]] void throw_overflow()
{
    throw std::overflow_error("Value was either too large or too small for a Decimal.");
}

/// Unsigned 192-bit integer (six little-endian 32-bit words). Wide enough for
/// every intermediate value .NET forms: a 96-bit mantissa aligned by 10^28 plus
/// a carry (< 2^191), a 96 × 96-bit product (< 2^192) and a division remainder
/// scaled by 10^9 (< 2^126).
class Wide {
public:
    static constexpr std::size_t kWords = 6;

    constexpr Wide() noexcept = default;
    constexpr explicit Wide(std::uint64_t value) noexcept
        : words_{static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32)}
    {
    }
    constexpr explicit Wide(const DecimalBits& bits) noexcept : words_{bits.lo, bits.mid, bits.hi} {}
    constexpr explicit Wide(const std::array<std::uint32_t, kWords>& words) noexcept : words_{words} {}

    [[nodiscard]] constexpr std::uint32_t word(std::size_t index) const noexcept { return words_[index]; }
    [[nodiscard]] constexpr bool is_zero() const noexcept
    {
        return std::ranges::all_of(words_, [](std::uint32_t w) { return w == 0; });
    }
    /// True when the value is below 2^96, i.e. fits a decimal mantissa.
    [[nodiscard]] constexpr bool fits_mantissa() const noexcept
    {
        return std::all_of(words_.begin() + 3, words_.end(), [](std::uint32_t w) { return w == 0; });
    }
    [[nodiscard]] constexpr bool is_odd() const noexcept { return (words_[0] & 1u) != 0; }
    [[nodiscard]] constexpr int bit_length() const noexcept
    {
        for (std::size_t i = kWords; i-- > 0;) {
            if (words_[i] != 0) {
                return static_cast<int>(i * 32 + 32) - std::countl_zero(words_[i]);
            }
        }
        return 0;
    }
    [[nodiscard]] constexpr bool bit(int index) const noexcept
    {
        return ((words_[static_cast<std::size_t>(index / 32)] >> (index % 32)) & 1u) != 0;
    }
    constexpr void set_bit(int index) noexcept
    {
        words_[static_cast<std::size_t>(index / 32)] |= 1u << (index % 32);
    }

    /// *this = *this × factor (callers guarantee that the product fits).
    constexpr void multiply(std::uint32_t factor) noexcept
    {
        std::uint64_t carry = 0;
        for (auto& w : words_) {
            const std::uint64_t t = std::uint64_t{w} * factor + carry;
            w = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
    }
    /// *this = *this × 10^exponent, in steps of at most 10^9.
    constexpr void multiply_pow10(int exponent) noexcept
    {
        for (; exponent > 0; exponent -= kMaxPowerStep) {
            multiply(kPow10[static_cast<std::size_t>(std::min(exponent, kMaxPowerStep))]);
        }
    }
    /// *this = *this / divisor; returns the remainder.
    constexpr std::uint32_t divide(std::uint32_t divisor) noexcept
    {
        std::uint64_t remainder = 0;
        for (std::size_t i = kWords; i-- > 0;) {
            const std::uint64_t current = (remainder << 32) | words_[i];
            words_[i] = static_cast<std::uint32_t>(current / divisor);
            remainder = current % divisor;
        }
        return static_cast<std::uint32_t>(remainder);
    }
    constexpr void shift_left_one() noexcept
    {
        std::uint32_t carry = 0;
        for (auto& w : words_) {
            const std::uint32_t next = w >> 31;
            w = (w << 1) | carry;
            carry = next;
        }
    }

    constexpr Wide& operator+=(const Wide& other) noexcept
    {
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < kWords; ++i) {
            const std::uint64_t t = std::uint64_t{words_[i]} + other.words_[i] + carry;
            words_[i] = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
        return *this;
    }
    /// Requires *this >= other.
    constexpr Wide& operator-=(const Wide& other) noexcept
    {
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < kWords; ++i) {
            const std::uint64_t t = std::uint64_t{words_[i]} - other.words_[i] - borrow;
            words_[i] = static_cast<std::uint32_t>(t);
            borrow = (t >> 32) & 1u;
        }
        return *this;
    }

    friend constexpr bool operator==(const Wide&, const Wide&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Wide& a, const Wide& b) noexcept
    {
        for (std::size_t i = kWords; i-- > 0;) {
            if (a.words_[i] != b.words_[i]) {
                return a.words_[i] <=> b.words_[i];
            }
        }
        return std::strong_ordering::equal;
    }

private:
    std::array<std::uint32_t, kWords> words_{};
};

struct WideDivision {
    Wide quotient;
    Wide remainder;
};

/// Restoring binary long division; operands here are at most 126 bits wide.
constexpr WideDivision divide(const Wide& dividend, const Wide& divisor) noexcept
{
    WideDivision result;
    for (int i = dividend.bit_length() - 1; i >= 0; --i) {
        result.remainder.shift_left_one();
        if (dividend.bit(i)) {
            result.remainder.set_bit(0);
        }
        if (result.remainder >= divisor) {
            result.remainder -= divisor;
            result.quotient.set_bit(i);
        }
    }
    return result;
}

/// Full 96 × 96 → 192-bit product of two mantissas.
constexpr Wide multiply_mantissas(const DecimalBits& a, const DecimalBits& b) noexcept
{
    const std::array<std::uint32_t, 3> x{a.lo, a.mid, a.hi};
    const std::array<std::uint32_t, 3> y{b.lo, b.mid, b.hi};
    std::array<std::uint32_t, Wide::kWords> product{};
    for (std::size_t i = 0; i < x.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < y.size(); ++j) {
            const std::uint64_t t = std::uint64_t{x[i]} * y[j] + product[i + j] + carry;
            product[i + j] = static_cast<std::uint32_t>(t);
            carry = t >> 32;
        }
        product[i + y.size()] = static_cast<std::uint32_t>(carry);
    }
    return Wide{product};
}

Decimal make_decimal(const Wide& mantissa, int scale, bool negative)
{
    return Decimal::from_bits({mantissa.word(0), mantissa.word(1), mantissa.word(2), scale, negative});
}

/// Half-to-even decision from the first dropped digit, whether any later
/// dropped digit was non-zero (`sticky`) and the parity of the kept part.
constexpr bool rounds_up_to_even(std::uint32_t first_dropped, bool sticky, bool odd) noexcept
{
    return first_dropped > 5 || (first_dropped == 5 && (sticky || odd));
}

/// Drops trailing zero digits while the scale allows (.NET Unscale).
constexpr void strip_trailing_zeros(Wide& mantissa, int& scale) noexcept
{
    while (scale > 0) {
        Wide reduced = mantissa;
        if (reduced.divide(10) != 0) {
            return;
        }
        mantissa = reduced;
        --scale;
    }
}

struct Scaled {
    Wide mantissa;
    int scale;
};

/// .NET ScaleResult: brings the exact result `value` × 10^-scale into range
/// (mantissa < 2^96, scale <= 28) by dropping the fewest decimal digits,
/// rounding half to even. Throws std::overflow_error when that would need a
/// negative scale.
Scaled scale_result(Wide value, int scale)
{
    int dropped = 0;
    std::uint32_t first_dropped = 0; // most significant digit dropped so far
    bool sticky = false;             // any less significant dropped digit was non-zero
    const auto drop_digit = [&] {
        sticky = sticky || first_dropped != 0;
        first_dropped = value.divide(10);
        ++dropped;
    };
    while (dropped < scale - kMaxScale) {
        drop_digit();
    }
    while (!value.fits_mantissa()) {
        drop_digit();
    }
    if (dropped > scale) {
        throw_overflow();
    }
    if (dropped > 0 && rounds_up_to_even(first_dropped, sticky, value.is_odd())) {
        value += Wide{1};
        if (!value.fits_mantissa()) {
            // Carried into 2^96: .NET divides by 10 once more and rounds that
            // single digit again, forgetting the earlier sticky state.
            if (dropped == scale) {
                throw_overflow();
            }
            const std::uint32_t digit = value.divide(10);
            ++dropped;
            if (rounds_up_to_even(digit, false, value.is_odd())) {
                value += Wide{1};
            }
        }
    }
    return {value, scale - dropped};
}

/// .NET SearchScale: the largest power p <= min(9, 28 - scale) by which the
/// quotient can be scaled up and still fit 96 bits. A full step of 9 always
/// continues; a shorter step that still leaves the scale negative means the
/// quotient cannot fit (std::overflow_error).
int search_scale(const Wide& quotient, int scale)
{
    const int limit = std::min(kMaxPowerStep, kMaxScale - scale);
    int power = 0;
    Wide scaled = quotient;
    while (power < limit) {
        scaled.multiply(10);
        if (!scaled.fits_mantissa()) {
            break;
        }
        ++power;
    }
    if (power < kMaxPowerStep && power + scale < 0) {
        throw_overflow();
    }
    return power;
}

/// .NET OverflowUnscale: the quotient carried into bit 96; drop one digit,
/// rounding half to even with the outstanding remainder as sticky bit.
int overflow_unscale(Wide& quotient, int scale, bool sticky)
{
    if (--scale < 0) {
        throw_overflow();
    }
    const std::uint32_t digit = quotient.divide(10);
    if (rounds_up_to_even(digit, sticky, quotient.is_odd())) {
        quotient += Wide{1};
    }
    return scale;
}

/// Both mantissas scaled exactly to the larger of the two scales.
struct Aligned {
    Wide a;
    Wide b;
};

constexpr Aligned align(const DecimalBits& a, const DecimalBits& b) noexcept
{
    const int scale = std::max(a.scale, b.scale);
    Aligned aligned{Wide{a}, Wide{b}};
    aligned.a.multiply_pow10(scale - a.scale);
    aligned.b.multiply_pow10(scale - b.scale);
    return aligned;
}

/// .NET VarDecAdd/VarDecSub: exact sum at the larger scale, then ScaleResult
/// when it exceeds 96 bits. `b` already carries the effective sign.
Decimal add_values(const DecimalBits& a, const DecimalBits& b)
{
    const int scale = std::max(a.scale, b.scale);
    auto [x, y] = align(a, b);
    bool negative = a.negative;
    if (a.negative == b.negative) {
        x += y;
    } else if (x >= y) {
        x -= y;
    } else {
        y -= x;
        x = y;
        negative = b.negative;
    }
    if (x.fits_mantissa()) {
        return make_decimal(x, scale, negative);
    }
    const auto [mantissa, reduced] = scale_result(x, scale);
    return make_decimal(mantissa, reduced, negative);
}

/// .NET VarDecMul: exact product at scale a.scale + b.scale, then ScaleResult
/// when it exceeds 96 bits or scale 28.
Decimal multiply_values(const DecimalBits& a, const DecimalBits& b)
{
    const int scale = a.scale + b.scale;
    const Wide product = multiply_mantissas(a, b);
    // Zero results whose scale .NET does not keep: it returns default(decimal)
    // (scale 0) for a 32 × 32-bit product scaled down by more than 10^19 and
    // for any zero product involving a wider operand. A narrow zero product
    // keeps its scale (0 × 1.50 = 0.00).
    const bool narrow = (a.mid | a.hi | b.mid | b.hi) == 0;
    if (narrow ? scale > kMaxScale + 19 : product.is_zero()) {
        return Decimal{};
    }
    const bool negative = a.negative != b.negative;
    if (product.fits_mantissa() && scale <= kMaxScale) {
        return make_decimal(product, scale, negative);
    }
    const auto [mantissa, reduced] = scale_result(product, scale);
    return make_decimal(mantissa, reduced, negative);
}

/// .NET VarDecDiv: integer quotient at the natural scale (a.scale - b.scale),
/// then up to 10^9 more digits at a time while the remainder is non-zero and
/// the quotient still fits, then one half-to-even rounding on the remainder.
Decimal divide_values(const DecimalBits& a, const DecimalBits& b)
{
    const Wide divisor{b};
    if (divisor.is_zero()) {
        throw std::domain_error("Attempted to divide by zero.");
    }
    auto [quotient, remainder] = divide(Wide{a}, divisor);
    int scale = a.scale - b.scale;
    bool inexact = false;
    for (;;) {
        int power = 0;
        if (remainder.is_zero()) {
            if (scale >= 0) {
                break;
            }
            power = std::min(kMaxPowerStep, -scale);
        } else {
            inexact = true;
            power = scale == kMaxScale ? 0 : search_scale(quotient, scale);
            if (power == 0) {
                Wide twice = remainder;
                twice.shift_left_one();
                const auto half = twice <=> divisor;
                if (half > 0 || (half == 0 && quotient.is_odd())) {
                    quotient += Wide{1};
                    if (!quotient.fits_mantissa()) {
                        scale = overflow_unscale(quotient, scale, true);
                    }
                }
                break;
            }
        }
        scale += power;
        quotient.multiply_pow10(power);
        if (!quotient.fits_mantissa()) {
            throw_overflow();
        }
        remainder.multiply_pow10(power);
        auto [digits, rest] = divide(remainder, divisor);
        remainder = rest;
        quotient += digits;
        if (!quotient.fits_mantissa()) {
            scale = overflow_unscale(quotient, scale, !remainder.is_zero());
            break;
        }
    }
    if (inexact) {
        strip_trailing_zeros(quotient, scale);
    }
    return make_decimal(quotient, scale, a.negative != b.negative);
}

/// Decimal digits of the mantissa without leading zeros ("" for zero).
std::string mantissa_digits(Wide value)
{
    std::string digits;
    while (!value.is_zero()) {
        digits.push_back(static_cast<char>('0' + value.divide(10)));
    }
    std::ranges::reverse(digits);
    return digits;
}

// ---------------------------------------------------------------------------
// Parsing (Utf8Parser.TryParseNumber + Number.TryNumberToDecimal)

constexpr bool is_digit(char ch) noexcept
{
    return ch >= '0' && ch <= '9';
}

/// Inputs this long cannot be decimals; refusing them keeps every digit count in `int`.
constexpr std::size_t kMaxTextLength = static_cast<std::size_t>(std::numeric_limits<int>::max() / 2);

/// Lexical parts of a number literal.
struct NumberParts {
    bool negative = false;
    std::string_view integer_digits;
    std::string_view fraction_digits;
    bool exponent_negative = false;
    std::string_view exponent_digits; ///< empty when there is no exponent
};

/// Reads a run of ASCII digits starting at `pos`.
constexpr std::string_view take_digits(std::string_view text, std::size_t& pos) noexcept
{
    const std::size_t start = pos;
    while (pos < text.size() && is_digit(text[pos])) {
        ++pos;
    }
    return text.substr(start, pos - start);
}

/// Optional exponent `[eE][+-]?digits` at `pos`; false when malformed.
constexpr bool take_exponent(std::string_view text, std::size_t& pos, NumberParts& parts) noexcept
{
    if (pos == text.size() || (text[pos] != 'e' && text[pos] != 'E')) {
        return true;
    }
    ++pos;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        parts.exponent_negative = text[pos] == '-';
        ++pos;
    }
    parts.exponent_digits = take_digits(text, pos);
    return !parts.exponent_digits.empty();
}

/// RFC 8259: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
std::optional<NumberParts> split_json_number(std::string_view text)
{
    if (text.size() > kMaxTextLength) {
        return std::nullopt;
    }
    NumberParts parts;
    std::size_t pos = 0;
    if (pos < text.size() && text[pos] == '-') {
        parts.negative = true;
        ++pos;
    }
    parts.integer_digits = take_digits(text, pos);
    if (parts.integer_digits.empty() || (parts.integer_digits.size() > 1 && parts.integer_digits.front() == '0')) {
        return std::nullopt;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        parts.fraction_digits = take_digits(text, pos);
        if (parts.fraction_digits.empty()) {
            return std::nullopt;
        }
    }
    if (!take_exponent(text, pos, parts) || pos != text.size()) {
        return std::nullopt;
    }
    return parts;
}

/// .NET NumberStyles.Float with the invariant culture.
std::optional<NumberParts> split_invariant_number(std::string_view text)
{
    // .NET white space for number parsing: U+0009..U+000D and U+0020.
    constexpr std::string_view kWhite = " \t\n\v\f\r";
    const auto first = text.find_first_not_of(kWhite);
    if (first == std::string_view::npos || text.size() > kMaxTextLength) {
        return std::nullopt;
    }
    text = text.substr(first, text.find_last_not_of(kWhite) - first + 1);
    NumberParts parts;
    std::size_t pos = 0;
    if (text[pos] == '+' || text[pos] == '-') {
        parts.negative = text[pos] == '-';
        ++pos;
    }
    parts.integer_digits = take_digits(text, pos);
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        parts.fraction_digits = take_digits(text, pos);
    }
    if ((parts.integer_digits.empty() && parts.fraction_digits.empty()) || !take_exponent(text, pos, parts) ||
        pos != text.size()) {
        return std::nullopt;
    }
    return parts;
}

/// .NET Number.NumberBuffer of kind Decimal: value = 0.d1d2d3… × 10^scale.
struct NumberBuffer {
    static constexpr int kMaxDigits = 30; // DecimalNumberBufferLength - 1
    std::array<char, kMaxDigits + 1> digits{}; // NUL-terminated
    int scale = 0;
    bool negative = false;
    bool has_non_zero_tail = false;
};

/// Utf8Parser.TryParseNumber: skips leading zeros, stores up to 30 digits and
/// remembers whether a non-zero digit was seen at digit count >= 30. The count
/// starts after the leading integer zeros but includes leading fraction zeros
/// when the integer part is zero, and the 30th digit is both stored and counted
/// as tail — quirks the rounding below depends on.
NumberBuffer to_number_buffer(const NumberParts& parts)
{
    NumberBuffer number;
    number.negative = parts.negative;
    int stored = 0;
    int digit_count = 0;
    const auto count = [&](char ch) {
        ++digit_count;
        if (digit_count >= NumberBuffer::kMaxDigits && ch != '0') {
            number.has_non_zero_tail = true;
        }
    };
    const auto store = [&](std::string_view digits) {
        const auto n = std::min(digits.size(), static_cast<std::size_t>(NumberBuffer::kMaxDigits - stored));
        std::ranges::copy(digits.substr(0, n), number.digits.begin() + stored);
        stored += static_cast<int>(n);
    };

    auto integer = parts.integer_digits;
    integer.remove_prefix(std::min(integer.find_first_not_of('0'), integer.size()));
    std::ranges::for_each(integer, count);
    store(integer);
    number.scale = static_cast<int>(integer.size());

    auto fraction = parts.fraction_digits;
    std::ranges::for_each(fraction, count);
    if (stored == 0) {
        const auto zeros = std::min(fraction.find_first_not_of('0'), fraction.size());
        number.scale -= static_cast<int>(zeros);
        fraction.remove_prefix(zeros);
    }
    store(fraction);

    if (!parts.exponent_digits.empty()) {
        // TryParseUInt32D; an exponent beyond uint.MaxValue saturates.
        std::int64_t exponent = 0;
        for (const char ch : parts.exponent_digits) {
            exponent = exponent * 10 + (ch - '0');
            if (exponent > std::numeric_limits<std::uint32_t>::max()) {
                exponent = std::numeric_limits<std::uint32_t>::max();
                break;
            }
        }
        const std::int64_t scale = number.scale;
        if (parts.exponent_negative) {
            number.scale = scale < std::int64_t{INT_MIN} + exponent ? INT_MIN : static_cast<int>(scale - exponent);
        } else {
            number.scale = scale > std::int64_t{INT_MAX} - exponent ? INT_MAX : static_cast<int>(scale + exponent);
        }
    }
    return number;
}

/// Number.TryNumberToDecimal: accumulates up to 29 digits while the value fits
/// 96 bits and the scale stays <= 28, then rounds half to even on the next
/// digit and the tail. nullopt when the magnitude does not fit.
std::optional<Decimal> number_to_decimal(const NumberBuffer& number)
{
    constexpr int kDecimalPrecision = 29;
    const char* p = number.digits.data();
    int e = number.scale;
    auto c = static_cast<std::uint32_t>(*p);
    if (c == 0) {
        // (byte)Math.Clamp(-e, 0, 28) with C#'s wrapping negation of int.MinValue.
        const std::int64_t negated = e == INT_MIN ? std::int64_t{INT_MIN} : -std::int64_t{e};
        return Decimal::from_bits({.scale = static_cast<int>(std::clamp<std::int64_t>(negated, 0, kMaxScale)),
                                   .negative = number.negative});
    }
    if (e > kDecimalPrecision) {
        return std::nullopt;
    }

    constexpr std::uint64_t kLowLimit = std::numeric_limits<std::uint64_t>::max() / 10;
    std::uint64_t low64 = 0;
    while (e > -kMaxScale) {
        --e;
        low64 = low64 * 10 + (c - '0');
        c = static_cast<std::uint32_t>(*++p);
        if (low64 >= kLowLimit) {
            break;
        }
        if (c == 0) {
            while (e > 0) {
                --e;
                low64 *= 10;
                if (low64 >= kLowLimit) {
                    break;
                }
            }
            break;
        }
    }

    constexpr std::uint32_t kHighLimit = std::numeric_limits<std::uint32_t>::max() / 10;
    constexpr std::uint64_t kLowAtHighLimit = 0x9999'9999'9999'9999u;
    std::uint32_t high = 0;
    while ((e > 0 || (c != 0 && e > -kMaxScale)) &&
           (high < kHighLimit ||
            (high == kHighLimit && (low64 < kLowAtHighLimit || (low64 == kLowAtHighLimit && c <= '5'))))) {
        // (high:low64) × 10
        const std::uint64_t low_product = (low64 & 0xFFFF'FFFFu) * 10u;
        const std::uint64_t high_product = (low64 >> 32) * 10u + (low_product >> 32);
        low64 = (low_product & 0xFFFF'FFFFu) + (high_product << 32);
        high = static_cast<std::uint32_t>(high_product >> 32) + high * 10;
        if (c != 0) {
            c -= '0';
            low64 += c;
            if (low64 < c) {
                ++high;
            }
            c = static_cast<std::uint32_t>(*++p);
        }
        --e;
    }

    if (c >= '5') {
        bool round_up = true;
        if (c == '5' && (low64 & 1u) == 0) {
            bool zero_tail = !number.has_non_zero_tail;
            for (c = static_cast<std::uint32_t>(*++p); c != 0 && zero_tail; c = static_cast<std::uint32_t>(*++p)) {
                zero_tail = c == '0';
            }
            round_up = !zero_tail; // exactly half and even: keep
        }
        if (round_up && ++low64 == 0 && ++high == 0) {
            // Carried into 2^96: one digit less, 2^96 / 10 rounded up
            // (7922816251426433759354395034).
            low64 = 0x9999'9999'9999'999Au;
            high = kHighLimit;
            ++e;
        }
    }

    if (e > 0) {
        return std::nullopt;
    }
    if (e <= -kDecimalPrecision) {
        return Decimal::from_bits({.scale = kMaxScale, .negative = number.negative});
    }
    return Decimal::from_bits({static_cast<std::uint32_t>(low64), static_cast<std::uint32_t>(low64 >> 32), high, -e,
                               number.negative});
}

// ---------------------------------------------------------------------------
// Custom numeric format (Number.NumberToStringFormat, single section)

struct NumericPattern {
    int max_fraction = 0; ///< placeholders after the decimal point
    int min_fraction = 0; ///< up to the last '0' after the decimal point
    int min_integer = 0;  ///< from the first '0' to the decimal point
    bool grouping = false;
    int scale_shift = 0; ///< -3 per ',' right before the decimal point
};

NumericPattern parse_pattern(std::string_view pattern)
{
    int digit_count = 0;
    int decimal_pos = -1;
    int first_zero = INT_MAX;
    int last_zero = 0;
    int thousand_pos = -1;
    int thousand_count = 0;
    bool grouping = false;
    for (const char ch : pattern) {
        switch (ch) {
        case '#':
            ++digit_count;
            break;
        case '0':
            first_zero = std::min(first_zero, digit_count);
            ++digit_count;
            last_zero = digit_count;
            break;
        case '.':
            if (decimal_pos < 0) {
                decimal_pos = digit_count;
            }
            break;
        case ',':
            // Only commas after a placeholder and before the point count.
            if (digit_count > 0 && decimal_pos < 0) {
                if (thousand_pos == digit_count) {
                    ++thousand_count;
                } else {
                    grouping = grouping || thousand_pos >= 0;
                    thousand_pos = digit_count;
                    thousand_count = 1;
                }
            }
            break;
        default:
            throw std::invalid_argument("Unsupported character in numeric format pattern: " + std::string{pattern});
        }
    }
    if (digit_count == 0) {
        throw std::invalid_argument("Numeric format pattern has no digit placeholder: " + std::string{pattern});
    }
    if (decimal_pos < 0) {
        decimal_pos = digit_count;
    }
    NumericPattern spec{.max_fraction = digit_count - decimal_pos,
                        .min_fraction = std::max(last_zero - decimal_pos, 0),
                        .min_integer = first_zero < decimal_pos ? decimal_pos - first_zero : 0,
                        .grouping = grouping};
    if (thousand_pos >= 0) {
        if (thousand_pos == decimal_pos) {
            spec.scale_shift = -3 * thousand_count;
        } else {
            spec.grouping = true;
        }
    }
    return spec;
}

/// Number.RoundNumber on a digit string (value = 0.digits × 10^exponent):
/// keeps `position` digits, rounding half away from zero, and drops trailing
/// zeros. A result of zero clears the sign.
void round_digits(std::string& digits, int& exponent, bool& negative, int position)
{
    std::size_t kept = position <= 0 ? 0 : std::min(static_cast<std::size_t>(position), digits.size());
    if (position >= 0 && kept == static_cast<std::size_t>(position) && kept < digits.size() && digits[kept] >= '5') {
        while (kept > 0 && digits[kept - 1] == '9') {
            --kept;
        }
        if (kept > 0) {
            ++digits[kept - 1];
        } else {
            ++exponent;
            digits[0] = '1';
            kept = 1;
        }
    } else {
        while (kept > 0 && digits[kept - 1] == '0') {
            --kept;
        }
    }
    digits.resize(kept);
    if (kept == 0) {
        negative = false;
        exponent = 0;
    }
}

/// Inserts the culture's group separator per NumberGroupSizes (the last size
/// repeats; a size of 0 or less stops grouping).
std::string group_digits(std::string_view integer, const NumberSymbols& symbols)
{
    if (symbols.group_separator.empty() || symbols.group_sizes.empty()) {
        return std::string{integer};
    }
    std::vector<std::size_t> cuts; // separator after this many digits from the right
    std::size_t index = 0;
    int size = symbols.group_sizes.front();
    for (std::size_t total = static_cast<std::size_t>(std::max(size, 0)); size > 0 && integer.size() > total;
         total += static_cast<std::size_t>(std::max(size, 0))) {
        cuts.push_back(total);
        if (index + 1 < symbols.group_sizes.size()) {
            size = symbols.group_sizes[++index];
        }
    }
    std::string grouped;
    for (std::size_t i = 0; i < integer.size(); ++i) {
        grouped += integer[i];
        if (std::ranges::find(cuts, integer.size() - i - 1) != cuts.end()) {
            grouped += symbols.group_separator;
        }
    }
    return grouped;
}

} // namespace

Decimal Decimal::from_bits(const DecimalBits& bits)
{
    if (bits.scale < 0 || bits.scale > kMaxScale) {
        throw std::invalid_argument("Decimal scale must be between 0 and 28.");
    }
    return Decimal{bits.lo, bits.mid, bits.hi, bits.scale, bits.negative};
}

std::expected<Decimal, DecimalParseError> Decimal::from_json_number(std::string_view lexeme)
{
    const auto parts = split_json_number(lexeme);
    if (!parts) {
        return std::unexpected(DecimalParseError::invalid_syntax);
    }
    if (auto value = number_to_decimal(to_number_buffer(*parts))) {
        return *value;
    }
    return std::unexpected(DecimalParseError::out_of_range);
}

std::optional<Decimal> Decimal::parse(std::string_view text)
{
    const auto parts = split_invariant_number(text);
    if (!parts) {
        return std::nullopt;
    }
    return number_to_decimal(to_number_buffer(*parts));
}

Decimal Decimal::round(int decimals, Rounding mode) const
{
    if (decimals < 0 || decimals > kMaxScale) {
        throw std::out_of_range("Decimal can only round to between 0 and 28 digits of precision.");
    }
    if (scale_ <= decimals) {
        return *this;
    }
    Wide mantissa{bits()};
    std::uint32_t first_dropped = 0;
    bool sticky = false;
    for (int i = scale_ - decimals; i > 0; --i) {
        sticky = sticky || first_dropped != 0;
        first_dropped = mantissa.divide(10);
    }
    const bool up = mode == Rounding::away_from_zero ? first_dropped >= 5
                                                     : rounds_up_to_even(first_dropped, sticky, mantissa.is_odd());
    if (up) {
        mantissa += Wide{1};
    }
    return make_decimal(mantissa, decimals, negative_);
}

std::string Decimal::to_string() const
{
    static const NumberSymbols invariant{}; // "." and "-", like CultureInfo.InvariantCulture
    return to_string(invariant);
}

std::string Decimal::to_string(const NumberSymbols& symbols) const
{
    const std::string digits = mantissa_digits(Wide{bits()});
    const int integer_count = static_cast<int>(digits.size()) - scale_;
    std::string text = negative_ ? symbols.negative_sign : std::string{};
    if (integer_count > 0) {
        text.append(digits, 0, static_cast<std::size_t>(integer_count));
    } else {
        text += '0';
    }
    if (scale_ > 0) {
        text += symbols.decimal_separator;
        if (integer_count < 0) {
            text.append(static_cast<std::size_t>(-integer_count), '0');
        }
        text.append(digits, static_cast<std::size_t>(std::max(integer_count, 0)));
    }
    return text;
}

std::string Decimal::format(std::string_view pattern, const NumberSymbols& symbols) const
{
    const NumericPattern spec = parse_pattern(pattern);
    std::string digits = mantissa_digits(Wide{bits()});
    bool negative = negative_;
    int exponent = 0; // value = 0.digits × 10^exponent
    if (!digits.empty()) {
        exponent = static_cast<int>(digits.size()) - scale_ + spec.scale_shift;
        round_digits(digits, exponent, negative, exponent + spec.max_fraction);
    }

    std::string integer;
    if (exponent > 0) {
        const auto count = static_cast<std::size_t>(exponent);
        integer.assign(digits, 0, std::min(count, digits.size()));
        integer.resize(count, '0');
    }
    if (std::cmp_less(integer.size(), spec.min_integer)) {
        integer.insert(0, static_cast<std::size_t>(spec.min_integer) - integer.size(), '0');
    }

    std::string fraction;
    if (exponent < 0) {
        fraction.assign(static_cast<std::size_t>(-exponent), '0');
    }
    const auto integer_digits = static_cast<std::size_t>(std::max(exponent, 0));
    if (digits.size() > integer_digits) {
        fraction.append(digits, integer_digits);
    }
    if (std::cmp_less(fraction.size(), spec.min_fraction)) {
        fraction.resize(static_cast<std::size_t>(spec.min_fraction), '0');
    }

    std::string text = negative ? symbols.negative_sign : std::string{};
    text += spec.grouping ? group_digits(integer, symbols) : integer;
    if (!fraction.empty()) {
        text += symbols.decimal_separator;
        text += fraction;
    }
    return text;
}

double Decimal::to_double() const noexcept
{
    // VarR8FromDec: ((double)low64 + (double)high × 2^64) / 10^scale.
    constexpr std::array<double, kMaxScale + 1> kPowers{1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
                                                        1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
                                                        1e20, 1e21, 1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28};
    constexpr double kTwoTo64 = 1.8446744073709552e19;
    const std::uint64_t low64 = (std::uint64_t{mid_} << 32) | lo_;
    const double magnitude = (static_cast<double>(low64) + static_cast<double>(hi_) * kTwoTo64) / kPowers[scale_];
    return negative_ ? -magnitude : magnitude;
}

std::size_t Decimal::hash() const noexcept
{
    Wide mantissa{bits()};
    int scale = scale_;
    strip_trailing_zeros(mantissa, scale);
    // FNV-1a over the canonical (trailing-zero free) representation.
    std::uint64_t h = 0xCBF2'9CE4'8422'2325u;
    for (const std::uint32_t part : {mantissa.word(0), mantissa.word(1), mantissa.word(2),
                                     static_cast<std::uint32_t>(scale), static_cast<std::uint32_t>(negative_)}) {
        h = (h ^ part) * 0x0000'0100'0000'01B3u;
    }
    return static_cast<std::size_t>(h);
}

Decimal operator+(Decimal a, Decimal b)
{
    return add_values(a.bits(), b.bits());
}

Decimal operator-(Decimal a, Decimal b)
{
    return add_values(a.bits(), (-b).bits());
}

Decimal operator*(Decimal a, Decimal b)
{
    return multiply_values(a.bits(), b.bits());
}

Decimal operator/(Decimal a, Decimal b)
{
    return divide_values(a.bits(), b.bits());
}

std::ostream& operator<<(std::ostream& out, const Decimal& value)
{
    return out << value.to_string();
}

bool operator==(Decimal a, Decimal b) noexcept
{
    return (a <=> b) == 0;
}

std::weak_ordering operator<=>(Decimal a, Decimal b) noexcept
{
    if (a.negative_ != b.negative_) {
        return a.negative_ ? std::weak_ordering::less : std::weak_ordering::greater; // zero is never negative
    }
    const auto [x, y] = align(a.bits(), b.bits());
    const auto magnitude = x <=> y;
    return a.negative_ ? 0 <=> magnitude : magnitude;
}

} // namespace wizz
