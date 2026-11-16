#include "event_watchdog.hpp"

#include <QEvent>
#include <QMetaEnum>
#include <QMetaObject>
#include <QObject>

#include <algorithm>
#include <functional>
#include <utility>

namespace wizz::app {

namespace {

constexpr double kOver50Ms = 50.0;
constexpr double kOver100Ms = 100.0;

[[nodiscard]] std::string event_name(int type)
{
    const char* key = QMetaEnum::fromType<QEvent::Type>().valueToKey(type);
    return key != nullptr ? std::string{key} : std::to_string(type);
}

} // namespace

EventWatchdog::EventWatchdog(std::function<bool()> is_busy) : is_busy_(std::move(is_busy))
{
    (void)CpuMeter::cycles_per_ms();  // measured now, not inside the first dispatch
    clock_.start();
}

void EventWatchdog::enter(const QObject* receiver, const QEvent* event)
{
    const std::int64_t start = now_ns();
    if (sampled_ns_ < 0 || start - sampled_ns_ >= kSampleEveryNs) {
        sampled_ = CpuMeter::sample();
        sampled_ns_ = start;
    }
    stack_.push_back(Frame{.start_ns = start,
                           .nested_ns = 0,
                           .start_cycles = CpuMeter::thread_cycles(),
                           .nested_cycles = 0,
                           .window_start = sampled_,
                           .event_type = event != nullptr ? static_cast<int>(event->type()) : 0,
                           .receiver = receiver != nullptr ? receiver->metaObject()->className() : ""});
}

void EventWatchdog::leave()
{
    if (stack_.empty()) {
        return;
    }
    const Frame frame = stack_.back();
    stack_.pop_back();
    const std::uint64_t end_cycles = CpuMeter::thread_cycles();
    const std::int64_t now = now_ns();
    const std::int64_t total = now - frame.start_ns;
    const std::uint64_t cycles = end_cycles > frame.start_cycles ? end_cycles - frame.start_cycles : 0;
    if (!stack_.empty()) {
        stack_.back().nested_ns += total;  // the parent's own time excludes this dispatch
        stack_.back().nested_cycles += cycles;
    }
    Dispatch measured{
        .ms = static_cast<double>(std::max<std::int64_t>(0, total - frame.nested_ns)) / 1'000'000.0,
        .cpu_ms = static_cast<double>(cycles > frame.nested_cycles ? cycles - frame.nested_cycles : 0) /
                  CpuMeter::cycles_per_ms(),
    };
    if (measured.ms >= kOver50Ms) {
        // Long enough to matter: who held the CPUs? Shorter dispatches are below every limit.
        sampled_ = CpuMeter::sample();
        sampled_ns_ = now_ns();
        measured.held_by_others = CpuMeter::between(frame.window_start, sampled_).held_by_others();
    }
    record(frame, measured, now);
}

void EventWatchdog::exclude_begin()
{
    if (excluding_since_ns_ < 0) {
        excluding_since_ns_ = now_ns();
        excluding_since_cycles_ = CpuMeter::thread_cycles();
    }
}

void EventWatchdog::exclude_end()
{
    if (excluding_since_ns_ < 0) {
        return;
    }
    const std::int64_t excluded = now_ns() - excluding_since_ns_;
    const std::uint64_t cycles = CpuMeter::thread_cycles();
    excluding_since_ns_ = -1;
    if (!stack_.empty()) {
        stack_.back().nested_ns += excluded;  // like a nested dispatch: not this dispatch's work
        stack_.back().nested_cycles += cycles > excluding_since_cycles_ ? cycles - excluding_since_cycles_ : 0;
    }
}

void EventWatchdog::record(const Frame& frame, const Dispatch& measured, std::int64_t now)
{
    const double ms = measured.ms;
    ++stats_.dispatches;
    if (ms > kOver50Ms) {
        ++stats_.over_50_ms;
    }
    if (ms >= kOver100Ms) {
        ++stats_.over_100_ms;
    }
    stats_.longest_ms = std::max(stats_.longest_ms, ms);
    std::vector<Dispatch>& kept = stats_.longest;
    const bool busy = is_busy_ && is_busy_();
    if (busy) {
        stats_.longest_busy_ms = std::max(stats_.longest_busy_ms, ms);
        stats_.longest_busy_cpu_ms = std::max(stats_.longest_busy_cpu_ms, measured.cpu_ms);
        if (!measured.held_by_others) {
            stats_.longest_busy_own_ms = std::max(stats_.longest_busy_own_ms, ms);
        }
    }
    if (kept.size() < kKeptDispatches || ms > kept.back().ms) {
        Dispatch dispatch = measured;
        dispatch.ended_at_ms = static_cast<double>(now) / 1'000'000.0;
        dispatch.event = event_name(frame.event_type);
        dispatch.receiver = frame.receiver;
        dispatch.busy = busy;
        kept.insert(std::ranges::upper_bound(kept, ms, std::greater<>{}, &Dispatch::ms), std::move(dispatch));
        if (kept.size() > kKeptDispatches) {
            kept.pop_back();
        }
    }
}

} // namespace wizz::app
