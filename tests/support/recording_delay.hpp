#pragma once
// Recording delay (05 §1.3, §1.12): the injected `DelayFunc` of the client and
// the request gate. It records every requested wait and returns at once — it
// never sleeps — so back-off schedules are asserted exactly and tests stay fast.

#include "wizz/core/cancellation.hpp"
#include "wizz/core/time.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace wizz::test {

class RecordingDelay {
public:
    /// Runs after a delay was recorded and before the cancellation check, so a
    /// test can advance a FixedClock by the delay (R-01) or cancel the job from
    /// inside the wait (P-32).
    using Hook = std::function<void(Duration delay)>;

    explicit RecordingDelay(Hook on_delay = {});

    /// Appends `delay` (thread-safe), runs the hook, then throws OperationCancelled
    /// if `stop` was requested. Never sleeps.
    void operator()(Duration delay, std::stop_token stop) const;

    /// A `DelayFunc` sharing this recorder's list; it stays valid after the
    /// RecordingDelay itself is destroyed. Copies of a RecordingDelay share the list too.
    [[nodiscard]] DelayFunc delay_func() const;

    /// Snapshot of the recorded delays, in call order.
    [[nodiscard]] std::vector<Duration> delays() const;

private:
    struct State {
        Hook on_delay;
        std::mutex mutex;
        std::vector<Duration> delays;

        void record(Duration delay, const std::stop_token& stop);
    };
    std::shared_ptr<State> state_;
};

} // namespace wizz::test
