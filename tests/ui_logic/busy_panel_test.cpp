// BusyPanel state machine (01 §10; 05 §7.2 U-05..U-08, §7.3 R-04; 07 §4
// Esc/Cancel; D-11). Driven by a FixedClock; en-US unless a case says de-DE.

#include "wizz/ui/busy_panel_model.hpp"

#include "wizz/core/clocks.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace wizz;
using namespace wizz::ui;

[[nodiscard]] Duration seconds(double value)
{
    return Duration{static_cast<Duration::rep>(std::llround(value * 10'000'000.0))};
}

[[nodiscard]] SearchProgress progress(std::string stage, std::string detail, int completed, int total)
{
    SearchProgress p;
    p.stage = std::move(stage);
    p.detail = std::move(detail);
    p.completed = completed;
    p.total = total;
    return p;
}

[[nodiscard]] TaskbarCall state_call(TaskbarState state)
{
    return TaskbarCall{.kind = TaskbarCall::Kind::set_state, .state = state, .value = 0};
}

[[nodiscard]] TaskbarCall value_call(int value)
{
    return TaskbarCall{.kind = TaskbarCall::Kind::set_value, .state = TaskbarState::normal, .value = value};
}

class BusyPanel : public ::testing::Test {
protected:
    FixedClock clock_;
    BusyPanelModel panel_{clock_, Locale::en_us()};
};

// ---------------------------------------------------------------------------
// Initial state, Begin, End (01 §10.1, §10.3)
// ---------------------------------------------------------------------------

TEST_F(BusyPanel, Initial_view_is_ready_and_idle)
{
    const BusyView& view = panel_.view();
    EXPECT_EQ(view.status_text, "Ready.");
    EXPECT_EQ(view.status_tone, StatusTone::normal);
    EXPECT_EQ(view.elapsed_text, "");
    EXPECT_EQ(view.wait_text, "");
    EXPECT_FALSE(view.cancel_enabled);
    EXPECT_EQ(view.bar_style, BarStyle::continuous);
    EXPECT_EQ(view.bar_value, 0);
    EXPECT_FALSE(view.spinning);
    EXPECT_FALSE(panel_.is_busy());
    EXPECT_FALSE(panel_.timer_running());
    EXPECT_TRUE(panel_.take_taskbar_calls().empty());
}

TEST_F(BusyPanel, Begin_starts_an_indeterminate_cancellable_busy_period)
{
    panel_.begin("Loading Wizz Air route map\u2026");
    const BusyView& view = panel_.view();
    EXPECT_TRUE(panel_.is_busy());
    EXPECT_TRUE(panel_.timer_running());
    EXPECT_EQ(view.status_text, "Loading Wizz Air route map\u2026");
    EXPECT_EQ(view.status_tone, StatusTone::normal);
    EXPECT_TRUE(view.cancel_enabled);
    EXPECT_TRUE(view.spinning);
    EXPECT_EQ(view.bar_style, BarStyle::marquee);
    EXPECT_EQ(view.elapsed_text, "0:00");  // Begin ends with one Animate
    EXPECT_EQ(view.spinner_angle, 24);
    EXPECT_EQ(view.frame, 1u);
    EXPECT_LT(panel_.target(), 0.0);
    EXPECT_EQ(panel_.take_taskbar_calls(), (std::vector<TaskbarCall>{state_call(TaskbarState::indeterminate)}));

    panel_.begin("again", false);
    EXPECT_FALSE(panel_.view().cancel_enabled);
}

TEST_F(BusyPanel, End_success_fills_the_bar_and_colours_green)
{
    panel_.begin("Finding the places you typed\u2026");
    clock_.advance(5s);
    (void)panel_.take_taskbar_calls();
    panel_.end("Places found.");
    const BusyView& view = panel_.view();
    EXPECT_FALSE(panel_.is_busy());
    EXPECT_FALSE(panel_.timer_running());
    EXPECT_FALSE(view.spinning);
    EXPECT_FALSE(view.cancel_enabled);
    EXPECT_EQ(view.wait_text, "");
    EXPECT_EQ(view.bar_style, BarStyle::continuous);
    EXPECT_EQ(view.bar_value, 1000);
    EXPECT_EQ(view.elapsed_text, "0:05");  // no ETA suffix
    EXPECT_EQ(view.status_tone, StatusTone::success);
    EXPECT_EQ(view.status_text, "Places found.");
    EXPECT_EQ(panel_.take_taskbar_calls(), (std::vector<TaskbarCall>{state_call(TaskbarState::none)}));
    EXPECT_FALSE(panel_.take_error_reset_request());
}

TEST_F(BusyPanel, End_failure_truncates_the_bar_and_clears_the_taskbar_error_after_4_s)
{
    panel_.begin("Outbound: starting (2 requests, about 3 s)\u2026");
    panel_.report(progress("Fetching fares", "", 1, 2));  // 0.15
    panel_.animate();                                      // 0.255
    panel_.animate();                                      // 0.3285
    panel_.animate();                                      // 0.37995 → bar round(379.95) = 380
    EXPECT_EQ(panel_.view().bar_value, 380);
    (void)panel_.take_taskbar_calls();
    panel_.end("Cancelled.", false);
    EXPECT_EQ(panel_.view().bar_value, 379);  // (int)(0.37995 * 1000): truncation (07 §2)
    EXPECT_EQ(panel_.view().status_tone, StatusTone::failure);
    EXPECT_EQ(panel_.take_taskbar_calls(), (std::vector<TaskbarCall>{state_call(TaskbarState::error)}));
    EXPECT_TRUE(panel_.take_error_reset_request());
    EXPECT_FALSE(panel_.take_error_reset_request());
    EXPECT_EQ(BusyPanelModel::kErrorResetDelay, 4000ms);
    panel_.error_reset_elapsed();
    EXPECT_EQ(panel_.take_taskbar_calls(), (std::vector<TaskbarCall>{state_call(TaskbarState::none)}));
}

TEST_F(BusyPanel, Error_reset_does_nothing_once_a_new_busy_period_began)
{
    panel_.begin("a");
    panel_.end("Could not reach wizzair.com (offline?): x", false);
    (void)panel_.take_taskbar_calls();
    panel_.begin("b");
    (void)panel_.take_taskbar_calls();
    panel_.error_reset_elapsed();
    EXPECT_TRUE(panel_.take_taskbar_calls().empty());
    EXPECT_EQ(panel_.taskbar().last_state(), TaskbarState::indeterminate);
}

TEST_F(BusyPanel, End_when_not_busy_recolours_and_measures_from_the_last_begin)
{
    panel_.begin("Finding the places you typed\u2026");
    panel_.end("Places found.");
    clock_.advance(7s);  // e.g. a "Big search" question was open
    const std::uint64_t frame = panel_.view().frame;
    panel_.end("Search not started.");
    EXPECT_EQ(panel_.view().status_text, "Search not started.");
    EXPECT_EQ(panel_.view().status_tone, StatusTone::success);
    EXPECT_EQ(panel_.view().elapsed_text, "0:07");  // [WART] includes the dialog time
    EXPECT_GT(panel_.view().frame, frame);
    panel_.end("This search would need 501 requests \u2026", false);
    EXPECT_EQ(panel_.view().status_tone, StatusTone::failure);
}

TEST_F(BusyPanel, Reports_after_end_are_ignored)
{
    panel_.begin("x");
    panel_.report(progress("Fetching fares", "D", 0, 3));
    panel_.end("Outbound Done: 3 fares from 3 of 3 requests in 1 s.");
    const BusyView before = panel_.view();
    panel_.report(progress("Fetching fares", "late", 2, 3));
    panel_.animate();
    EXPECT_EQ(panel_.view(), before);
    EXPECT_EQ(panel_.updates_received(), 1);
}

TEST_F(BusyPanel, Updates_received_counts_reports_since_begin)
{
    panel_.begin("a");
    panel_.report(progress("Loading Wizz Air route map", "", 0, 0));
    panel_.report(progress("Getting exchange rates", "", 0, 0));
    EXPECT_EQ(panel_.updates_received(), 2);
    panel_.begin("b");
    EXPECT_EQ(panel_.updates_received(), 0);
    EXPECT_FALSE(panel_.last_report());
}

// ---------------------------------------------------------------------------
// U-05 status text and determinate / indeterminate bar (01 §10.3)
// ---------------------------------------------------------------------------

TEST(BusyPanelText, U_05_Status_text_with_detail_and_counter)
{
    EXPECT_EQ(progress_status_text(progress("Fetching fares", "D", 2, 7)), "Fetching fares:  D    (3 of 7)");
    EXPECT_EQ(progress_status_text(progress("Fetching fares", "D", 0, 0)), "Fetching fares:  D");
    EXPECT_EQ(progress_status_text(progress("Fetching fares", "", 2, 7)), "Fetching fares    (3 of 7)");
    EXPECT_EQ(progress_status_text(progress("Done", "", 7, 7)), "Done    (7 of 7)");  // capped at N
    EXPECT_EQ(progress_status_text(progress("Fetching fares", "KSC \u2192 London (LON)   20 Nov \u2013 5 Dec", 4, 7)),
              "Fetching fares:  KSC \u2192 London (LON)   20 Nov \u2013 5 Dec    (5 of 7)");
}

TEST_F(BusyPanel, U_05_Report_sets_the_text_and_the_bar_mode)
{
    panel_.begin("Outbound: starting (7 requests, about 9 s)\u2026");
    panel_.report(progress("Fetching fares", "D", 2, 7));
    EXPECT_EQ(panel_.status_text(), "Fetching fares:  D    (3 of 7)");
    EXPECT_EQ(panel_.view().bar_style, BarStyle::continuous);
    EXPECT_DOUBLE_EQ(panel_.target(), 2.0 / 7.0);
    panel_.report(progress("Fetching fares", "D", 2, 0));
    EXPECT_EQ(panel_.status_text(), "Fetching fares:  D");
    EXPECT_EQ(panel_.view().bar_style, BarStyle::marquee);
    EXPECT_LT(panel_.target(), 0.0);
}

// ---------------------------------------------------------------------------
// U-06 easing (01 §10.5)
// ---------------------------------------------------------------------------

TEST_F(BusyPanel, U_06_Easing_from_0_towards_0_5)
{
    panel_.begin("x");
    panel_.report(progress("Fetching fares", "", 1, 2));  // Report animates once
    EXPECT_DOUBLE_EQ(panel_.shown(), 0.15);
    EXPECT_EQ(panel_.view().bar_value, 150);
    panel_.animate();
    EXPECT_DOUBLE_EQ(panel_.shown(), 0.255);
    EXPECT_EQ(panel_.view().bar_value, 255);
    panel_.animate();
    EXPECT_DOUBLE_EQ(panel_.shown(), 0.3285);
    EXPECT_EQ(panel_.view().bar_value, 328);  // round half to even of 328.5
    panel_.animate();
    EXPECT_DOUBLE_EQ(panel_.shown(), 0.37995);
    EXPECT_EQ(panel_.view().bar_value, 380);

    // Snap: the gap before step k is 0.5·0.7^(k-1); step 17 sees a gap < 0.002 and snaps.
    for (int step = 5; step <= 16; ++step) {
        panel_.animate();
    }
    EXPECT_NE(panel_.shown(), 0.5);
    EXPECT_NEAR(panel_.shown(), 0.5 - 0.5 * std::pow(0.7, 16), 1e-12);
    panel_.animate();  // step 17
    EXPECT_EQ(panel_.shown(), 0.5);
    EXPECT_EQ(panel_.view().bar_value, 500);
}

TEST_F(BusyPanel, U_06_Ninety_percent_of_a_jump_takes_seven_ticks)
{
    panel_.begin("x");
    panel_.report(progress("Fetching fares", "", 0, 1));  // target 0: stays at 0
    EXPECT_EQ(panel_.shown(), 0.0);
    panel_.report(progress("Fetching fares", "", 1, 1));  // jump to 1, first tick
    for (int tick = 2; tick <= 6; ++tick) {
        panel_.animate();
    }
    EXPECT_LT(panel_.shown(), 0.9);  // 1 − 0.7^6 = 0.882
    panel_.animate();
    EXPECT_GE(panel_.shown(), 0.9);  // 1 − 0.7^7 = 0.918
}

// ---------------------------------------------------------------------------
// U-07 countdown and slow-step note (01 §10.5)
// ---------------------------------------------------------------------------

TEST_F(BusyPanel, U_07_Retry_countdown_and_waiting_for_server)
{
    panel_.begin("x");
    SearchProgress retry = progress("Fetching fares", "Wizz Air is busy (HTTP 503) \u2014 retry 1 of 3", 0, 1);
    retry.waiting_until_utc = clock_.utc_now() + seconds(4.24);
    retry.step_started_utc = clock_.utc_now();
    panel_.report(retry);
    EXPECT_EQ(panel_.view().wait_text, "\u23F3 retrying in 4.2 s");
    clock_.advance(seconds(0.2));
    panel_.animate();
    EXPECT_EQ(panel_.view().wait_text, "\u23F3 retrying in 4.0 s");
    clock_.advance(seconds(4.04));  // countdown over; the step started 4.24 s ago
    panel_.animate();
    EXPECT_EQ(panel_.view().wait_text, "waiting for server\u2026 4 s");

    SearchProgress slow = progress("Fetching fares", "BUD \u2192 London (LON)   10 Nov \u2013 20 Nov", 0, 1);
    slow.step_started_utc = clock_.utc_now() - seconds(3.6);
    panel_.report(slow);
    EXPECT_EQ(panel_.view().wait_text, "waiting for server\u2026 4 s");  // "0" format, half away from zero

    slow.waiting_for_user = true;
    panel_.report(slow);
    EXPECT_EQ(panel_.view().wait_text, "");
}

TEST_F(BusyPanel, Waiting_for_server_needs_more_than_3_s_and_a_known_step_start)
{
    panel_.begin("x");
    SearchProgress p = progress("Loading Wizz Air route map", "", 0, 0);
    panel_.report(p);
    clock_.advance(10s);
    panel_.animate();
    EXPECT_EQ(panel_.view().wait_text, "");  // no step start

    p.step_started_utc = clock_.utc_now() - 3s;
    panel_.report(p);
    EXPECT_EQ(panel_.view().wait_text, "");  // exactly 3 s is not more than 3 s
    clock_.advance(Duration{1});
    panel_.animate();
    EXPECT_EQ(panel_.view().wait_text, "waiting for server\u2026 3 s");
    clock_.advance(seconds(2.5));
    panel_.animate();
    EXPECT_EQ(panel_.view().wait_text, "waiting for server\u2026 6 s");  // 5.5000001 → 6
}

TEST(BusyPanelLocale, Countdown_uses_the_culture_decimal_separator_and_half_away_rounding)
{
    FixedClock clock;
    BusyPanelModel panel{clock, Locale::create("de-DE")};
    panel.begin("x");
    SearchProgress p = progress("Fetching fares", "", 0, 1);
    p.waiting_until_utc = clock.utc_now() + seconds(4.25);
    panel.report(p);
    EXPECT_EQ(panel.view().wait_text, "\u23F3 retrying in 4,3 s");
}

// ---------------------------------------------------------------------------
// U-08 remaining time (01 §10.5, §10.7)
// ---------------------------------------------------------------------------

TEST_F(BusyPanel, U_08_Eta_from_the_current_stage)
{
    panel_.begin("Outbound: starting (10 requests, about 13 s)\u2026");
    panel_.report(progress("Fetching fares", "a", 0, 10));
    EXPECT_EQ(panel_.view().elapsed_text, "0:00");  // nothing done yet: no ETA
    clock_.advance(10s);
    panel_.report(progress("Fetching fares", "b", 2, 10));
    EXPECT_EQ(panel_.view().elapsed_text, "0:10  ~40 s left");  // 10 s / 2 × 8
    clock_.advance(1s);
    panel_.animate();
    EXPECT_EQ(panel_.view().elapsed_text, "0:11  ~44 s left");  // rises while the request is slow
}

TEST_F(BusyPanel, U_08_Eta_above_a_minute_and_the_display_example)
{
    panel_.begin("x");
    panel_.report(progress("Fetching fares", "", 0, 9));
    clock_.advance(seconds(12.5));
    panel_.report(progress("Fetching fares", "", 1, 9));
    EXPECT_EQ(panel_.view().elapsed_text, "0:12  ~1 min 40 s left");  // 12.5 × 8 = 100 s

    panel_.begin("y");
    panel_.report(progress("Fetching fares", "", 0, 7));
    clock_.advance(seconds(12.4));
    panel_.report(progress("Fetching fares", "", 3, 7));
    EXPECT_EQ(panel_.view().elapsed_text, "0:12  ~17 s left");  // 12.4/3×4 = 16.53 → ceil 17

    panel_.begin("z");  // the 01 §10.7 display example
    panel_.report(progress("Fetching fares", "", 0, 37));
    clock_.advance(seconds(12.3));
    panel_.report(progress("Fetching fares", "", 12, 37));
    EXPECT_EQ(panel_.view().elapsed_text, "0:12  ~26 s left");  // 12.3/12×25 = 25.6 → ceil 26
    panel_.begin("z2");
    panel_.report(progress("Fetching fares", "", 0, 37));
    clock_.advance(12s);
    panel_.report(progress("Fetching fares", "", 12, 37));
    EXPECT_EQ(panel_.view().elapsed_text, "0:12  ~25 s left");
}

// ---------------------------------------------------------------------------
// The engine's progress sequences as the panel shows them (05 CT-FSS-01, P-31)
// ---------------------------------------------------------------------------

TEST_F(BusyPanel, CT_FSS_01_Progress_sequence)
{
    panel_.begin("Outbound: starting (3 requests, about 4 s)\u2026");
    const auto report = [&](std::string stage, std::string detail, int completed, int total) {
        SearchProgress p = progress(std::move(stage), std::move(detail), completed, total);
        p.step_started_utc = FixedClock::kDefaultUtcNow;
        panel_.report(p);
        return panel_.status_text();
    };
    EXPECT_EQ(report("Loading Wizz Air route map", "", 0, 0), "Loading Wizz Air route map");
    EXPECT_EQ(panel_.view().bar_style, BarStyle::marquee);
    EXPECT_EQ(report("Getting exchange rates", "", 0, 0), "Getting exchange rates");
    EXPECT_EQ(report("Getting exchange rates", "Downloading exchange rates (Frankfurter)", 0, 0),
              "Getting exchange rates:  Downloading exchange rates (Frankfurter)");
    EXPECT_EQ(report("Fetching fares", "BUD \u2192 London (LON)   10 Nov \u2013 20 Nov", 0, 3),
              "Fetching fares:  BUD \u2192 London (LON)   10 Nov \u2013 20 Nov    (1 of 3)");
    EXPECT_EQ(panel_.view().bar_style, BarStyle::continuous);
    EXPECT_EQ(report("Fetching fares", "BTS \u2192 London (LON)   10 Nov \u2013 20 Nov", 1, 3),
              "Fetching fares:  BTS \u2192 London (LON)   10 Nov \u2013 20 Nov    (2 of 3)");
    EXPECT_EQ(report("Fetching fares", "DEB \u2192 London (LON)   10 Nov \u2013 20 Nov", 2, 3),
              "Fetching fares:  DEB \u2192 London (LON)   10 Nov \u2013 20 Nov    (3 of 3)");
    EXPECT_EQ(report("Done", "", 3, 3), "Done    (3 of 3)");
    EXPECT_EQ(panel_.updates_received(), 7);
    EXPECT_EQ(panel_.view().wait_text, "");  // the fixed clock never moved: no slow step
    panel_.end("Outbound Done: 12 fares from 3 of 3 requests in 1 s.");
    EXPECT_EQ(panel_.view().bar_value, 1000);
}

TEST_F(BusyPanel, P_31_Back_off_inside_a_search)
{
    panel_.begin("Outbound: starting (1 requests, about 1 s)\u2026");
    SearchProgress fetch = progress("Fetching fares", "BUD \u2192 London (LON)   10 Nov \u2013 20 Nov", 0, 1);
    fetch.step_started_utc = clock_.utc_now();
    panel_.report(fetch);
    SearchProgress retry = progress("Fetching fares", "Wizz Air is busy (HTTP 503) \u2014 retry 1 of 3", 0, 1);
    retry.waiting_until_utc = make_utc(Date::from_ymd(2026, 11, 2), 10, 0, 5);
    retry.step_started_utc = clock_.utc_now();
    panel_.report(retry);
    EXPECT_EQ(panel_.status_text(), "Fetching fares:  Wizz Air is busy (HTTP 503) \u2014 retry 1 of 3    (1 of 1)");
    EXPECT_EQ(panel_.view().wait_text, "\u23F3 retrying in 5.0 s");
    for (int tick = 0; tick < 25; ++tick) {  // the countdown runs on the timer alone
        clock_.advance(BusyPanelModel::kTimerInterval);
        panel_.animate();
    }
    EXPECT_EQ(panel_.view().wait_text, "\u23F3 retrying in 4.0 s");
    clock_.advance(4s);
    panel_.animate();
    EXPECT_EQ(panel_.view().wait_text, "waiting for server\u2026 5 s");  // until "Retrying (1 of 3)" resets the step
    SearchProgress again = progress("Fetching fares", "Retrying (1 of 3)", 0, 1);
    again.step_started_utc = clock_.utc_now();
    panel_.report(again);
    EXPECT_EQ(panel_.view().wait_text, "");
    EXPECT_EQ(panel_.status_text(), "Fetching fares:  Retrying (1 of 3)    (1 of 1)");
}

TEST_F(BusyPanel, Eta_restarts_for_a_new_determinate_stage_and_needs_requests_left)
{
    panel_.begin("x");
    panel_.report(progress("Loading Wizz Air route map", "", 0, 0));
    clock_.advance(30s);
    panel_.report(progress("Fetching fares", "", 3, 10));  // new stage: base 3 at t = 30 s
    clock_.advance(10s);
    panel_.report(progress("Fetching fares", "", 5, 10));
    EXPECT_EQ(panel_.view().elapsed_text, "0:40  ~25 s left");  // 10 s / 2 × 5
    panel_.report(progress("Done", "", 10, 10));               // new stage, nothing left
    EXPECT_EQ(panel_.view().elapsed_text, "0:40");
}

// ---------------------------------------------------------------------------
// Cancel / Esc (01 §10.4, 07 §4, D-11)
// ---------------------------------------------------------------------------

TEST_F(BusyPanel, D_11_One_idempotent_cancel_handler)
{
    EXPECT_FALSE(panel_.request_cancel());  // not busy: nothing
    panel_.begin("Outbound: starting (7 requests, about 9 s)\u2026");
    panel_.report(progress("Fetching fares", "D", 2, 7));
    EXPECT_TRUE(panel_.request_cancel());
    EXPECT_EQ(panel_.status_text(), "Cancelling\u2026 (finishing the current request)");
    EXPECT_FALSE(panel_.view().cancel_enabled);
    EXPECT_FALSE(panel_.request_cancel());  // Esc after Cancel: one transition only
    EXPECT_FALSE(panel_.request_cancel());
    panel_.report(progress("Fetching fares", "E", 3, 7));
    EXPECT_EQ(panel_.status_text(), "Fetching fares:  E    (4 of 7)");  // the next report overwrites it
    EXPECT_FALSE(panel_.request_cancel());
    panel_.end("Outbound Cancelled: 3 fares from 3 of 7 requests in 5 s.");
    EXPECT_FALSE(panel_.request_cancel());
    panel_.begin("Finding the places you typed\u2026");
    EXPECT_TRUE(panel_.request_cancel());  // a new cancellable job
    panel_.begin("not cancellable", false);
    EXPECT_FALSE(panel_.request_cancel());
}

// ---------------------------------------------------------------------------
// Taskbar (01 §10.9)
// ---------------------------------------------------------------------------

TEST(TaskbarProgressModel, De_duplicates_states_and_values)
{
    TaskbarProgress taskbar;
    taskbar.set_state(TaskbarState::none);  // already None
    EXPECT_TRUE(taskbar.take_calls().empty());
    taskbar.set_state(TaskbarState::indeterminate);
    taskbar.set_state(TaskbarState::indeterminate);
    taskbar.set_value(150);
    taskbar.set_value(150);
    taskbar.set_value(1500);  // clamped
    taskbar.set_value(-5);
    taskbar.set_state(TaskbarState::normal);  // already Normal after a value
    taskbar.set_state(TaskbarState::error);
    taskbar.set_value(0);  // forced after a state change even though 0 was the last value
    EXPECT_EQ(taskbar.take_calls(),
              (std::vector<TaskbarCall>{state_call(TaskbarState::indeterminate), value_call(150), value_call(1000),
                                        value_call(0), state_call(TaskbarState::error), value_call(0)}));
    EXPECT_EQ(taskbar.last_state(), TaskbarState::normal);
}

TEST_F(BusyPanel, Taskbar_follows_the_lifecycle_without_repeats)
{
    panel_.begin("x");
    panel_.animate();
    panel_.animate();
    panel_.report(progress("Loading Wizz Air route map", "", 0, 0));
    EXPECT_EQ(panel_.take_taskbar_calls(), (std::vector<TaskbarCall>{state_call(TaskbarState::indeterminate)}));
    panel_.report(progress("Fetching fares", "", 1, 2));
    panel_.animate();
    EXPECT_EQ(panel_.take_taskbar_calls(), (std::vector<TaskbarCall>{value_call(150), value_call(255)}));
    panel_.report(progress("Fetching fares", "", 1, 0));  // indeterminate again
    panel_.animate();
    EXPECT_EQ(panel_.take_taskbar_calls(), (std::vector<TaskbarCall>{state_call(TaskbarState::indeterminate)}));
    panel_.end("done");
    EXPECT_EQ(panel_.take_taskbar_calls(), (std::vector<TaskbarCall>{state_call(TaskbarState::none)}));
}

// ---------------------------------------------------------------------------
// R-04 timing contract and the spinner (01 §10, §10.8)
// ---------------------------------------------------------------------------

TEST_F(BusyPanel, R_04_Repaints_every_tick_while_busy_independent_of_reports)
{
    static_assert(BusyPanelModel::kTimerInterval <= 50ms);
    panel_.begin("Outbound: starting (3 requests, about 4 s)\u2026");
    panel_.report(progress("Fetching fares", "BUD \u2192 London (LON)   10 Nov \u2013 20 Nov", 0, 3));
    std::uint64_t frame = panel_.view().frame;
    int angle = panel_.view().spinner_angle;
    std::string elapsed = panel_.view().elapsed_text;
    int elapsed_changes = 0;
    for (int tick = 0; tick < 250; ++tick) {  // 10 s without any report
        ASSERT_TRUE(panel_.timer_running());
        clock_.advance(BusyPanelModel::kTimerInterval);
        panel_.animate();
        EXPECT_EQ(panel_.view().frame, frame + 1);
        frame = panel_.view().frame;
        EXPECT_EQ(panel_.view().spinner_angle, next_spinner_angle(angle));
        angle = panel_.view().spinner_angle;
        if (panel_.view().elapsed_text != elapsed) {
            ++elapsed_changes;
            elapsed = panel_.view().elapsed_text;
        }
    }
    EXPECT_EQ(panel_.view().elapsed_text, "0:10");
    EXPECT_EQ(elapsed_changes, 10);
    EXPECT_EQ(panel_.view().wait_text, "");  // no step start in that report
    panel_.end("x");
    EXPECT_FALSE(panel_.timer_running());
    const std::uint64_t ended = panel_.view().frame;
    panel_.animate();
    EXPECT_EQ(panel_.view().frame, ended);  // the timer is stopped: no more repaints
}

TEST(Spinner, Step_and_geometry)
{
    EXPECT_EQ(next_spinner_angle(0), 24);
    EXPECT_EQ(next_spinner_angle(336), 0);
    EXPECT_EQ(next_spinner_angle(348), 12);

    const std::optional<SpinnerGeometry> geometry = spinner_geometry(36, 40);
    ASSERT_TRUE(geometry);
    EXPECT_EQ(geometry->size, 28);
    EXPECT_EQ(geometry->left, 4);
    EXPECT_EQ(geometry->top, 6);
    EXPECT_FLOAT_EQ(geometry->thickness, 4.0F);
    EXPECT_EQ(geometry->dot_diameter, 6);
    EXPECT_EQ(geometry->dot_left, 15);
    EXPECT_EQ(geometry->dot_top, 17);
    EXPECT_EQ(geometry->sweep_degrees, 100);

    EXPECT_FALSE(spinner_geometry(15, 40));  // size 7 < 8: nothing drawn
    const std::optional<SpinnerGeometry> small = spinner_geometry(16, 16);
    ASSERT_TRUE(small);
    EXPECT_FLOAT_EQ(small->thickness, 2.5F);
    EXPECT_EQ(small->dot_diameter, 4);
}

} // namespace
