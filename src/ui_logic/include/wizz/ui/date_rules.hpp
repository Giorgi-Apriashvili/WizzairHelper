#pragma once
// The date and nights coupling of the input panel as a window-free model
// (01 §4.4, §6.1 WireDateRules, §6.2, §11.6; 05 U-03).
//
// Four date pickers (outbound from/to, return from/to) share one range
// [MinDate, MaxDate] = [start-up day, start-up day + 2 calendar years]. A
// picker raises ValueChanged only when its value really changes; the four
// handlers below run for user and code changes alike, synchronously and
// re-entrantly, exactly as in the pseudocode:
//
//   on outFrom changed: delta = outFrom − lastOutFrom; lastOutFrom = outFrom   (ALWAYS)
//                       if adjusting: return
//                       adjusting { if outTo < outFrom: outTo = outFrom
//                                   if delta != 0: length = retTo − retFrom
//                                                  retFrom = Clamp(retFrom + delta)   (its handler runs)
//                                                  retTo   = Clamp(newFrom + length) }
//   on outTo changed:   if adjusting: return
//                       adjusting { if outFrom > outTo: outFrom = outTo     (nested: lastOutFrom only)
//                                   minEnd = Clamp(outTo + minNights)
//                                   if retTo < minEnd: retTo = minEnd
//                                   if retFrom > retTo: retFrom = retTo }
//   on retFrom changed: if retTo < retFrom: retTo = retFrom                  (no guard)
//   on retTo changed:   if retFrom > retTo: retFrom = retTo                  (no guard)
//
// Every programmatic assignment is clamped to the range (UI-11, UX-16). Dates
// are civil dates: the night filters are whole numbers (fractional typed
// values are truncated toward zero, 01 §4.4), so a picker never gets a
// non-midnight time (C-09). UI thread only.

#include "wizz/core/time.hpp"

#include <optional>

namespace wizz::ui {

/// DateTime.AddYears: the same month and day `years` later; 29 Feb becomes 28 Feb
/// in a non-leap target year.
[[nodiscard]] Date add_years(Date date, int years) noexcept;

/// The picker range shared by all four date pickers.
struct DateBounds {
    Date min;
    Date max;

    /// `Clamp(p, v)` of 01 §6.1.
    [[nodiscard]] constexpr Date clamp(Date value) const noexcept
    {
        return value < min ? min : value > max ? max : value;
    }
    friend constexpr bool operator==(const DateBounds&, const DateBounds&) = default;
};

/// An inclusive date window (autotest `--out` / `--ret`).
struct DateWindow {
    Date start;
    Date end;
    friend constexpr bool operator==(const DateWindow&, const DateWindow&) = default;
};

class DateRules {
public:
    /// `_minNights` range 0..60, `_maxNights` range 0..120 (01 §4.4).
    static constexpr int kMinNightsMaximum = 60;
    static constexpr int kMaxNightsMaximum = 120;

    /// The start-up state of 01 §4.4: range [today, today + 2 years], outbound
    /// [today+7, today+37], return [today+10, today+44], nights clamp(min, 0, 60) and
    /// max(clamp(max, 0, 120), min). The rules are wired after these defaults, so no
    /// handler runs here.
    [[nodiscard]] static DateRules for_today(Date today, int min_nights = 2, int max_nights = 10);

    /// Explicit start state (values clamped to `bounds`; nights to their ranges). No handler runs.
    DateRules(DateBounds bounds, Date out_from, Date out_to, Date ret_from, Date ret_to, int min_nights,
              int max_nights);

    // --- Picker assignments (user or code). The value is clamped to the range; the
    //     handlers run only when the value changes.
    void set_out_from(Date value);
    void set_out_to(Date value);
    void set_ret_from(Date value);
    void set_ret_to(Date value);

    /// Nights coupling (01 §6.2): on min change, max = min when max < min; on max change,
    /// min = min(max, 60) when min > max. Values are clamped to 0..60 / 0..120. Returns how
    /// many times `RecomputeTrips` must run (one per changed box: 0, 1 or 2 for a cascade).
    int set_min_nights(int value);
    int set_max_nights(int value);

    /// The right-click flow (01 §11.6, UX-9) under the guard: return window =
    /// [Clamp(outbound + minNights), Clamp(outbound + maxNights)] (to ≥ from), widening
    /// the return end first so from never exceeds to during the update. The unguarded
    /// return handlers still run.
    void plan_return_window(Date outbound_date);

    /// The autotest order (01 §17.2): outTo, outFrom, retTo, retFrom, each clamped and
    /// each triggering the rules. Missing windows leave the defaults.
    void apply_autotest_dates(const std::optional<DateWindow>& outbound, const std::optional<DateWindow>& inbound);

    [[nodiscard]] const DateBounds& bounds() const noexcept { return bounds_; }
    [[nodiscard]] Date out_from() const noexcept { return out_from_; }
    [[nodiscard]] Date out_to() const noexcept { return out_to_; }
    [[nodiscard]] Date ret_from() const noexcept { return ret_from_; }
    [[nodiscard]] Date ret_to() const noexcept { return ret_to_; }
    [[nodiscard]] int min_nights() const noexcept { return min_nights_; }
    [[nodiscard]] int max_nights() const noexcept { return max_nights_; }
    /// `_lastOutFrom`.
    [[nodiscard]] Date last_out_from() const noexcept { return last_out_from_; }
    /// `_adjustingDates` (false between calls).
    [[nodiscard]] bool adjusting() const noexcept { return adjusting_; }

private:
    // Raw assignments: set the value and run the handler when it changed.
    void assign_out_from(Date value);
    void assign_out_to(Date value);
    void assign_ret_from(Date value);
    void assign_ret_to(Date value);
    void on_out_from_changed();
    void on_out_to_changed();
    void on_ret_from_changed();
    void on_ret_to_changed();
    int assign_min_nights(int value);
    int assign_max_nights(int value);

    DateBounds bounds_;
    Date out_from_;
    Date out_to_;
    Date ret_from_;
    Date ret_to_;
    int min_nights_ = 2;
    int max_nights_ = 10;
    Date last_out_from_;
    bool adjusting_ = false;
};

} // namespace wizz::ui
