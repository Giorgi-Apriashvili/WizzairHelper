#include "wizz/core/format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>

namespace wizz {

namespace {

/// .NET formats a double with a custom format from 15 significant digits (07 §2).
constexpr int kLabelSignificantDigits = 15;
constexpr int kMaxLabelDecimals = 15;

constexpr std::int64_t kTicksPerSecond = 10'000'000;
constexpr std::int64_t kTicksPerMinute = 60 * kTicksPerSecond;
constexpr std::int64_t kTicksPerHour = 60 * kTicksPerMinute;

/// Remaining-time estimates beyond this (≈ 31 700 years) are clamped so the
/// integer conversions below stay defined.
constexpr double kMaxRemainingSeconds = 1e12;

/// Adds one to a non-negative decimal digit string ("" counts as 0).
void increment_digits(std::string& digits)
{
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (*it != '9') {
            ++*it;
            return;
        }
        *it = '0';
    }
    digits.insert(digits.begin(), '1');
}

/// Digits of round(magnitude × 10^decimals) after the 15-significant-digit step,
/// halves away from zero, without leading zeros ("0" for zero).
[[nodiscard]] std::string rounded_label_digits(double magnitude, int decimals)
{
    if (magnitude == 0.0) {
        return "0";
    }
    // "d.dddddddddddddde±XX": the exact binary value correctly rounded to 15 digits.
    const std::string scientific = std::format("{:.{}e}", magnitude, kLabelSignificantDigits - 1);
    const auto e_pos = scientific.find('e');
    const std::string significand = scientific.substr(0, 1) + scientific.substr(2, e_pos - 2);
    const int exponent_magnitude = std::stoi(scientific.substr(e_pos + 2));
    const int exponent = scientific[e_pos + 1] == '-' ? -exponent_magnitude : exponent_magnitude;

    // magnitude × 10^decimals == significand × 10^shift
    const int shift = exponent - (kLabelSignificantDigits - 1) + decimals;
    std::string digits;
    if (shift >= 0) {
        digits = significand + std::string(static_cast<std::size_t>(shift), '0');
    } else {
        const int kept = kLabelSignificantDigits + shift;
        if (kept < 0) {
            return "0";  // even the first dropped digit is a leading zero
        }
        digits = significand.substr(0, static_cast<std::size_t>(kept));
        if (significand[static_cast<std::size_t>(kept)] >= '5') {
            increment_digits(digits);
        }
    }
    const auto first_non_zero = digits.find_first_not_of('0');
    return first_non_zero == std::string::npos ? "0" : digits.substr(first_non_zero);
}

} // namespace

std::string format_label(double value, int decimals, const NumberSymbols& symbols)
{
    if (decimals < 0 || decimals > kMaxLabelDecimals) {
        throw std::invalid_argument("format_label supports 0 to 15 decimals.");
    }
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? std::string{"∞"} : symbols.negative_sign + "∞";
    }
    std::string digits = rounded_label_digits(std::abs(value), decimals);
    const bool negative = std::signbit(value) && digits != "0";
    const auto fraction_length = static_cast<std::size_t>(decimals);
    if (digits.size() <= fraction_length) {
        digits.insert(0, fraction_length + 1 - digits.size(), '0');
    }
    std::string text = negative ? symbols.negative_sign : std::string{};
    text.append(digits, 0, digits.size() - fraction_length);
    if (fraction_length > 0) {
        text += symbols.decimal_separator;
        text.append(digits, digits.size() - fraction_length);
    }
    return text;
}

double round_half_even(double value) noexcept
{
    if (std::abs(value - std::trunc(value)) == 0.5) {
        return 2.0 * std::round(value / 2.0);  // the even neighbour of an exact half
    }
    return std::round(value);
}

std::string format_duration(Duration duration)
{
    const std::int64_t ticks = duration.count();
    const double total_minutes = static_cast<double>(ticks) / kTicksPerMinute;
    if (total_minutes >= 1.0) {
        const auto minutes = static_cast<std::int64_t>(total_minutes);
        const std::int64_t seconds = (ticks / kTicksPerSecond) % 60;
        return std::format("{} min {} s", minutes, seconds);
    }
    const double rounded = round_half_even(static_cast<double>(ticks) / kTicksPerSecond);
    return std::format("{} s", rounded > 1.0 ? static_cast<std::int64_t>(rounded) : 1);
}

std::string format_elapsed(Duration duration)
{
    const std::int64_t ticks = duration.count();
    const std::uint64_t magnitude = ticks < 0 ? 0 - static_cast<std::uint64_t>(ticks) : static_cast<std::uint64_t>(ticks);
    const std::uint64_t seconds = magnitude / kTicksPerSecond % 60;
    const std::uint64_t minutes = magnitude / kTicksPerMinute % 60;
    if (static_cast<double>(ticks) / kTicksPerHour >= 1.0) {
        const std::uint64_t hours = magnitude / kTicksPerHour % 24;
        return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
    }
    return std::format("{}:{:02}", minutes, seconds);
}

std::string format_remaining(double seconds)
{
    if (!(seconds >= 60.0)) {  // also NaN, which ends up as "1 s"
        const double up = std::ceil(seconds);
        return std::format("{} s", up > 1.0 ? static_cast<int>(up) : 1);
    }
    const double clamped = std::min(seconds, kMaxRemainingSeconds);
    const auto minutes = static_cast<std::int64_t>(clamped / 60);
    const auto second_component = static_cast<std::int64_t>(clamped) % 60;
    return std::format("{} min {:02} s", minutes, second_component);
}

} // namespace wizz
