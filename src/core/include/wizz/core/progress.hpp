#pragma once
// Status records that cross from the engine to the UI (03 §5.1, 04 §3.1).
// Sinks are invoked synchronously on the reporting (worker) thread; the UI is
// responsible for marshalling them to its own thread.

#include "wizz/core/time.hpp"

#include <functional>
#include <optional>
#include <string>

namespace wizz {

/// A short status text the client emits while it waits. `wait_until_utc` is set
/// only for timed back-offs (the UI shows a live countdown from it).
struct ClientNotice {
    std::string message;
    std::optional<UtcInstant> wait_until_utc;

    friend bool operator==(const ClientNotice&, const ClientNotice&) = default;
};

/// Empty function = "no notices".
using NoticeSink = std::function<void(const ClientNotice&)>;

/// Immutable progress snapshot, one per report.
struct SearchProgress {
    std::string stage;                           ///< "Loading Wizz Air route map", "Fetching fares", ...
    std::string detail;                          ///< route text or the latest client notice
    int completed = 0;                           ///< plan entries finished before the current one
    int total = 0;                               ///< 0 = unknown (indeterminate bar)
    std::optional<UtcInstant> waiting_until_utc; ///< set only during a timed back-off
    std::optional<UtcInstant> step_started_utc;  ///< nullopt = "unset"
    bool waiting_for_user = false;               ///< never set by the engine; the UI sets it for dialogs

    friend bool operator==(const SearchProgress&, const SearchProgress&) = default;
};

using ProgressSink = std::function<void(const SearchProgress&)>;

} // namespace wizz
