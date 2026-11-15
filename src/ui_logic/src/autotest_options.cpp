#include "wizz/ui/autotest_options.hpp"

#include "wizz/ui/settings.hpp"
#include "wizz/ui/strings.hpp"

#include "wizz/core/errors.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <vector>

namespace wizz::ui {

namespace {

/// Array.IndexOf(args, flag): the first exact, case-sensitive match.
[[nodiscard]] std::optional<std::size_t> index_of(std::span<const std::string> args, std::string_view flag) noexcept
{
    const auto found = std::ranges::find(args, flag);
    if (found == args.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - args.begin());
}

/// The argument after the first occurrence of `flag`, or nullopt.
[[nodiscard]] std::optional<std::string> value_of(std::span<const std::string> args, std::string_view flag)
{
    const std::optional<std::size_t> index = index_of(args, flag);
    if (!index || *index + 1 >= args.size()) {
        return std::nullopt;
    }
    return args[*index + 1];
}

[[nodiscard]] bool has_flag(std::span<const std::string> args, std::string_view flag) noexcept
{
    return index_of(args, flag).has_value();
}

/// A UTF-8 command-line value as a path (never through the ANSI code page).
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view utf8)
{
    return std::filesystem::path{std::u8string_view{reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()}};
}

/// Path.GetFullPath(value) against `current_dir`.
[[nodiscard]] std::filesystem::path full_path(std::string_view value, const std::filesystem::path& current_dir)
{
    if (value.empty()) {
        throw std::invalid_argument("The value cannot be an empty string. (Parameter 'path')");
    }
    const std::filesystem::path path = path_from_utf8(value);
    return (path.is_absolute() ? path : current_dir / path).lexically_normal();
}

[[nodiscard]] Date parse_exact_date(std::string_view text)
{
    if (const std::optional<Date> date = Date::parse_iso(text)) {
        return *date;
    }
    throw FormatError(std::format("String '{}' was not recognized as a valid DateOnly.", text));
}

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr bool is_number_white(char c) noexcept
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/// NumberFormatInfo.AllowHyphenDuringParsing: the negative sign is a minus-like character.
[[nodiscard]] bool allows_hyphen(std::string_view negative_sign) noexcept
{
    static constexpr std::array<std::string_view, 7> kMinusLike{
        "‒", "⁻", "₋", "−", "➖", "﹣", "－"};
    return std::ranges::find(kMinusLike, negative_sign) != kMinusLike.end();
}

[[nodiscard]] std::optional<int> clamped_radius(const std::optional<std::string>& value, const NumberSymbols& symbols)
{
    if (!value) {
        return std::nullopt;
    }
    const std::optional<int> parsed = parse_culture_int(*value, symbols);
    if (!parsed) {
        return std::nullopt;  // unparsable: ignored, the settings value stays
    }
    return std::clamp(*parsed, 0, kMaxRadiusKm);
}

} // namespace

std::optional<int> parse_culture_int(std::string_view text, const NumberSymbols& symbols) noexcept
{
    std::size_t i = 0;
    while (i < text.size() && is_number_white(text[i])) {
        ++i;
    }
    bool negative = false;
    const std::string_view rest = text.substr(i);
    const std::string_view negative_sign = symbols.negative_sign;
    if (rest.starts_with('+')) {
        ++i;
    } else if (!negative_sign.empty() && rest.starts_with(negative_sign)) {
        negative = true;
        i += negative_sign.size();
    } else if (rest.starts_with('-') && (negative_sign == "-" || allows_hyphen(negative_sign))) {
        negative = true;
        ++i;
    }
    const std::size_t digits_start = i;
    std::int64_t magnitude = 0;
    while (i < text.size() && is_ascii_digit(text[i])) {
        magnitude = magnitude * 10 + (text[i] - '0');
        if (magnitude > static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1) {
            return std::nullopt;  // overflow
        }
        ++i;
    }
    if (i == digits_start) {
        return std::nullopt;
    }
    while (i < text.size() && is_number_white(text[i])) {
        ++i;
    }
    while (i < text.size() && text[i] == '\0') {
        ++i;  // trailing NULs are tolerated
    }
    if (i != text.size()) {
        return std::nullopt;
    }
    const std::int64_t value = negative ? -magnitude : magnitude;
    if (value > std::numeric_limits<int>::max() || value < std::numeric_limits<int>::min()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

AutoTestDates parse_autotest_dates(std::string_view value)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t colon = value.find(':', start);
        parts.push_back(value.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start));
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    const Date first = parse_exact_date(parts.front());
    const Date last = parse_exact_date(parts.back());
    return AutoTestDates{first, last};
}

UtcInstant parse_fixed_now(std::string_view value)
{
    const auto fail = [&]() -> FormatError {
        return FormatError(std::format("String '{}' was not recognized as a valid UTC date and time.", value));
    };
    std::string_view text = value;
    if (text.ends_with('Z')) {
        text.remove_suffix(1);
    }
    if (text.size() < 19 || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        throw fail();
    }
    const std::optional<Date> date = Date::parse_iso(text.substr(0, 10));
    const auto two_digits = [&](std::size_t at) -> int {
        if (!is_ascii_digit(text[at]) || !is_ascii_digit(text[at + 1])) {
            throw fail();
        }
        return (text[at] - '0') * 10 + (text[at + 1] - '0');
    };
    if (!date) {
        throw fail();
    }
    const int hour = two_digits(11);
    const int minute = two_digits(14);
    const int second = two_digits(17);
    if (hour > 23 || minute > 59 || second > 59) {
        throw fail();
    }
    std::int64_t fraction_ticks = 0;
    if (text.size() > 19) {
        const std::string_view fraction = text.substr(20);
        if (text[19] != '.' || fraction.empty() || fraction.size() > 7 || !std::ranges::all_of(fraction, is_ascii_digit)) {
            throw fail();
        }
        for (std::size_t k = 0; k < 7; ++k) {
            fraction_ticks = fraction_ticks * 10 + (k < fraction.size() ? fraction[k] - '0' : 0);
        }
    }
    return make_utc(*date, hour, minute, second) + Ticks{fraction_ticks};
}

std::chrono::milliseconds parse_cancel_after(std::string_view value)
{
    constexpr std::size_t kMaxDigits = 9;  // under 12 days; int64 milliseconds never overflow
    if (value.empty() || value.size() > kMaxDigits || !std::ranges::all_of(value, is_ascii_digit)) {
        throw FormatError(std::format("String '{}' was not recognized as a number of milliseconds.", value));
    }
    std::int64_t milliseconds = 0;
    for (const char c : value) {
        milliseconds = milliseconds * 10 + (c - '0');
    }
    return std::chrono::milliseconds{milliseconds};
}

std::optional<AutoTestOptions> AutoTestOptions::parse(std::span<const std::string> args,
                                                      const std::filesystem::path& current_dir,
                                                      const NumberSymbols& number_symbols)
{
    const std::optional<std::string> output = value_of(args, "--autotest");
    if (!output) {
        return std::nullopt;
    }
    AutoTestOptions options;
    options.output_dir = full_path(*output, current_dir);
    if (std::optional<std::string> from = value_of(args, "--from")) {
        options.from = *std::move(from);
    }
    if (std::optional<std::string> to = value_of(args, "--to")) {
        options.to = *std::move(to);
    }
    options.from_radius_km = clamped_radius(value_of(args, "--from-radius"), number_symbols);
    options.to_radius_km = clamped_radius(value_of(args, "--to-radius"), number_symbols);
    if (const std::optional<std::string> out = value_of(args, "--out")) {
        options.outbound = parse_autotest_dates(*out);
    }
    if (const std::optional<std::string> ret = value_of(args, "--ret")) {
        options.inbound = parse_autotest_dates(*ret);
    }
    options.currency = value_of(args, "--currency");
    options.then_currency = value_of(args, "--then-currency");
    options.return_from_top = has_flag(args, "--return-from-top");
    options.keep_open = has_flag(args, "--keep-open");
    if (const std::optional<std::string> root = value_of(args, "--data-root")) {
        options.data_root = full_path(*root, current_dir);
    }
    if (const std::optional<std::string> replay = value_of(args, "--replay")) {
        options.replay_dir = full_path(*replay, current_dir);
    }
    if (const std::optional<std::string> now = value_of(args, "--fixed-now")) {
        options.fixed_now = parse_fixed_now(*now);
    }
    options.no_pacing = has_flag(args, "--no-pacing");
    options.warm_cache = has_flag(args, "--warm-cache");
    if (const std::optional<std::string> after = value_of(args, "--cancel-after-ms")) {
        options.cancel_after = parse_cancel_after(*after);
    }
    options.diagnostics = has_flag(args, "--diagnostics");
    return options;
}

std::optional<std::string> AutoTestOptions::listed_currency() const
{
    if (currency && strings::is_listed_currency(*currency)) {
        return currency;
    }
    return std::nullopt;
}

std::optional<std::string> AutoTestOptions::listed_then_currency() const
{
    if (then_currency && strings::is_listed_currency(*then_currency)) {
        return then_currency;
    }
    return std::nullopt;
}

} // namespace wizz::ui
