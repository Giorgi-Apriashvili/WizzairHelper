#include "wizz/core/datetime_parse.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>

namespace wizz {

namespace {

constexpr std::string_view kAsciiWhitespace = " \t\n\v\f\r";
constexpr int kMaxFractionDigits = 7;  // .fffffff = one 100 ns tick
constexpr int kMaxOffsetHours = 14;    // the widest real UTC offset

[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept
{
    const auto first = text.find_first_not_of(kAsciiWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kAsciiWhitespace);
    return text.substr(first, last - first + 1);
}

[[nodiscard]] constexpr bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// Forward-only reader over the trimmed input.
class Cursor {
public:
    explicit Cursor(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] bool at_end() const noexcept { return pos_ == text_.size(); }

    [[nodiscard]] bool accept(char c) noexcept
    {
        if (!at_end() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    /// Exactly `count` ASCII digits as a number.
    [[nodiscard]] std::optional<int> digits(int count) noexcept
    {
        int value = 0;
        for (int i = 0; i < count; ++i) {
            if (at_end() || !is_digit(text_[pos_])) {
                return std::nullopt;
            }
            value = value * 10 + (text_[pos_++] - '0');
        }
        return value;
    }

    /// One to seven fraction digits, scaled to 100 ns ticks.
    [[nodiscard]] std::optional<std::int64_t> fraction_ticks() noexcept
    {
        std::int64_t ticks = 0;
        int count = 0;
        while (!at_end() && is_digit(text_[pos_])) {
            if (++count > kMaxFractionDigits) {
                return std::nullopt;
            }
            ticks = ticks * 10 + (text_[pos_++] - '0');
        }
        if (count == 0) {
            return std::nullopt;
        }
        for (; count < kMaxFractionDigits; ++count) {
            ticks *= 10;
        }
        return ticks;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
};

[[nodiscard]] std::optional<Date> make_date(std::optional<int> year, std::optional<int> month,
                                            std::optional<int> day) noexcept
{
    if (!year || !month || !day || *year < 1) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)},
                                          std::chrono::day{static_cast<unsigned>(*day)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return Date{ymd};
}

/// Invariant-culture short date `MM/dd/yyyy` (02 §2.4: "11/02/2026" is 2 Nov 2026).
[[nodiscard]] std::optional<LocalDateTime> parse_us_date(std::string_view text) noexcept
{
    Cursor cursor{text};
    const auto month = cursor.digits(2);
    if (!cursor.accept('/')) {
        return std::nullopt;
    }
    const auto day = cursor.digits(2);
    if (!cursor.accept('/')) {
        return std::nullopt;
    }
    const auto year = cursor.digits(4);
    const auto date = make_date(year, month, day);
    if (!date || !cursor.at_end()) {
        return std::nullopt;
    }
    return at_midnight(*date);
}

/// Skips `Z` or `±HH[[:]mm]` (D-22: offsets are ignored, the wall clock is kept).
[[nodiscard]] bool skip_utc_designator(Cursor& cursor) noexcept
{
    if (cursor.accept('Z') || cursor.accept('z')) {
        return true;
    }
    if (!cursor.accept('+') && !cursor.accept('-')) {
        return false;
    }
    const auto hours = cursor.digits(2);
    if (!hours || *hours > kMaxOffsetHours) {
        return false;
    }
    if (cursor.at_end()) {
        return true;
    }
    static_cast<void>(cursor.accept(':'));  // both `+02:00` and `+0200` are accepted
    const auto minutes = cursor.digits(2);
    return minutes && *minutes <= 59;
}

} // namespace

std::optional<LocalDateTime> parse_local_datetime(std::string_view text)
{
    const std::string_view trimmed = trim_ascii(text);
    if (trimmed.size() > 2 && trimmed[2] == '/') {
        return parse_us_date(trimmed);
    }

    Cursor cursor{trimmed};
    const auto year = cursor.digits(4);
    if (!cursor.accept('-')) {
        return std::nullopt;
    }
    const auto month = cursor.digits(2);
    if (!cursor.accept('-')) {
        return std::nullopt;
    }
    const auto day = cursor.digits(2);
    const auto date = make_date(year, month, day);
    if (!date) {
        return std::nullopt;
    }
    if (cursor.at_end()) {
        return at_midnight(*date);
    }

    if (!cursor.accept('T') && !cursor.accept(' ')) {
        return std::nullopt;
    }
    const auto hour = cursor.digits(2);
    if (!cursor.accept(':')) {
        return std::nullopt;
    }
    const auto minute = cursor.digits(2);
    std::optional<int> second = 0;
    std::optional<std::int64_t> fraction = 0;
    if (cursor.accept(':')) {
        second = cursor.digits(2);
        if (cursor.accept('.')) {
            fraction = cursor.fraction_ticks();
        }
    }
    if (!hour || !minute || !second || !fraction || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    if (!cursor.at_end() && !skip_utc_designator(cursor)) {
        return std::nullopt;
    }
    if (!cursor.at_end()) {
        return std::nullopt;
    }
    return make_local(*date, *hour, *minute, *second) + Ticks{*fraction};
}

std::string format_hhmm(LocalDateTime time)
{
    const std::chrono::hh_mm_ss hms{std::chrono::floor<std::chrono::seconds>(time_of_day(time))};
    return std::format("{:02}:{:02}", hms.hours().count(), hms.minutes().count());
}

std::string format_iso_seconds(LocalDateTime time)
{
    const std::chrono::hh_mm_ss hms{std::chrono::floor<std::chrono::seconds>(time_of_day(time))};
    return std::format("{}T{:02}:{:02}:{:02}", date_of(time).iso(), hms.hours().count(), hms.minutes().count(),
                       hms.seconds().count());
}

std::string format_iso_utc(UtcInstant instant)
{
    std::string text = format_iso_local(LocalDateTime{instant.time_since_epoch()});
    text += 'Z';
    return text;
}

std::string format_iso_local(LocalDateTime time)
{
    std::string text = format_iso_seconds(time);
    const Ticks sub_second = time - std::chrono::floor<std::chrono::seconds>(time);
    if (sub_second != Ticks::zero()) {
        text += std::format(".{:07}", sub_second.count());
    }
    return text;
}

std::optional<UtcInstant> parse_iso_utc(std::string_view text)
{
    Cursor cursor{text};
    const auto year = cursor.digits(4);
    if (!cursor.accept('-')) {
        return std::nullopt;
    }
    const auto month = cursor.digits(2);
    if (!cursor.accept('-')) {
        return std::nullopt;
    }
    const auto day = cursor.digits(2);
    const auto date = make_date(year, month, day);
    if (!date || !cursor.accept('T')) {
        return std::nullopt;
    }
    const auto hour = cursor.digits(2);
    if (!cursor.accept(':')) {
        return std::nullopt;
    }
    const auto minute = cursor.digits(2);
    if (!cursor.accept(':')) {
        return std::nullopt;
    }
    const auto second = cursor.digits(2);
    std::optional<std::int64_t> fraction = 0;
    if (cursor.accept('.')) {
        fraction = cursor.fraction_ticks();
    }
    if (!hour || !minute || !second || !fraction || *hour > 23 || *minute > 59 || *second > 59 ||
        !cursor.accept('Z') || !cursor.at_end()) {
        return std::nullopt;
    }
    return make_utc(*date, *hour, *minute, *second) + Ticks{*fraction};
}

} // namespace wizz
