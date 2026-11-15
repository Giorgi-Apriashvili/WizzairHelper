#include "support/recording_delay.hpp"

#include <utility>

namespace wizz::test {

void RecordingDelay::State::record(Duration delay, const std::stop_token& stop)
{
    {
        const std::scoped_lock lock{mutex};
        delays.push_back(delay);
    }
    if (on_delay) {
        on_delay(delay);
    }
    throw_if_cancelled(stop);
}

RecordingDelay::RecordingDelay(Hook on_delay) : state_(std::make_shared<State>())
{
    state_->on_delay = std::move(on_delay);
}

void RecordingDelay::operator()(Duration delay, std::stop_token stop) const
{
    state_->record(delay, stop);
}

DelayFunc RecordingDelay::delay_func() const
{
    return [state = state_](Duration delay, std::stop_token stop) { state->record(delay, stop); };
}

std::vector<Duration> RecordingDelay::delays() const
{
    const std::scoped_lock lock{state_->mutex};
    return state_->delays;
}

} // namespace wizz::test
