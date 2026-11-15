#pragma once
// User cancellation is a separate kind of failure: never an API error and never
// a timeout (04 §10, 06 §2). One std::stop_token flows through planning, gate
// waits, delays, network reads and publication.

#include "wizz/core/time.hpp"

#include <exception>
#include <functional>
#include <stop_token>

namespace wizz {

/// Thrown when the caller's stop token is triggered. Equivalent of .NET
/// OperationCanceledException raised for a *user* cancel.
class OperationCancelled : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override { return "The operation was canceled."; }
};

inline void throw_if_cancelled(const std::stop_token& stop)
{
    if (stop.stop_requested()) {
        throw OperationCancelled{};
    }
}

/// Cancellable delay (`DelayFunc` in 04 §3.3). Must throw OperationCancelled
/// promptly when `stop` fires. Tests inject a recorder that returns immediately.
using DelayFunc = std::function<void(Duration delay, std::stop_token stop)>;

/// Real cancellable sleep (condition_variable_any + stop_token). Returns early
/// with OperationCancelled when `stop` fires; non-positive delays return at once.
void cancellable_sleep(Duration delay, std::stop_token stop);

} // namespace wizz
