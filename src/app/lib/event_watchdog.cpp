#include "event_watchdog.hpp"

#include <QEvent>
#include <QMetaEnum>
#include <QMetaObject>
#include <QObject>

#include <algorithm>
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
    clock_.start();
}

void EventWatchdog::enter(const QObject* receiver, const QEvent* event)
{
    stack_.push_back(Frame{.start_ns = now_ns(),
                           .nested_ns = 0,
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
    const std::int64_t now = now_ns();
    const std::int64_t total = now - frame.start_ns;
    if (!stack_.empty()) {
        stack_.back().nested_ns += total;  // the parent's own time excludes this dispatch
    }
    const double ms = static_cast<double>(std::max<std::int64_t>(0, total - frame.nested_ns)) / 1'000'000.0;
    record(frame, ms, now);
}

void EventWatchdog::exclude_begin()
{
    if (excluding_since_ns_ < 0) {
        excluding_since_ns_ = now_ns();
    }
}

void EventWatchdog::exclude_end()
{
    if (excluding_since_ns_ < 0) {
        return;
    }
    const std::int64_t excluded = now_ns() - excluding_since_ns_;
    excluding_since_ns_ = -1;
    if (!stack_.empty()) {
        stack_.back().nested_ns += excluded;  // like a nested dispatch: not this dispatch's work
    }
}

void EventWatchdog::record(const Frame& frame, double ms, std::int64_t now)
{
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
    }
    if (kept.size() < kKeptDispatches || ms > kept.back().ms) {
        Dispatch dispatch{.ms = ms,
                          .ended_at_ms = static_cast<double>(now) / 1'000'000.0,
                          .event = event_name(frame.event_type),
                          .receiver = frame.receiver,
                          .busy = busy};
        kept.insert(std::ranges::upper_bound(kept, ms, std::greater<>{}, &Dispatch::ms), std::move(dispatch));
        if (kept.size() > kKeptDispatches) {
            kept.pop_back();
        }
    }
}

} // namespace wizz::app
