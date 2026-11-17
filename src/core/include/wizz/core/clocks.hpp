#pragma once
// Concrete clocks behind the injectable `wizz::Clock` (06 §2, 05 §1.1):
// SystemClock for production and FixedClock, the deterministic test clock that
// never advances unless a test moves it.

#include "wizz/core/time.hpp"

#include <atomic>
#include <chrono>

namespace wizz {

/// The real clock: UTC from std::chrono::system_clock, the local wall clock in
/// the machine's time zone (DST-aware, like TimeProvider.System.GetLocalNow()),
/// and std::chrono::steady_clock for elapsed time. Stateless after construction
/// and safe to share between threads.
class SystemClock final : public Clock {
public:
    /// Resolves the machine time zone once. If the time-zone database is
    /// unavailable the local wall clock falls back to UTC.
    SystemClock() noexcept;

    [[nodiscard]] UtcInstant utc_now() const override;
    [[nodiscard]] LocalDateTime local_now() const override;
    [[nodiscard]] Ticks monotonic_now() const override;
    /// The machine zone's wall clock at `instant`, with the DST rule of that instant.
    [[nodiscard]] LocalDateTime to_local(UtcInstant instant) const override;

private:
    const std::chrono::time_zone* zone_ = nullptr;  ///< nullptr = UTC fallback
};

/// Deterministic test clock (05 §1.1 `FixedTime`). Defaults to
/// 2026-11-02T10:00:00Z with the local zone UTC, so "today" is 2026-11-02.
/// Nothing moves unless a test calls a setter; all setters are thread-safe.
/// The monotonic counter starts at zero and only moves with `advance`.
class FixedClock final : public Clock {
public:
    static constexpr UtcInstant kDefaultUtcNow = make_utc(Date::from_ymd(2026, 11, 2), 10, 0);

    explicit FixedClock(UtcInstant utc_now = kDefaultUtcNow, Duration local_offset = Duration::zero()) noexcept;

    [[nodiscard]] UtcInstant utc_now() const override;
    /// `utc_now() + local_offset()` as a naive wall-clock value.
    [[nodiscard]] LocalDateTime local_now() const override;
    [[nodiscard]] Ticks monotonic_now() const override;
    /// `instant + local_offset()`: the fixed zone has no daylight saving time.
    [[nodiscard]] LocalDateTime to_local(UtcInstant instant) const override;

    [[nodiscard]] Duration local_offset() const noexcept;

    /// Jumps the UTC (and therefore local) time; the monotonic counter is unaffected.
    void set_utc_now(UtcInstant utc_now) noexcept;
    /// Changes the local zone's offset from UTC (e.g. +4 h) to control "today".
    void set_local_offset(Duration offset) noexcept;
    /// Moves UTC, local and monotonic time forward together by `delta`.
    void advance(Duration delta) noexcept;

private:
    std::atomic<Ticks::rep> utc_ticks_;
    std::atomic<Ticks::rep> local_offset_ticks_;
    std::atomic<Ticks::rep> monotonic_ticks_{0};
};

} // namespace wizz
