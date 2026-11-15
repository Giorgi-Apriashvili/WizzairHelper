#pragma once
// Three views of time (06 §3): civil calendar dates for inclusive ranges and
// nights, naive airport-local wall-clock date-times for timetable values, UTC
// instants for cache timestamps / retry deadlines, plus a monotonic clock for
// elapsed time. All of them are injectable through `Clock`.

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wizz {

/// .NET-compatible 100 ns tick. Every wizz duration and time point uses it.
using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
using Duration = Ticks;

/// A UTC instant (cache ages, gate deadlines, WaitUntilUtc).
using UtcInstant = std::chrono::sys_time<Ticks>;

/// A naive, zone-less wall-clock date-time. Never converted between zones.
using LocalDateTime = std::chrono::local_time<Ticks>;

/// A civil calendar date (DateOnly). Default-constructed value is 0001-01-01,
/// the .NET DateOnly.MinValue.
class Date {
public:
    constexpr Date() noexcept = default;
    constexpr explicit Date(std::chrono::sys_days days) noexcept : days_(days) {}
    constexpr Date(std::chrono::year_month_day ymd) noexcept : days_(ymd) {}

    [[nodiscard]] static constexpr Date from_ymd(int year, unsigned month, unsigned day) noexcept
    {
        return Date{std::chrono::year_month_day{std::chrono::year{year}, std::chrono::month{month},
                                                std::chrono::day{day}}};
    }
    [[nodiscard]] static constexpr Date min_value() noexcept { return Date{}; }

    [[nodiscard]] constexpr std::chrono::sys_days sys_days() const noexcept { return days_; }
    [[nodiscard]] constexpr std::chrono::year_month_day ymd() const noexcept
    {
        return std::chrono::year_month_day{days_};
    }
    [[nodiscard]] constexpr int year() const noexcept { return static_cast<int>(ymd().year()); }
    [[nodiscard]] constexpr unsigned month() const noexcept { return static_cast<unsigned>(ymd().month()); }
    [[nodiscard]] constexpr unsigned day() const noexcept { return static_cast<unsigned>(ymd().day()); }
    [[nodiscard]] constexpr std::chrono::weekday weekday() const noexcept { return std::chrono::weekday{days_}; }

    /// Linear day count (only differences are meaningful, like DateOnly.DayNumber).
    [[nodiscard]] constexpr std::int64_t day_number() const noexcept
    {
        return static_cast<std::int64_t>(days_.time_since_epoch().count());
    }
    [[nodiscard]] constexpr Date add_days(std::int64_t days) const noexcept
    {
        return Date{days_ + std::chrono::days{days}};
    }
    /// Signed whole days `a - b`.
    [[nodiscard]] friend constexpr std::int64_t days_between(Date a, Date b) noexcept
    {
        return a.day_number() - b.day_number();
    }

    friend constexpr bool operator==(Date, Date) noexcept = default;
    friend constexpr auto operator<=>(Date, Date) noexcept = default;

    /// Invariant `yyyy-MM-dd`.
    [[nodiscard]] std::string iso() const;
    /// Strict invariant `yyyy-MM-dd` (exactly 10 chars). Returns nullopt on anything else.
    [[nodiscard]] static std::optional<Date> parse_iso(std::string_view text);

private:
    std::chrono::sys_days days_{std::chrono::year_month_day{std::chrono::year{1}, std::chrono::January,
                                                             std::chrono::day{1}}};
};

[[nodiscard]] constexpr Date date_of(LocalDateTime t) noexcept
{
    return Date{std::chrono::sys_days{std::chrono::floor<std::chrono::days>(t).time_since_epoch()}};
}
[[nodiscard]] constexpr LocalDateTime at_midnight(Date d) noexcept
{
    return LocalDateTime{std::chrono::duration_cast<Ticks>(d.sys_days().time_since_epoch())};
}
[[nodiscard]] constexpr Ticks time_of_day(LocalDateTime t) noexcept
{
    return t - std::chrono::floor<std::chrono::days>(t);
}
[[nodiscard]] constexpr LocalDateTime make_local(Date d, int hour, int minute, int second = 0) noexcept
{
    return at_midnight(d) + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
}

/// Injectable clock: UTC instant, machine-local wall clock and a monotonic counter.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual UtcInstant utc_now() const = 0;
    /// Local wall-clock "now" in the clock's time zone (tests: UTC).
    [[nodiscard]] virtual LocalDateTime local_now() const = 0;
    /// Monotonic time since an arbitrary epoch, for elapsed durations.
    [[nodiscard]] virtual Ticks monotonic_now() const = 0;

    /// "today" = DateOnly.FromDateTime(GetLocalNow().DateTime).
    [[nodiscard]] Date local_today() const { return date_of(local_now()); }
};

} // namespace wizz
