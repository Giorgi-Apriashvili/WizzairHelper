#include "wizz/ui/date_rules.hpp"

#include <algorithm>
#include <chrono>

namespace wizz::ui {

namespace {

/// `_adjustingDates = true; try { … } finally { _adjustingDates = false; }`.
class AdjustingScope {
public:
    explicit AdjustingScope(bool& flag) noexcept : flag_(flag) { flag_ = true; }
    ~AdjustingScope() { flag_ = false; }
    AdjustingScope(const AdjustingScope&) = delete;
    AdjustingScope& operator=(const AdjustingScope&) = delete;

private:
    bool& flag_;
};

[[nodiscard]] int clamp_min_nights(int value) noexcept
{
    return std::clamp(value, 0, DateRules::kMinNightsMaximum);
}

[[nodiscard]] int clamp_max_nights(int value) noexcept
{
    return std::clamp(value, 0, DateRules::kMaxNightsMaximum);
}

} // namespace

Date add_years(Date date, int years) noexcept
{
    const std::chrono::year_month_day moved = date.ymd() + std::chrono::years{years};
    if (moved.ok()) {
        return Date{moved};
    }
    return Date{std::chrono::year_month_day{std::chrono::year_month_day_last{moved.year(), std::chrono::month_day_last{moved.month()}}}};
}

DateRules DateRules::for_today(Date today, int min_nights, int max_nights)
{
    const int min_value = clamp_min_nights(min_nights);
    const int max_value = std::max(clamp_max_nights(max_nights), min_value);
    return DateRules{DateBounds{today, add_years(today, 2)},
                     today.add_days(7),
                     today.add_days(37),
                     today.add_days(10),
                     today.add_days(44),
                     min_value,
                     max_value};
}

DateRules::DateRules(DateBounds bounds, Date out_from, Date out_to, Date ret_from, Date ret_to, int min_nights,
                     int max_nights)
    : bounds_(bounds),
      out_from_(bounds.clamp(out_from)),
      out_to_(bounds.clamp(out_to)),
      ret_from_(bounds.clamp(ret_from)),
      ret_to_(bounds.clamp(ret_to)),
      min_nights_(clamp_min_nights(min_nights)),
      max_nights_(clamp_max_nights(max_nights)),
      last_out_from_(out_from_)
{
}

void DateRules::set_out_from(Date value)
{
    assign_out_from(bounds_.clamp(value));
}

void DateRules::set_out_to(Date value)
{
    assign_out_to(bounds_.clamp(value));
}

void DateRules::set_ret_from(Date value)
{
    assign_ret_from(bounds_.clamp(value));
}

void DateRules::set_ret_to(Date value)
{
    assign_ret_to(bounds_.clamp(value));
}

int DateRules::set_min_nights(int value)
{
    return assign_min_nights(value);
}

int DateRules::set_max_nights(int value)
{
    return assign_max_nights(value);
}

void DateRules::plan_return_window(Date outbound_date)
{
    const AdjustingScope scope{adjusting_};
    const Date from = bounds_.clamp(outbound_date.add_days(min_nights_));
    Date to = bounds_.clamp(outbound_date.add_days(max_nights_));
    if (to < from) {
        to = from;
    }
    assign_ret_to(bounds_.clamp(std::max(to, ret_to_)));  // widen first
    assign_ret_from(from);
    assign_ret_to(to);
}

void DateRules::apply_autotest_dates(const std::optional<DateWindow>& outbound, const std::optional<DateWindow>& inbound)
{
    if (outbound) {
        set_out_to(outbound->end);
        set_out_from(outbound->start);
    }
    if (inbound) {
        set_ret_to(inbound->end);
        set_ret_from(inbound->start);
    }
}

void DateRules::assign_out_from(Date value)
{
    if (value == out_from_) {
        return;  // a picker raises ValueChanged only on a real change
    }
    out_from_ = value;
    on_out_from_changed();
}

void DateRules::assign_out_to(Date value)
{
    if (value == out_to_) {
        return;
    }
    out_to_ = value;
    on_out_to_changed();
}

void DateRules::assign_ret_from(Date value)
{
    if (value == ret_from_) {
        return;
    }
    ret_from_ = value;
    on_ret_from_changed();
}

void DateRules::assign_ret_to(Date value)
{
    if (value == ret_to_) {
        return;
    }
    ret_to_ = value;
    on_ret_to_changed();
}

void DateRules::on_out_from_changed()
{
    const std::int64_t delta = days_between(out_from_, last_out_from_);
    last_out_from_ = out_from_;  // always, even when guarded
    if (adjusting_) {
        return;
    }
    const AdjustingScope scope{adjusting_};
    if (out_to_ < out_from_) {
        assign_out_to(out_from_);  // its handler returns early (guard)
    }
    if (delta != 0) {
        const std::int64_t length = days_between(ret_to_, ret_from_);
        const Date new_from = bounds_.clamp(ret_from_.add_days(delta));
        assign_ret_from(new_from);  // unguarded handler runs
        assign_ret_to(bounds_.clamp(new_from.add_days(length)));
    }
}

void DateRules::on_out_to_changed()
{
    if (adjusting_) {
        return;
    }
    const AdjustingScope scope{adjusting_};
    if (out_from_ > out_to_) {
        assign_out_from(out_to_);  // nested: updates _lastOutFrom, then returns (no return shift)
    }
    const Date min_return_end = bounds_.clamp(out_to_.add_days(min_nights_));
    if (ret_to_ < min_return_end) {
        assign_ret_to(min_return_end);
    }
    if (ret_from_ > ret_to_) {
        assign_ret_from(ret_to_);
    }
}

void DateRules::on_ret_from_changed()
{
    if (ret_to_ < ret_from_) {
        assign_ret_to(ret_from_);
    }
}

void DateRules::on_ret_to_changed()
{
    if (ret_from_ > ret_to_) {
        assign_ret_from(ret_to_);
    }
}

int DateRules::assign_min_nights(int value)
{
    const int clamped = clamp_min_nights(value);
    if (clamped == min_nights_) {
        return 0;
    }
    min_nights_ = clamped;
    int recomputes = 0;
    if (max_nights_ < min_nights_) {
        recomputes += assign_max_nights(min_nights_);
    }
    return recomputes + 1;  // RecomputeTrips()
}

int DateRules::assign_max_nights(int value)
{
    const int clamped = clamp_max_nights(value);
    if (clamped == max_nights_) {
        return 0;
    }
    max_nights_ = clamped;
    int recomputes = 0;
    if (min_nights_ > max_nights_) {
        recomputes += assign_min_nights(std::min(max_nights_, kMinNightsMaximum));
    }
    return recomputes + 1;  // RecomputeTrips()
}

} // namespace wizz::ui
