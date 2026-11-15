#include "wizz/core/cancellation.hpp"

#include <condition_variable>
#include <mutex>

namespace wizz {

void cancellable_sleep(Duration delay, std::stop_token stop)
{
    throw_if_cancelled(stop);
    if (delay <= Duration::zero()) {
        return;
    }
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    // Returns true only when the stop token fired before the delay elapsed.
    const bool stopped = wake.wait_for(lock, stop, delay, [] { return false; });
    if (stopped || stop.stop_requested()) {
        throw OperationCancelled{};
    }
}

} // namespace wizz
