#pragma once
// The global Wizz Air request gate (04 §5) and the cancellable mutex behind it
// and behind the client's discovery and map locks (04 §7.1, §8, §12).
//
// One gate per WizzApiClient paces every request to wizzair.com — discovery
// pages, asset/map, timetableV2 and all retries — by *start* time: at least
// `spacing` apart, plus a deterministic 0..0.9·jitter pseudo-jitter, and a
// server-overload cool-down can push the next start further out. FX and
// geocoding traffic does not pass through it.

#include "wizz/core/cancellation.hpp"
#include "wizz/core/time.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace wizz {

/// A mutex whose waits honour a std::stop_token, the C++ stand-in for the
/// async `SemaphoreSlim(1,1)` locks of 04 §5 and §12. Waiters are served in
/// arrival order. A cancelled waiter leaves the queue and throws
/// OperationCancelled without ever owning the mutex. Not recursive.
class CancellableMutex {
public:
    CancellableMutex() = default;
    CancellableMutex(const CancellableMutex&) = delete;
    CancellableMutex& operator=(const CancellableMutex&) = delete;

    /// Blocks until the mutex is owned. Throws OperationCancelled — without taking
    /// the mutex — when `stop` is already requested on entry or fires while waiting.
    void lock(std::stop_token stop);

    /// Releases the mutex; the caller must own it.
    void unlock() noexcept;

    /// Number of callers currently queued in lock() (diagnostics and tests).
    [[nodiscard]] std::size_t waiter_count() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<std::uint64_t> waiters_;  ///< FIFO of waiting tickets
    std::uint64_t next_ticket_ = 0;
    bool locked_ = false;
};

/// RAII owner of a CancellableMutex: locks in the constructor (cancellable),
/// unlocks in the destructor.
class [[nodiscard]] CancellableLock {
public:
    CancellableLock(CancellableMutex& mutex, std::stop_token stop) : mutex_(mutex) { mutex_.lock(std::move(stop)); }
    ~CancellableLock() { mutex_.unlock(); }
    CancellableLock(const CancellableLock&) = delete;
    CancellableLock& operator=(const CancellableLock&) = delete;

private:
    CancellableMutex& mutex_;
};

/// `RequestGate` (04 §5). Thread-safe; every timed wait goes through the injected
/// DelayFunc and every "now" through the injected Clock, so tests record exact
/// delays against a fixed clock (P-09, P-09b, CT-API-05).
class RequestGate {
public:
    /// `spacing` is the minimum gap between request starts (default 1 s); `jitter`
    /// (default 400 ms) scales the pseudo-jitter cycle; a non-positive jitter
    /// disables it. Throws std::invalid_argument for a null clock or an empty delay.
    RequestGate(Duration spacing, Duration jitter, std::shared_ptr<const Clock> clock, DelayFunc delay);

    RequestGate(const RequestGate&) = delete;
    RequestGate& operator=(const RequestGate&) = delete;

    /// `WaitTurn(ct)`: waits until this caller may start a request, then books the
    /// next slot at `now + spacing + j`, with `now` read after the wait.
    /// - Callers queue on a cancellable lock (FIFO); the very first turn never waits.
    /// - While holding it, the gate waits until the booked slot and re-checks, so a
    ///   cool-down that moved the slot meanwhile is honoured as well.
    /// - j = (jitter_ticks · k) / 10 with k = (counter · 7) mod 10 in integer
    ///   arithmetic (1 tick = 100 ns); the counter advances only on granted turns
    ///   with a positive jitter: 0, 280, 160, 40, 320, 200, 80, 360, 240, 120 ms, …
    /// - A cancellation while queueing or waiting throws OperationCancelled and
    ///   consumes nothing (neither the slot nor a jitter step).
    /// - Improvement over 04 §5 (allowed there): a cool-down that lands between
    ///   the end of the wait and the booking is kept (the later of the two wins);
    ///   without such a race the booking is written unconditionally, as in .NET.
    void wait_turn(std::stop_token stop);

    /// `CoolDown(d)`: the next turn may not start before `now + duration`. Only ever
    /// moves the slot later; does not take the turn lock, so it may be called
    /// at any time and from any thread.
    void cool_down(Duration duration);

private:
    [[nodiscard]] std::optional<UtcInstant> next_allowed() const;

    const Duration spacing_;
    const Duration jitter_;
    const std::shared_ptr<const Clock> clock_;
    const DelayFunc delay_;

    CancellableMutex turn_lock_;             ///< 04 §5 `lock`
    std::uint64_t counter_ = 0;              ///< jitter step; guarded by turn_lock_
    mutable std::mutex state_mutex_;         ///< 04 §5 `stateLock`
    std::optional<UtcInstant> next_allowed_; ///< nullopt = minus infinity; guarded by state_mutex_
};

} // namespace wizz
