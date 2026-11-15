#include "timer_gap_probe.hpp"

#include "busy_panel.hpp"

#include <QTimer>

#include <algorithm>
#include <functional>

namespace wizz::app {

TimerGapProbe::TimerGapProbe(BusyPanel& busy, QObject* parent) : QObject(parent), busy_(busy)
{
    clock_.start();
    connect(&busy_, &BusyPanel::animation_started, this, &TimerGapProbe::on_started);
    connect(&busy_, &BusyPanel::animation_stopped, this, &TimerGapProbe::on_stopped);
    connect(busy_.animation_timer(), &QTimer::timeout, this, &TimerGapProbe::on_tick);
    if (busy_.animation_timer()->isActive()) {
        ++stats_.runs;  // measured from its next tick
    }
}

void TimerGapProbe::on_started()
{
    ++stats_.runs;
    last_ns_ = now_ns();
}

void TimerGapProbe::on_tick()
{
    const std::int64_t now = now_ns();
    ++stats_.ticks;
    add_gap(now);
    last_ns_ = now;
}

void TimerGapProbe::on_stopped()
{
    add_gap(now_ns());
    last_ns_.reset();
}

void TimerGapProbe::exclude_begin()
{
    if (!excluding_since_ns_) {
        excluding_since_ns_ = now_ns();
    }
}

void TimerGapProbe::exclude_end()
{
    if (!excluding_since_ns_) {
        return;
    }
    const std::int64_t excluded = now_ns() - *excluding_since_ns_;
    excluding_since_ns_.reset();
    if (!last_ns_) {
        return;  // the timer was not running: nothing to correct
    }
    *last_ns_ += excluded;
    const double excluded_ms = static_cast<double>(excluded) / 1'000'000.0;
    ++stats_.exclusions;
    stats_.excluded_ms += excluded_ms;
    stats_.longest_exclusion_ms = std::max(stats_.longest_exclusion_ms, excluded_ms);
}

void TimerGapProbe::add_gap(std::int64_t until_ns)
{
    if (!last_ns_) {
        return;
    }
    const double gap_ms = static_cast<double>(until_ns - *last_ns_) / 1'000'000.0;
    ++stats_.gaps;
    stats_.total_gap_ms += gap_ms;
    if (gap_ms > kRepaintLimitMs) {
        ++stats_.gaps_over_50_ms;
    }
    if (gap_ms >= kGapLimitMs) {
        ++stats_.gaps_over_100_ms;
    }
    if (gap_ms > stats_.longest_gap_ms) {
        stats_.longest_gap_ms = gap_ms;
        stats_.longest_gap_status = busy_.status_text();
    }
    std::vector<Gap>& kept = stats_.longest_gaps;
    if (kept.size() < kKeptGaps || gap_ms > kept.back().ms) {
        const Gap gap{.ms = gap_ms, .ended_at_ms = static_cast<double>(until_ns) / 1'000'000.0,
                      .status = busy_.status_text()};
        kept.insert(std::ranges::upper_bound(kept, gap_ms, std::greater<>{}, &Gap::ms), gap);
        if (kept.size() > kKeptGaps) {
            kept.pop_back();
        }
    }
}

} // namespace wizz::app
