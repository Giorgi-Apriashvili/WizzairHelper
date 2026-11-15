// RequestGate (04 §5) and CancellableMutex (04 §5, §12): 05 §7.1 P-09 and P-09b,
// the verified properties of 04 §5 (first call never waits, idle gaps, cool-down,
// cancellation does not consume a slot, the re-check loop, the race improvement)
// and the 04 §14.1 timeline. Every wait goes through the recording delay, so the
// expected delays are exact.

#include "wizz/services/request_gate.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/time.hpp"

#include "support/recording_delay.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace {

using wizz::CancellableLock;
using wizz::CancellableMutex;
using wizz::Duration;
using wizz::FixedClock;
using wizz::OperationCancelled;
using wizz::RequestGate;
using wizz::test::RecordingDelay;

using namespace std::chrono_literals;

using Delays = std::vector<Duration>;

/// A FixedClock that runs a hook the first time `utc_now()` is read.
class FirstReadHookClock final : public wizz::Clock {
public:
    explicit FirstReadHookClock(std::function<void()> hook) : hook_(std::move(hook)) {}

    [[nodiscard]] wizz::UtcInstant utc_now() const override
    {
        if (!fired_.exchange(true) && hook_) {
            hook_();
        }
        return base_.utc_now();
    }
    [[nodiscard]] wizz::LocalDateTime local_now() const override { return base_.local_now(); }
    [[nodiscard]] wizz::Ticks monotonic_now() const override { return base_.monotonic_now(); }

private:
    FixedClock base_;
    std::function<void()> hook_;
    mutable std::atomic<bool> fired_{false};
};

/// Spins (yielding) until `condition` holds; fails the test after 10 s.
void wait_until(const std::function<bool()>& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!condition()) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "condition never became true";
        std::this_thread::sleep_for(1ms);
    }
}

// ---------------------------------------------------------------------------
// 05 §7.1 P-09, P-09b
// ---------------------------------------------------------------------------

TEST(RequestGate, P_09_Request_gate_spacing_and_jitter)
{
    const RecordingDelay delay;
    RequestGate gate{1s, 400ms, std::make_shared<FixedClock>(), delay.delay_func()};
    for (int turn = 0; turn < 12; ++turn) {
        gate.wait_turn({});
    }
    // The first call never waits; then spacing + 400ms·((n·7) mod 10)/10 for n = 0, 1, 2, …
    const Delays expected{1000ms, 1280ms, 1160ms, 1040ms, 1320ms, 1200ms, 1080ms, 1360ms, 1240ms, 1120ms, 1000ms};
    EXPECT_EQ(delay.delays(), expected);
}

TEST(RequestGate, P_09b_Gate_cool_down)
{
    const RecordingDelay delay;
    RequestGate gate{1s, Duration::zero(), std::make_shared<FixedClock>(), delay.delay_func()};
    gate.wait_turn({});
    gate.cool_down(6s);
    gate.wait_turn({});
    gate.wait_turn({});
    // A cool-down only moves the next slot later; the turn after it is spaced normally.
    EXPECT_EQ(delay.delays(), (Delays{6s, 1s}));
}

// ---------------------------------------------------------------------------
// 04 §5 properties
// ---------------------------------------------------------------------------

TEST(RequestGate, First_turn_never_waits_and_an_idle_gap_longer_than_the_spacing_skips_the_wait)
{
    const auto clock = std::make_shared<FixedClock>();
    const RecordingDelay delay;
    RequestGate gate{1s, Duration::zero(), clock, delay.delay_func()};
    gate.wait_turn({});
    clock->advance(1500ms);  // idle longer than the spacing
    gate.wait_turn({});
    clock->advance(1s);  // exactly the spacing: the slot is due now
    gate.wait_turn({});
    EXPECT_TRUE(delay.delays().empty());

    clock->advance(400ms);  // 600 ms of the spacing left
    gate.wait_turn({});
    EXPECT_EQ(delay.delays(), (Delays{600ms}));
}

TEST(RequestGate, Zero_spacing_and_jitter_never_wait)
{
    const RecordingDelay delay;
    RequestGate gate{Duration::zero(), Duration::zero(), std::make_shared<FixedClock>(), delay.delay_func()};
    for (int turn = 0; turn < 5; ++turn) {
        gate.wait_turn({});
    }
    EXPECT_TRUE(delay.delays().empty());
}

TEST(RequestGate, Timeline_of_04_14_1_with_a_clock_that_moves_during_each_wait)
{
    // Instant responses, default options: turns start at t = 0, 1.000 and 2.280; the
    // bookings are 1.000, 2.280 (1 s + 280 ms) and 3.440 (1 s + 160 ms).
    const auto clock = std::make_shared<FixedClock>();
    const auto start = clock->utc_now();
    const RecordingDelay delay{[&clock](Duration d) { clock->advance(d); }};
    RequestGate gate{1s, 400ms, clock, delay.delay_func()};

    gate.wait_turn({});
    EXPECT_EQ(clock->utc_now() - start, Duration::zero());
    gate.wait_turn({});
    EXPECT_EQ(clock->utc_now() - start, Duration{1000ms});
    gate.wait_turn({});
    EXPECT_EQ(clock->utc_now() - start, Duration{2280ms});
    gate.wait_turn({});
    EXPECT_EQ(clock->utc_now() - start, Duration{3440ms});
    EXPECT_EQ(delay.delays(), (Delays{1000ms, 1280ms, 1160ms}));
}

TEST(RequestGate, Cool_down_only_moves_the_slot_later)
{
    const RecordingDelay delay;
    RequestGate gate{1s, Duration::zero(), std::make_shared<FixedClock>(), delay.delay_func()};
    gate.cool_down(6s);  // before any turn: the first turn waits for it too
    gate.cool_down(2s);  // earlier than the current slot: ignored
    gate.wait_turn({});
    EXPECT_EQ(delay.delays(), (Delays{6s}));
}

TEST(RequestGate, A_cool_down_during_the_wait_is_honoured_by_the_recheck_loop)
{
    std::shared_ptr<RequestGate> gate;
    std::atomic<int> waits{0};
    const RecordingDelay delay{[&](Duration) {
        if (waits.fetch_add(1) == 0) {
            gate->cool_down(6s);  // e.g. another caller's 503 while this one sleeps
        }
    }};
    gate = std::make_shared<RequestGate>(1s, Duration::zero(), std::make_shared<FixedClock>(), delay.delay_func());
    gate->wait_turn({});
    gate->wait_turn({});
    EXPECT_EQ(delay.delays(), (Delays{1s, 6s}));
}

TEST(RequestGate, A_cool_down_racing_with_the_booking_is_not_lost)
{
    // The cool-down lands after the wait loop, before the booking is written (the
    // race of 04 §5). The .NET gate would overwrite it with now + 1 s; this gate
    // keeps the later instant, so the next turn waits the full 6 s.
    std::shared_ptr<RequestGate> gate;
    const auto clock = std::make_shared<FirstReadHookClock>([&gate] { gate->cool_down(6s); });
    const RecordingDelay delay;
    gate = std::make_shared<RequestGate>(1s, Duration::zero(), clock, delay.delay_func());
    gate->wait_turn({});  // the first turn reads the clock only for its booking
    gate->wait_turn({});
    EXPECT_EQ(delay.delays(), (Delays{6s}));
}

TEST(RequestGate, Cancellation_during_the_wait_does_not_consume_a_slot_or_a_jitter_step)
{
    std::stop_source cancel;
    std::atomic<bool> cancel_in_next_wait{false};
    const RecordingDelay delay{[&](Duration) {
        if (cancel_in_next_wait.exchange(false)) {
            cancel.request_stop();  // the user cancels while the gate sleeps
        }
    }};
    RequestGate gate{1s, 400ms, std::make_shared<FixedClock>(), delay.delay_func()};
    gate.wait_turn({});  // k = 0: booked now + 1 s

    cancel_in_next_wait = true;
    EXPECT_THROW(gate.wait_turn(cancel.get_token()), OperationCancelled);  // the recorder logs 1 s, then throws

    gate.wait_turn({});  // the slot is still now + 1 s; this turn uses k = 7
    gate.wait_turn({});  // 1 s + 280 ms
    EXPECT_EQ(delay.delays(), (Delays{1s, 1s, 1280ms}));
}

TEST(RequestGate, A_cancelled_token_fails_at_once_even_when_the_gate_is_free)
{
    std::stop_source cancel;
    cancel.request_stop();
    const RecordingDelay delay;
    RequestGate gate{1s, 400ms, std::make_shared<FixedClock>(), delay.delay_func()};
    EXPECT_THROW(gate.wait_turn(cancel.get_token()), OperationCancelled);
    gate.wait_turn({});  // still the very first turn: no wait
    EXPECT_TRUE(delay.delays().empty());
}

TEST(RequestGate, Cancellation_while_queued_for_the_turn_lock)
{
    std::latch release{1};
    std::atomic<int> waits{0};
    const RecordingDelay delay{[&](Duration) {
        if (waits.fetch_add(1) == 0) {
            release.wait();  // the holder of the turn lock sleeps until the test lets it go
        }
    }};
    RequestGate gate{1s, Duration::zero(), std::make_shared<FixedClock>(), delay.delay_func()};
    gate.wait_turn({});

    std::jthread holder{[&] { gate.wait_turn({}); }};
    wait_until([&] { return waits.load() == 1; });  // holder owns the lock and is waiting

    std::stop_source cancel;
    std::exception_ptr queued_error;
    std::jthread queued{[&] {
        try {
            gate.wait_turn(cancel.get_token());
        } catch (...) {
            queued_error = std::current_exception();
        }
    }};
    std::this_thread::sleep_for(20ms);  // let it queue (the outcome is the same if it has not yet)
    cancel.request_stop();
    queued.join();
    ASSERT_TRUE(queued_error);
    EXPECT_THROW(std::rethrow_exception(queued_error), OperationCancelled);

    release.count_down();
    holder.join();
    gate.wait_turn({});
    // The cancelled caller never waited and booked nothing.
    EXPECT_EQ(delay.delays(), (Delays{1s, 1s}));
}

TEST(RequestGate, Concurrent_callers_are_serialised_and_follow_the_jitter_cycle)
{
    const RecordingDelay delay;
    RequestGate gate{1s, 400ms, std::make_shared<FixedClock>(), delay.delay_func()};
    {
        std::vector<std::jthread> callers;
        for (int t = 0; t < 4; ++t) {
            callers.emplace_back([&gate] {
                for (int turn = 0; turn < 10; ++turn) {
                    gate.wait_turn({});
                }
            });
        }
    }
    // Turns are serialised, so the waits are the P-09 cycle in turn order whatever the interleaving.
    const Delays cycle{1000ms, 1280ms, 1160ms, 1040ms, 1320ms, 1200ms, 1080ms, 1360ms, 1240ms, 1120ms};
    const Delays delays = delay.delays();
    ASSERT_EQ(delays.size(), 39u);
    for (std::size_t i = 0; i < delays.size(); ++i) {
        EXPECT_EQ(delays[i], cycle[i % cycle.size()]) << "turn " << i + 1;
    }
}

TEST(RequestGate, Rejects_a_missing_clock_or_delay)
{
    const RecordingDelay delay;
    EXPECT_THROW((RequestGate{1s, 0s, nullptr, delay.delay_func()}), std::invalid_argument);
    EXPECT_THROW((RequestGate{1s, 0s, std::make_shared<FixedClock>(), wizz::DelayFunc{}}), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// CancellableMutex
// ---------------------------------------------------------------------------

TEST(CancellableMutex, Waiters_are_served_in_arrival_order)
{
    CancellableMutex mutex;
    std::mutex order_mutex;
    std::vector<int> order;
    mutex.lock({});
    std::vector<std::jthread> waiters;
    for (int id = 1; id <= 3; ++id) {
        waiters.emplace_back([&, id] {
            const CancellableLock guard{mutex, {}};
            const std::scoped_lock record{order_mutex};
            order.push_back(id);
        });
        wait_until([&] { return mutex.waiter_count() == static_cast<std::size_t>(id); });
    }
    mutex.unlock();
    waiters.clear();  // joins
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(mutex.waiter_count(), 0u);
}

TEST(CancellableMutex, A_cancelled_waiter_leaves_the_queue_without_owning_the_mutex)
{
    CancellableMutex mutex;
    mutex.lock({});

    std::stop_source cancel;
    std::exception_ptr first_error;
    std::jthread first{[&] {
        try {
            const CancellableLock guard{mutex, cancel.get_token()};
        } catch (...) {
            first_error = std::current_exception();
        }
    }};
    wait_until([&] { return mutex.waiter_count() == 1; });
    std::atomic<bool> second_owned{false};
    std::jthread second{[&] {
        const CancellableLock guard{mutex, {}};
        second_owned = true;
    }};
    wait_until([&] { return mutex.waiter_count() == 2; });

    cancel.request_stop();
    first.join();
    ASSERT_TRUE(first_error);
    EXPECT_THROW(std::rethrow_exception(first_error), OperationCancelled);
    EXPECT_EQ(mutex.waiter_count(), 1u);
    EXPECT_FALSE(second_owned.load());  // the holder still owns it

    mutex.unlock();
    second.join();
    EXPECT_TRUE(second_owned.load());
}

TEST(CancellableMutex, A_cancelled_token_fails_even_when_the_mutex_is_free)
{
    CancellableMutex mutex;
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_THROW(mutex.lock(cancel.get_token()), OperationCancelled);
    mutex.lock({});  // it was never taken
    mutex.unlock();
}

} // namespace
