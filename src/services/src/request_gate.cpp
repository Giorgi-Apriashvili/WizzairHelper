// RequestGate and CancellableMutex (04 §5, §12).

#include "wizz/services/request_gate.hpp"

#include <algorithm>
#include <stdexcept>

namespace wizz {

// ---------------------------------------------------------------------------
// CancellableMutex
// ---------------------------------------------------------------------------

void CancellableMutex::lock(std::stop_token stop)
{
    // SemaphoreSlim.WaitAsync(ct) with an already cancelled token fails even
    // when the semaphore is free.
    throw_if_cancelled(stop);

    std::unique_lock guard{mutex_};
    if (!locked_ && waiters_.empty()) {
        locked_ = true;
        return;
    }
    const std::uint64_t ticket = next_ticket_++;
    waiters_.push_back(ticket);
    const bool acquired =
        changed_.wait(guard, stop, [&] { return !locked_ && waiters_.front() == ticket; });
    if (!acquired) {
        // Leave the queue; the waiter behind us may now be first in line.
        std::erase(waiters_, ticket);
        guard.unlock();
        changed_.notify_all();
        throw OperationCancelled{};
    }
    waiters_.pop_front();
    locked_ = true;
}

void CancellableMutex::unlock() noexcept
{
    {
        const std::scoped_lock guard{mutex_};
        locked_ = false;
    }
    changed_.notify_all();
}

std::size_t CancellableMutex::waiter_count() const
{
    const std::scoped_lock guard{mutex_};
    return waiters_.size();
}

// ---------------------------------------------------------------------------
// RequestGate
// ---------------------------------------------------------------------------

RequestGate::RequestGate(Duration spacing, Duration jitter, std::shared_ptr<const Clock> clock, DelayFunc delay)
    : spacing_(spacing), jitter_(jitter), clock_(std::move(clock)), delay_(std::move(delay))
{
    if (!clock_) {
        throw std::invalid_argument("RequestGate needs a clock.");
    }
    if (!delay_) {
        throw std::invalid_argument("RequestGate needs a delay function.");
    }
}

std::optional<UtcInstant> RequestGate::next_allowed() const
{
    const std::scoped_lock guard{state_mutex_};
    return next_allowed_;
}

void RequestGate::wait_turn(std::stop_token stop)
{
    const CancellableLock turn{turn_lock_, stop};

    // while read(nextAllowed) > target: target = nextAllowed; wait until target.
    // Both start at minus infinity (nullopt), and a value is never compared with
    // or subtracted from minus infinity, so there is no sentinel arithmetic.
    std::optional<UtcInstant> target;
    for (;;) {
        const std::optional<UtcInstant> next = next_allowed();
        if (!next || (target && *next <= *target)) {
            break;
        }
        target = next;
        const Duration wait = *target - clock_->utc_now();
        if (wait > Duration::zero()) {
            delay_(wait, stop);  // cancellable; on cancel nothing has been booked
        }
    }

    Duration jitter = Duration::zero();
    if (jitter_ > Duration::zero()) {
        const auto k = static_cast<Duration::rep>((counter_ * 7) % 10);
        ++counter_;
        jitter = Duration{(jitter_.count() * k) / 10};  // int64 integer division, as in .NET
    }
    const UtcInstant booked = clock_->utc_now() + spacing_ + jitter;  // "now" after the wait = this start

    const std::scoped_lock guard{state_mutex_};
    // `target` is the last value seen by the re-check loop. If nextAllowed still
    // holds it, write unconditionally (04 §5); if a cool-down moved it since, keep
    // the later instant so the cool-down cannot be lost (the allowed improvement).
    if (next_allowed_ == target) {
        next_allowed_ = booked;
    } else {
        next_allowed_ = std::max(*next_allowed_, booked);
    }
}

void RequestGate::cool_down(Duration duration)
{
    const UtcInstant until = clock_->utc_now() + duration;
    const std::scoped_lock guard{state_mutex_};
    if (!next_allowed_ || until > *next_allowed_) {
        next_allowed_ = until;
    }
}

} // namespace wizz
