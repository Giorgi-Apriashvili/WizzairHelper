#include "wizz/ui/leg_stream.hpp"

#include "wizz/ui/strings.hpp"

#include <utility>

namespace wizz::ui {

void LegStream::on_partial(std::span<const FlightResultPtr> batch)
{
    if (finished_) {
        return;
    }
    pending_.insert(pending_.end(), batch.begin(), batch.end());
}

void LegStream::on_partial(std::uint64_t job_id, std::span<const FlightResultPtr> batch)
{
    if (job_id != job_id_) {
        return;  // a stale event of an earlier job
    }
    on_partial(batch);
}

bool LegStream::flush(FlightGridModel& grid)
{
    if (finished_ || pending_.empty()) {
        return false;
    }
    const std::vector<FlightResultPtr> batch = std::exchange(pending_, {});
    grid.append(batch);
    grid.set_summary(strings::fares_so_far(grid.all_results().size()));
    return true;
}

void LegStream::finish() noexcept
{
    finished_ = true;
    pending_.clear();
}

} // namespace wizz::ui
