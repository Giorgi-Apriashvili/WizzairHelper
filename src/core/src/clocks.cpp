#include "wizz/core/clocks.hpp"

#include <stdexcept>

namespace wizz {

namespace {

[[nodiscard]] const std::chrono::time_zone* locate_machine_zone() noexcept
{
    try {
        return std::chrono::current_zone();
    } catch (const std::runtime_error&) {
        // No time-zone database (ICU unavailable): treat the machine as UTC.
        return nullptr;
    }
}

} // namespace

SystemClock::SystemClock() noexcept : zone_(locate_machine_zone()) {}

UtcInstant SystemClock::utc_now() const
{
    return std::chrono::time_point_cast<Ticks>(std::chrono::system_clock::now());
}

LocalDateTime SystemClock::local_now() const
{
    return to_local(utc_now());
}

LocalDateTime SystemClock::to_local(UtcInstant instant) const
{
    if (zone_ == nullptr) {
        return LocalDateTime{instant.time_since_epoch()};
    }
    return zone_->to_local(instant);
}

Ticks SystemClock::monotonic_now() const
{
    return std::chrono::duration_cast<Ticks>(std::chrono::steady_clock::now().time_since_epoch());
}

FixedClock::FixedClock(UtcInstant utc_now, Duration local_offset) noexcept
    : utc_ticks_(utc_now.time_since_epoch().count()), local_offset_ticks_(local_offset.count())
{
}

UtcInstant FixedClock::utc_now() const
{
    return UtcInstant{Ticks{utc_ticks_.load()}};
}

LocalDateTime FixedClock::local_now() const
{
    return to_local(utc_now());
}

LocalDateTime FixedClock::to_local(UtcInstant instant) const
{
    return LocalDateTime{instant.time_since_epoch() + local_offset()};
}

Ticks FixedClock::monotonic_now() const
{
    return Ticks{monotonic_ticks_.load()};
}

Duration FixedClock::local_offset() const noexcept
{
    return Duration{local_offset_ticks_.load()};
}

void FixedClock::set_utc_now(UtcInstant utc_now) noexcept
{
    utc_ticks_.store(utc_now.time_since_epoch().count());
}

void FixedClock::set_local_offset(Duration offset) noexcept
{
    local_offset_ticks_.store(offset.count());
}

void FixedClock::advance(Duration delta) noexcept
{
    utc_ticks_.fetch_add(delta.count());
    monotonic_ticks_.fetch_add(delta.count());
}

} // namespace wizz
