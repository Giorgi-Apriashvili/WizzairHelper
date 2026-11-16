#include "timer_gap_probe.hpp"

#include "busy_panel.hpp"

#include <QTimer>

#include <algorithm>
#include <functional>

namespace wizz::app {

TimerGapProbe::TimerGapProbe(BusyPanel& busy, QObject* parent) : QObject(parent), busy_(busy)
{
    (void)CpuMeter::cycles_per_ms();  // measured now, not inside the first gap
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
    last_cpu_ = CpuMeter::sample();
    last_ns_ = now_ns();
}

void TimerGapProbe::on_tick()
{
    const std::int64_t now = now_ns();
    const CpuMeter::Sample cpu = CpuMeter::sample();
    ++stats_.ticks;
    add_gap(now, cpu);
    last_ns_ = now;
    last_cpu_ = cpu;
}

void TimerGapProbe::on_stopped()
{
    add_gap(now_ns(), CpuMeter::sample());
    last_ns_.reset();
}

void TimerGapProbe::exclude_begin()
{
    if (!excluding_since_ns_) {
        excluding_since_ns_ = now_ns();
        excluding_since_cpu_ = CpuMeter::sample();
    }
}

void TimerGapProbe::exclude_end()
{
    if (!excluding_since_ns_) {
        return;
    }
    const std::int64_t excluded = now_ns() - *excluding_since_ns_;
    const CpuMeter::Sample cpu = CpuMeter::sample();
    excluding_since_ns_.reset();
    if (!last_ns_) {
        return;  // the timer was not running: nothing to correct
    }
    *last_ns_ += excluded;
    last_cpu_ = CpuMeter::skip(last_cpu_, excluding_since_cpu_, cpu);
    const double excluded_ms = static_cast<double>(excluded) / 1'000'000.0;
    ++stats_.exclusions;
    stats_.excluded_ms += excluded_ms;
    stats_.longest_exclusion_ms = std::max(stats_.longest_exclusion_ms, excluded_ms);
}

void TimerGapProbe::add_gap(std::int64_t until_ns, const CpuMeter::Sample& until)
{
    if (!last_ns_) {
        return;
    }
    const double gap_ms = static_cast<double>(until_ns - *last_ns_) / 1'000'000.0;
    const CpuMeter::Interval cpu = CpuMeter::between(last_cpu_, until);
    const bool held = cpu.held_by_others();
    ++stats_.gaps;
    stats_.total_gap_ms += gap_ms;
    if (gap_ms > kRepaintLimitMs) {
        ++stats_.gaps_over_50_ms;
    }
    if (gap_ms >= kGapLimitMs) {
        ++stats_.gaps_over_100_ms;
        if (held) {
            ++stats_.held_gaps_over_100_ms;
        }
    }
    if (gap_ms > stats_.longest_gap_ms) {
        stats_.longest_gap_ms = gap_ms;
        stats_.longest_gap_status = busy_.status_text();
    }
    stats_.longest_gap_cpu_ms = std::max(stats_.longest_gap_cpu_ms, cpu.thread_ms);
    if (!held) {
        stats_.longest_own_gap_ms = std::max(stats_.longest_own_gap_ms, gap_ms);
    }
    std::vector<Gap>& kept = stats_.longest_gaps;
    if (kept.size() < kKeptGaps || gap_ms > kept.back().ms) {
        const Gap gap{.ms = gap_ms,
                      .cpu_ms = cpu.thread_ms,
                      .held_by_others = held,
                      .ended_at_ms = static_cast<double>(until_ns) / 1'000'000.0,
                      .status = busy_.status_text()};
        kept.insert(std::ranges::upper_bound(kept, gap_ms, std::greater<>{}, &Gap::ms), gap);
        if (kept.size() > kKeptGaps) {
            kept.pop_back();
        }
    }
}

} // namespace wizz::app
