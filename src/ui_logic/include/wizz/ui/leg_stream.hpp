#pragma once
// The streaming side of RunLegAsync (01 §11.9, §14; 06 §2; 05 U-10, UI-2):
// partial batches from the worker are collected per run and flushed into the
// grid by a 150 ms UI timer; once the final result arrived (`finish`) late
// batches and flushes are ignored, and SetResults replaces the streamed rows
// with the authoritative list, so no row appears twice even after a fully
// cached re-run. Every worker event carries the job ID of its run; events of
// another (older) job are dropped. UI thread only.

#include "wizz/ui/flight_rows.hpp"

#include "wizz/core/domain.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wizz::ui {

class LegStream {
public:
    /// The row-coalescing interval (at most ~7 grid updates per second).
    static constexpr std::chrono::milliseconds kFlushInterval{150};

    explicit LegStream(std::uint64_t job_id) noexcept : job_id_(job_id) {}

    [[nodiscard]] std::uint64_t job_id() const noexcept { return job_id_; }
    /// True for events of this run while it is not finished.
    [[nodiscard]] bool accepts(std::uint64_t job_id) const noexcept { return job_id == job_id_ && !finished_; }

    /// `partial(batch)`: queued unless the run finished (late batches are ignored).
    void on_partial(std::span<const FlightResultPtr> batch);
    /// `partial(batch)` for an event tagged with `job_id`: ignored for another job.
    void on_partial(std::uint64_t job_id, std::span<const FlightResultPtr> batch);

    /// `Flush()`: nothing once finished or without pending rows; otherwise appends the
    /// pending rows to `grid`, clears them and sets the summary "{N} fares so far…" (N =
    /// all fares the grid holds). Returns true when the grid changed (update tab titles).
    bool flush(FlightGridModel& grid);

    /// The rows queued since the last take, in arrival order; empty once finished. A
    /// widget grid whose model must announce its own changes appends them itself, then
    /// sets the "{N} fares so far…" summary (the same steps as `flush`).
    [[nodiscard]] std::vector<FlightResultPtr> take_pending();

    /// The final result arrived (or the run failed): from here on batches are ignored.
    void finish() noexcept;
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

private:
    std::uint64_t job_id_;
    bool finished_ = false;
    std::vector<FlightResultPtr> pending_;
};

} // namespace wizz::ui
