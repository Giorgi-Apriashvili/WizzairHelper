// BusyPanel widget (01 §10, §18.2-18.3; 05 U-05..U-08, R-04; 07 §4; D-04, D-11):
// texts, colours, enabled states, bar, spinner, timers, taskbar forwarding, Cancel/Esc
// idempotence and the font-driven layout of §10.1. Time comes from a FixedClock.

#include "busy_panel.hpp"
#include "diagnostics_test_support.hpp"
#include "elided_label.hpp"
#include "mock_taskbar_list.hpp"
#include "spinner_widget.hpp"
#include "system_colours.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/ui/strings.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::Duration;
using wizz::FixedClock;
using wizz::Locale;
using wizz::SearchProgress;
using wizz::app::BusyPanel;
using wizz::app::BusyPanelMetrics;
using wizz::app::system_colour;
using wizz::app::SystemColour;
using wizz::test::is_state_call;
using wizz::test::is_value_call;
using wizz::test::MockTaskbarList;
using wizz::test::pump_for;
using wizz::test::pump_until;
using wizz::test::save_snapshot;

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

[[nodiscard]] std::string text_of(const QLabel* label)
{
    return label->text().toStdString();
}

class BusyPanelTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        panel_ = std::make_unique<BusyPanel>(clock_, Locale::en_us(), mock_.factory());
        static_cast<void>(panel_->winId());  // taskbar calls need the window handle
    }

    void TearDown() override { panel_.reset(); }

    [[nodiscard]] QTimer* animation_timer() const
    {
        return panel_->findChild<QTimer*>(QStringLiteral("animationTimer"));
    }
    [[nodiscard]] QTimer* error_reset_timer() const
    {
        return panel_->findChild<QTimer*>(QStringLiteral("errorResetTimer"));
    }
    [[nodiscard]] std::string status() const { return panel_->status_label()->text().toStdString(); }
    [[nodiscard]] QColor status_colour() const
    {
        const auto* label = panel_->status_label();
        return label->palette().color(label->foregroundRole());
    }
    /// Lets the one-shot error timer fire now instead of after 4 s.
    void fire_error_reset_timer() const
    {
        QTimer* timer = error_reset_timer();
        timer->setInterval(0);
        timer->start();
        ASSERT_TRUE(pump_until([timer] { return !timer->isActive(); }));
    }

    FixedClock clock_;
    MockTaskbarList mock_;
    std::unique_ptr<BusyPanel> panel_;
};

// ---------------------------------------------------------------------------
// Initial state (01 §10.1)
// ---------------------------------------------------------------------------

TEST_F(BusyPanelTest, Initial_state_is_ready_and_idle)
{
    EXPECT_EQ(status(), "Ready.");
    EXPECT_EQ(panel_->status_text(), "Ready.");
    EXPECT_EQ(text_of(panel_->elapsed_label()), "");
    EXPECT_EQ(text_of(panel_->wait_label()), "");
    EXPECT_EQ(panel_->cancel_button()->text().toStdString(), "Cancel");
    EXPECT_FALSE(panel_->cancel_button()->isEnabled());
    EXPECT_FALSE(panel_->is_busy());
    EXPECT_EQ(panel_->updates_received(), 0);
    EXPECT_FALSE(panel_->spinner()->is_spinning());
    EXPECT_FALSE(animation_timer()->isActive());
    EXPECT_TRUE(mock_.calls.empty());
}

TEST_F(BusyPanelTest, Progress_bar_is_continuous_0_to_1001_without_text)
{
    const QProgressBar* bar = panel_->progress_bar();
    EXPECT_EQ(bar->minimum(), 0);
    EXPECT_EQ(bar->maximum(), 1001);
    EXPECT_EQ(bar->value(), 0);
    EXPECT_FALSE(bar->isTextVisible());
}

TEST_F(BusyPanelTest, Timers_are_a_precise_40_ms_tick_and_a_4_s_one_shot)
{
    const QTimer* timer = animation_timer();
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->interval(), 40);
    EXPECT_EQ(timer->timerType(), Qt::PreciseTimer);
    EXPECT_FALSE(timer->isSingleShot());

    const QTimer* reset = error_reset_timer();
    ASSERT_NE(reset, nullptr);
    EXPECT_EQ(reset->interval(), 4000);
    EXPECT_TRUE(reset->isSingleShot());
    EXPECT_FALSE(reset->isActive());
}

TEST_F(BusyPanelTest, Fonts_and_colours_follow_01_10_1)
{
    EXPECT_EQ(panel_->background_colour(), system_colour(SystemColour::control_light));

    const QFont elapsed = panel_->elapsed_label()->font();
    EXPECT_EQ(elapsed.family(), QStringLiteral("Courier New"));
    EXPECT_DOUBLE_EQ(elapsed.pointSizeF(), 9.5);
    EXPECT_TRUE(elapsed.bold());

    const QLabel* wait = panel_->wait_label();
    EXPECT_TRUE(wait->font().bold());
    EXPECT_EQ(wait->font().family(), panel_->font().family());
    EXPECT_DOUBLE_EQ(wait->font().pointSizeF(), panel_->font().pointSizeF());
    EXPECT_EQ(wait->palette().color(wait->foregroundRole()), QColor(255, 140, 0));  // DarkOrange

    EXPECT_EQ(status_colour(), system_colour(SystemColour::control_text));  // ControlText
    const QLabel* elapsed_label = panel_->elapsed_label();
    EXPECT_EQ(elapsed_label->palette().color(elapsed_label->foregroundRole()), system_colour(SystemColour::control_text));
}

TEST_F(BusyPanelTest, Background_is_painted_in_control_light)
{
    panel_->resize(500, panel_->height());
    const QImage image = panel_->grab().toImage();
    const QColor control_light = system_colour(SystemColour::control_light);  // COLOR_3DLIGHT
    EXPECT_EQ(image.pixelColor(1, 1), control_light);
    EXPECT_EQ(image.pixelColor(image.width() - 2, image.height() - 2), control_light);
    EXPECT_EQ(image.pixelColor(image.width() / 2, 2), control_light);  // above the bar (top padding)
}

// ---------------------------------------------------------------------------
// Begin / Report / End (01 §10.3, U-05)
// ---------------------------------------------------------------------------

TEST_F(BusyPanelTest, Begin_starts_a_cancellable_marquee_busy_period)
{
    panel_->begin("Loading Wizz Air route map…");

    EXPECT_TRUE(panel_->is_busy());
    EXPECT_EQ(status(), "Loading Wizz Air route map…");
    EXPECT_EQ(status_colour(), system_colour(SystemColour::control_text));
    EXPECT_TRUE(panel_->cancel_button()->isEnabled());
    EXPECT_EQ(panel_->progress_bar()->minimum(), 0);
    EXPECT_EQ(panel_->progress_bar()->maximum(), 0);  // marquee
    EXPECT_TRUE(panel_->spinner()->is_spinning());
    EXPECT_EQ(panel_->spinner()->angle(), 24);  // Begin ends with one Animate
    EXPECT_EQ(text_of(panel_->elapsed_label()), "0:00");
    EXPECT_EQ(text_of(panel_->wait_label()), "");
    EXPECT_TRUE(animation_timer()->isActive());
    ASSERT_EQ(mock_.calls.size(), 1U);
    EXPECT_TRUE(is_state_call(mock_.calls[0], TBPF_INDETERMINATE));
}

TEST_F(BusyPanelTest, Begin_without_cancel_keeps_cancel_disabled)
{
    panel_->begin("Working", false);
    EXPECT_FALSE(panel_->cancel_button()->isEnabled());
    panel_->request_cancel();
    EXPECT_EQ(status(), "Working");
}

TEST_F(BusyPanelTest, Report_shows_stage_detail_and_the_request_in_flight_U_05)
{
    panel_->begin("Outbound: starting…");
    panel_->report(progress("Fetching fares", "D", 2, 7));

    EXPECT_EQ(status(), "Fetching fares:  D    (3 of 7)");
    EXPECT_EQ(panel_->status_text(), "Fetching fares:  D    (3 of 7)");
    EXPECT_EQ(panel_->updates_received(), 1);
    EXPECT_EQ(panel_->progress_bar()->maximum(), 1001);  // continuous again
    // Eased 30 % towards 2/7 from 0: round(0.0857 × 1000).
    EXPECT_EQ(panel_->progress_bar()->value(), 86);
    EXPECT_EQ(panel_->spinner()->angle(), 48);

    panel_->report(progress("Fetching fares", "D", 2, 7));
    EXPECT_EQ(panel_->progress_bar()->value(), 146);  // 0.0857 + 0.2 × 0.3
    EXPECT_EQ(panel_->updates_received(), 2);

    ASSERT_EQ(mock_.calls.size(), 3U);
    EXPECT_TRUE(is_state_call(mock_.calls[0], TBPF_INDETERMINATE));
    EXPECT_TRUE(is_value_call(mock_.calls[1], 86));
    EXPECT_TRUE(is_value_call(mock_.calls[2], 146));
}

TEST_F(BusyPanelTest, Report_without_total_is_a_marquee_without_counter_U_05)
{
    panel_->begin("Finding the places you typed…");
    panel_->report(progress("Finding places", "Looking up \"Graz\" (Photon geocoder)", 0, 0));

    EXPECT_EQ(status(), "Finding places:  Looking up \"Graz\" (Photon geocoder)");
    EXPECT_EQ(panel_->progress_bar()->maximum(), 0);

    panel_->report(progress("Getting exchange rates", "", 0, 0));
    EXPECT_EQ(status(), "Getting exchange rates");
}

TEST_F(BusyPanelTest, Reports_outside_a_busy_period_are_ignored)
{
    panel_->report(progress("Fetching fares", "D", 1, 3));
    EXPECT_EQ(status(), "Ready.");
    EXPECT_EQ(panel_->updates_received(), 0);

    panel_->begin("Go");
    panel_->report(progress("Fetching fares", "D", 1, 3));
    panel_->end("Done.");
    panel_->report(progress("Fetching fares", "late", 2, 3));  // posted before End, delivered after

    EXPECT_EQ(status(), "Done.");
    EXPECT_EQ(panel_->updates_received(), 1);
    EXPECT_EQ(panel_->progress_bar()->value(), 1000);
}

TEST_F(BusyPanelTest, Updates_received_count_from_the_last_begin)
{
    panel_->begin("One");
    panel_->report(progress("Fetching fares", "", 0, 2));
    panel_->report(progress("Fetching fares", "", 1, 2));
    EXPECT_EQ(panel_->updates_received(), 2);
    panel_->end("ok");
    EXPECT_EQ(panel_->updates_received(), 2);  // kept for summary.json after End

    panel_->begin("Two");
    EXPECT_EQ(panel_->updates_received(), 0);
}

TEST_F(BusyPanelTest, A_burst_of_reports_counts_each_and_shows_the_last)
{
    panel_->begin("One");
    const std::vector<SearchProgress> burst{progress("Fetching fares", "BUD → NAP", 0, 3),
                                            progress("Fetching fares", "BUD → LTN", 1, 3),
                                            progress("Fetching fares", "BUD → BCN", 2, 3)};
    panel_->report(burst);
    EXPECT_EQ(panel_->updates_received(), 3) << "UpdatesReceived counts every report (summary.json)";
    EXPECT_EQ(status(), "Fetching fares:  BUD → BCN    (3 of 3)");
    panel_->report(std::span<const SearchProgress>{});
    EXPECT_EQ(panel_->updates_received(), 3);

    panel_->end("ok");
    panel_->report(burst);  // after End: ignored like single reports
    EXPECT_EQ(status(), "ok");
}

TEST_F(BusyPanelTest, Animation_start_and_stop_are_signalled_once_per_busy_period)
{
    int started = 0;
    int stopped = 0;
    QObject::connect(panel_.get(), &BusyPanel::animation_started, [&started] { ++started; });
    QObject::connect(panel_.get(), &BusyPanel::animation_stopped, [&stopped] { ++stopped; });
    EXPECT_EQ(panel_->animation_timer(), animation_timer());

    panel_->begin("One");
    panel_->report(progress("Fetching fares", "", 0, 2));
    panel_->begin("One again");  // still busy: the timer keeps running
    EXPECT_EQ(started, 1);
    EXPECT_EQ(stopped, 0);
    panel_->end("ok");
    panel_->end("ok again");
    EXPECT_EQ(stopped, 1);
    panel_->begin("Two");
    EXPECT_EQ(started, 2);
    panel_->end("done");
    EXPECT_EQ(stopped, 2);
}

TEST_F(BusyPanelTest, Successful_end_is_green_full_and_idle)
{
    panel_->begin("Finding the places you typed…");
    clock_.advance(seconds(5.9));
    panel_->end("Places found.");

    EXPECT_FALSE(panel_->is_busy());
    EXPECT_EQ(status(), "Places found.");
    EXPECT_EQ(status_colour(), QColor(0, 100, 0));
    EXPECT_EQ(panel_->status_colour(), QColor(0, 100, 0));
    EXPECT_EQ(text_of(panel_->elapsed_label()), "0:05");  // truncated, no ETA
    EXPECT_EQ(text_of(panel_->wait_label()), "");
    EXPECT_FALSE(panel_->cancel_button()->isEnabled());
    EXPECT_EQ(panel_->progress_bar()->maximum(), 1001);
    EXPECT_EQ(panel_->progress_bar()->value(), 1000);
    EXPECT_FALSE(panel_->spinner()->is_spinning());
    EXPECT_FALSE(animation_timer()->isActive());
    EXPECT_FALSE(error_reset_timer()->isActive());
    ASSERT_EQ(mock_.calls.size(), 2U);
    EXPECT_TRUE(is_state_call(mock_.calls[1], TBPF_NOPROGRESS));
}

TEST_F(BusyPanelTest, Failed_end_is_red_keeps_the_bar_and_clears_the_taskbar_error_after_4_s)
{
    panel_->begin("Outbound: starting…");
    panel_->report(progress("Fetching fares", "x", 4, 8));  // shown 0.15 → 150
    panel_->end("Cancelled.", false);

    EXPECT_EQ(status(), "Cancelled.");
    EXPECT_EQ(status_colour(), QColor(178, 34, 34));
    EXPECT_EQ(panel_->progress_bar()->value(), 150);  // (int)(0.15 × 1000): left where it was
    ASSERT_EQ(mock_.calls.size(), 3U);
    EXPECT_TRUE(is_state_call(mock_.calls[2], TBPF_ERROR));
    EXPECT_TRUE(error_reset_timer()->isActive());

    fire_error_reset_timer();
    ASSERT_EQ(mock_.calls.size(), 4U);
    EXPECT_TRUE(is_state_call(mock_.calls[3], TBPF_NOPROGRESS));
}

/// Counts the native style's animation frames sent to a widget.
class StyleAnimationCounter final : public QObject {
public:
    explicit StyleAnimationCounter(QObject* target) { target->installEventFilter(this); }
    int frames = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::StyleAnimationUpdate) {
            ++frames;
        }
        return QObject::eventFilter(watched, event);
    }
};

TEST_F(BusyPanelTest, An_idle_bar_is_still_after_a_search_ends)
{
    // The "windowsvista" style the app uses animates a moving glow over any bar
    // with 0 < value < maximum. After End the bar rests at 1000 of 1001 (success)
    // or part-filled (failure); it must not keep moving, or the app looks busy.
    std::unique_ptr<QStyle> vista{QStyleFactory::create(QStringLiteral("windowsvista"))};
    if (!vista) {
        GTEST_SKIP() << "windowsvista style not available";
    }
    QProgressBar* bar = panel_->progress_bar();
    bar->setStyle(vista.get());
    panel_->resize(800, 60);
    panel_->show();
    StyleAnimationCounter counter{bar};

    // Sanity: while busy and part-filled, the style does animate (the detector works).
    panel_->begin("Outbound: starting…");
    panel_->report(progress("Fetching fares", "x", 4, 8));
    bar->repaint();
    pump_for(300ms);
    EXPECT_GT(counter.frames, 0) << "the style's glow should run while busy";

    for (const bool success : {true, false}) {
        if (!success) {
            panel_->begin("Return: starting…");
            panel_->report(progress("Fetching fares", "x", 4, 8));
            bar->repaint();
            pump_for(100ms);
        }
        panel_->end(success ? "Done." : "Cancelled.", success);
        bar->repaint();
        pump_for(100ms);  // let a stopping animation deliver its last frame
        counter.frames = 0;
        pump_for(400ms);
        EXPECT_EQ(counter.frames, 0) << (success ? "after a successful end" : "after a failed end");
        if (success) {
            EXPECT_EQ(bar->value(), 1000);  // full, as End sets it
        } else {
            EXPECT_GT(bar->value(), 0);  // failure leaves the bar part-filled where it was
            EXPECT_LT(bar->value(), 1000);
        }
    }
    bar->setStyle(nullptr);
}

TEST_F(BusyPanelTest, The_error_reset_keeps_a_new_busy_period_untouched)
{
    panel_->begin("First");
    panel_->end("Failed.", false);
    panel_->begin("Second");
    fire_error_reset_timer();

    ASSERT_EQ(mock_.calls.size(), 3U);  // Indeterminate, Error, Indeterminate
    EXPECT_TRUE(is_state_call(mock_.calls[2], TBPF_INDETERMINATE));
    EXPECT_TRUE(panel_->is_busy());
}

TEST_F(BusyPanelTest, End_when_not_busy_still_recolours_and_retexts)
{
    panel_->begin("Searching");
    panel_->end("Search not started.", false);
    panel_->end("Search not started (cancelled).", true);
    EXPECT_EQ(status(), "Search not started (cancelled).");
    EXPECT_EQ(status_colour(), QColor(0, 100, 0));
}

TEST_F(BusyPanelTest, Beginning_again_restores_control_text)
{
    panel_->begin("One");
    panel_->end("Failed.", false);
    panel_->begin("Two");
    EXPECT_EQ(status_colour(), system_colour(SystemColour::control_text));
}

TEST_F(BusyPanelTest, End_if_still_busy_is_the_unexpected_error_safety_net)
{
    panel_->end_if_still_busy();
    EXPECT_EQ(status(), "Ready.");

    panel_->begin("Return: starting…");
    panel_->end_if_still_busy();
    EXPECT_FALSE(panel_->is_busy());
    EXPECT_EQ(status(), "Stopped by an unexpected error — details are in %LOCALAPPDATA%\\WizzairHelper\\errors.log.");
    EXPECT_EQ(status_colour(), QColor(178, 34, 34));

    panel_->end("Done.");
    panel_->end_if_still_busy();
    EXPECT_EQ(status(), "Done.");
}

// ---------------------------------------------------------------------------
// Elapsed, ETA and wait notes (01 §10.5, U-07, U-08)
// ---------------------------------------------------------------------------

TEST_F(BusyPanelTest, Elapsed_text_carries_the_eta_of_the_current_stage_U_08)
{
    panel_->begin("Outbound: starting…");
    panel_->report(progress("Fetching fares", "a", 0, 10));
    EXPECT_EQ(text_of(panel_->elapsed_label()), "0:00");  // nothing done yet

    clock_.advance(seconds(10));
    panel_->report(progress("Fetching fares", "c", 2, 10));
    EXPECT_EQ(text_of(panel_->elapsed_label()), "0:10  ~40 s left");

    panel_->report(progress("Fetching fares", "c", 2, 27));  // 25 left: 10 / 2 × 25 = 125 s
    EXPECT_EQ(text_of(panel_->elapsed_label()), "0:10  ~2 min 05 s left");
}

TEST_F(BusyPanelTest, Retry_countdown_and_slow_server_notes_U_07)
{
    panel_->begin("Outbound: starting…");

    SearchProgress retry = progress("Fetching fares", "Wizz Air is busy (HTTP 503) — retry 1 of 3", 0, 1);
    retry.waiting_until_utc = clock_.utc_now() + seconds(4.24);
    panel_->report(retry);
    EXPECT_EQ(text_of(panel_->wait_label()), "⏳ retrying in 4.2 s");

    SearchProgress slow = progress("Fetching fares", "BUD → London (LON)   10 Nov – 20 Nov", 0, 1);
    slow.step_started_utc = clock_.utc_now() - seconds(3.6);
    panel_->report(slow);
    EXPECT_EQ(text_of(panel_->wait_label()), "waiting for server… 4 s");

    SearchProgress chooser = progress("Waiting for your choice", "which place is “Graz”?", 0, 0);
    chooser.step_started_utc = clock_.utc_now() - seconds(30);
    chooser.waiting_for_user = true;
    panel_->report(chooser);
    EXPECT_EQ(text_of(panel_->wait_label()), "");
    EXPECT_EQ(panel_->progress_bar()->maximum(), 0);  // marquee while the user chooses
}

TEST(BusyPanelLocale, Wait_notes_use_the_culture_decimal_separator)
{
    FixedClock clock;
    MockTaskbarList mock;
    BusyPanel panel(clock, Locale::create("fi-FI"), mock.factory());
    panel.begin("x");
    SearchProgress retry = progress("Fetching fares", "retry", 0, 1);
    retry.waiting_until_utc = clock.utc_now() + seconds(4.24);
    panel.report(retry);
    EXPECT_EQ(panel.wait_label()->text().toStdString(), "⏳ retrying in 4,2 s");
}

TEST_F(BusyPanelTest, The_40_ms_timer_redraws_without_reports_R_04)
{
    panel_->begin("Loading Wizz Air route map…");
    const int angle = panel_->spinner()->angle();
    clock_.advance(seconds(61));

    int ticks = 0;
    QObject::connect(animation_timer(), &QTimer::timeout, panel_.get(), [&ticks] { ++ticks; });
    ASSERT_TRUE(pump_until([&ticks] { return ticks >= 3; }));

    EXPECT_EQ(text_of(panel_->elapsed_label()), "1:01");
    EXPECT_NE(panel_->spinner()->angle(), angle);

    panel_->end("Ready.");
    const int ticks_at_end = ticks;
    pump_for(120ms);
    EXPECT_EQ(ticks, ticks_at_end);  // the timer runs exactly while busy
}

// ---------------------------------------------------------------------------
// Cancel and Esc (01 §10.4, 07 §4, D-11)
// ---------------------------------------------------------------------------

TEST_F(BusyPanelTest, Cancel_and_esc_share_one_idempotent_handler)
{
    int signals = 0;
    QObject::connect(panel_.get(), &BusyPanel::cancel_requested, panel_.get(), [&signals] { ++signals; });

    panel_->begin("Outbound: starting (12 requests, about 16 s)…");
    panel_->cancel_button()->click();

    EXPECT_EQ(signals, 1);
    EXPECT_EQ(status(), "Cancelling… (finishing the current request)");
    EXPECT_FALSE(panel_->cancel_button()->isEnabled());
    EXPECT_TRUE(panel_->is_busy());

    panel_->request_cancel();  // Esc after the click
    panel_->request_cancel();
    panel_->cancel_button()->click();  // disabled: no effect
    EXPECT_EQ(signals, 1);
    EXPECT_EQ(status(), "Cancelling… (finishing the current request)");

    panel_->report(progress("Fetching fares", "next", 1, 12));  // the next report overwrites it
    EXPECT_EQ(status(), "Fetching fares:  next    (2 of 12)");
    panel_->request_cancel();
    EXPECT_EQ(signals, 1);
}

TEST_F(BusyPanelTest, A_new_cancellable_begin_rearms_cancel)
{
    int signals = 0;
    QObject::connect(panel_.get(), &BusyPanel::cancel_requested, panel_.get(), [&signals] { ++signals; });

    panel_->request_cancel();  // not busy
    EXPECT_EQ(signals, 0);
    EXPECT_EQ(status(), "Ready.");

    panel_->begin("One");
    panel_->request_cancel();
    panel_->end("Cancelled.", false);
    panel_->request_cancel();  // after End
    EXPECT_EQ(signals, 1);
    EXPECT_EQ(status(), "Cancelled.");

    panel_->begin("Two");
    panel_->request_cancel();
    EXPECT_EQ(signals, 2);
}

// ---------------------------------------------------------------------------
// Metrics and layout (01 §10.1, UI-6)
// ---------------------------------------------------------------------------

TEST_F(BusyPanelTest, Metrics_follow_01_10_1)
{
    const BusyPanelMetrics& metrics = panel_->metrics();
    EXPECT_EQ(metrics.padding, QMargins(8, 5, 8, 5));
    EXPECT_EQ(metrics.spinner_column, 36);
    EXPECT_GE(metrics.bar_row, 22);
    EXPECT_GT(metrics.line, 0);
    EXPECT_EQ(metrics.status_row, metrics.line + 6);
    EXPECT_EQ(metrics.height, metrics.bar_row + metrics.status_row + 10);
    EXPECT_EQ(panel_->height(), metrics.height);
    EXPECT_EQ(panel_->minimumHeight(), metrics.height);
    EXPECT_EQ(panel_->maximumHeight(), metrics.height);
    EXPECT_EQ(metrics, BusyPanel::compute_metrics(panel_->font(), panel_->wait_label()->font(),
                                                  panel_->elapsed_label()->font(), panel_.get()));
}

TEST_F(BusyPanelTest, A_dpi_change_remeasures_the_rows)
{
    // A child's own font change does not reach the panel as a FontChange; the
    // DevicePixelRatioChange sent after a move to another monitor makes it measure again.
    QFont large_wait = panel_->wait_label()->font();
    large_wait.setPointSizeF(30);
    panel_->wait_label()->setFont(large_wait);
    const BusyPanelMetrics stale = panel_->metrics();
    const BusyPanelMetrics expected = BusyPanel::compute_metrics(panel_->font(), panel_->wait_label()->font(),
                                                                 panel_->elapsed_label()->font(), panel_.get());
    ASSERT_GT(expected.line, stale.line);

    QEvent change(QEvent::DevicePixelRatioChange);
    QCoreApplication::sendEvent(panel_.get(), &change);
    EXPECT_EQ(panel_->metrics(), expected);
    EXPECT_EQ(panel_->height(), expected.height);
}

TEST_F(BusyPanelTest, Metrics_grow_with_the_form_font)
{
    QFont regular(QStringLiteral("Segoe UI"));  // not "small": <rpcndr.h> defines that as a macro
    regular.setPointSizeF(9);
    panel_->setFont(regular);
    const BusyPanelMetrics before = panel_->metrics();

    QFont large = regular;
    large.setPointSizeF(24);
    panel_->setFont(large);
    const BusyPanelMetrics after = panel_->metrics();

    EXPECT_GT(after.line, before.line);
    EXPECT_EQ(after.status_row, after.line + 6);
    EXPECT_GT(after.height, before.height);
    EXPECT_EQ(panel_->height(), after.height);
    EXPECT_EQ(after.bar_row, before.bar_row);  // the elapsed font is fixed
    EXPECT_DOUBLE_EQ(panel_->wait_label()->font().pointSizeF(), 24.0);
    EXPECT_TRUE(panel_->wait_label()->font().bold());
    EXPECT_DOUBLE_EQ(panel_->elapsed_label()->font().pointSizeF(), 9.5);
}

TEST(BusyPanelMetricsTest, Bar_row_grows_with_a_large_elapsed_font)
{
    QFont form(QStringLiteral("Segoe UI"));
    form.setPointSizeF(9);
    QFont wait = form;
    wait.setBold(true);
    const BusyPanelMetrics normal = BusyPanel::compute_metrics(form, wait, BusyPanel::elapsed_font());
    EXPECT_GE(normal.bar_row, 22);

    QFont huge = BusyPanel::elapsed_font();
    huge.setPointSizeF(40);
    const BusyPanelMetrics big = BusyPanel::compute_metrics(form, wait, huge);
    EXPECT_GT(big.bar_row, 22);
    EXPECT_EQ(big.bar_row, QFontMetrics(huge).size(Qt::TextSingleLine, QStringLiteral("0:00")).height() + 4);
    EXPECT_EQ(big.line, normal.line);
}

class BusyPanelLayoutTest : public BusyPanelTest {
protected:
    void SetUp() override
    {
        BusyPanelTest::SetUp();
        panel_->resize(kWidth, panel_->height());
        panel_->show();
        panel_->layout()->activate();
    }

    [[nodiscard]] QRect in_panel(const QWidget* widget) const
    {
        return {widget->mapTo(panel_.get(), QPoint(0, 0)), widget->size()};
    }

    static constexpr int kWidth = 640;
};

TEST_F(BusyPanelLayoutTest, Cells_sit_in_the_4_by_2_grid_with_the_01_10_1_margins)
{
    const BusyPanelMetrics& m = panel_->metrics();

    const QRect spinner = in_panel(panel_->spinner());
    EXPECT_EQ(spinner, QRect(8, 5, 36, m.bar_row + m.status_row));
    EXPECT_EQ(panel_->spinner()->contentsRect().width(), 30);  // Margin right 6

    const QRect bar = in_panel(panel_->progress_bar());
    const QRect status = in_panel(panel_->status_label());
    EXPECT_EQ(bar.left(), 44);
    EXPECT_EQ(bar.top(), 5 + 2);
    EXPECT_EQ(bar.height(), m.bar_row - 4);
    EXPECT_EQ(status.left(), 44);
    EXPECT_EQ(status.top(), 5 + m.bar_row);
    EXPECT_EQ(status.height(), m.status_row);
    EXPECT_EQ(bar.width(), status.width() - 8);  // the bar keeps 8 px to the elapsed column

    const QRect cancel = in_panel(panel_->cancel_button());
    EXPECT_EQ(cancel.right() + 1, kWidth - 8 - 3);
    EXPECT_NEAR(cancel.center().y(), 5 + (m.bar_row + m.status_row) / 2, 1);  // vertically centred

    const QRect elapsed = in_panel(panel_->elapsed_label());
    EXPECT_EQ(elapsed.left(), status.right() + 1);
}

TEST_F(BusyPanelLayoutTest, The_auto_column_grows_and_the_bar_narrows_for_a_wait_note)
{
    panel_->begin("Outbound: starting…");
    panel_->layout()->activate();
    const int bar_width = panel_->progress_bar()->width();

    SearchProgress retry = progress("Fetching fares", "Wizz Air is busy (HTTP 503) — retry 1 of 3", 0, 1);
    retry.waiting_until_utc = clock_.utc_now() + seconds(9.5);
    panel_->report(retry);
    panel_->layout()->activate();

    EXPECT_LT(panel_->progress_bar()->width(), bar_width);
    EXPECT_GE(panel_->wait_label()->width(),
              panel_->wait_label()->fontMetrics().horizontalAdvance(panel_->wait_label()->text()) + 8);
}

TEST_F(BusyPanelLayoutTest, A_long_status_line_is_elided_but_kept_whole)
{
    panel_->begin("x");
    const std::string detail(400, 'W');
    panel_->report(progress("Fetching fares", detail, 0, 0));

    const auto* label = panel_->status_label();
    const std::string expected = "Fetching fares:  " + detail;
    EXPECT_EQ(label->text().toStdString(), expected);
    EXPECT_EQ(panel_->status_text(), expected);
    EXPECT_TRUE(label->is_elided());
    EXPECT_TRUE(label->elided_text().endsWith(QChar(0x2026)));
    EXPECT_EQ(label->tooltip_text().toStdString(), expected);

    panel_->report(progress("Fetching fares", "short", 0, 0));
    EXPECT_FALSE(label->is_elided());
    EXPECT_TRUE(label->tooltip_text().isEmpty());
}

TEST_F(BusyPanelLayoutTest, Snapshots_for_visual_inspection)
{
    save_snapshot(*panel_, QStringLiteral("busy-0-ready"));

    panel_->begin("Outbound: starting (12 requests, about 16 s)…");
    clock_.advance(seconds(12));
    SearchProgress fetch = progress("Fetching fares", "KSC → London (LON)   20 Nov – 5 Dec", 4, 7);
    panel_->report(fetch);
    clock_.advance(seconds(4));
    fetch.step_started_utc = clock_.utc_now() - seconds(6);
    panel_->report(fetch);
    panel_->layout()->activate();
    save_snapshot(*panel_, QStringLiteral("busy-1-fetching"));

    panel_->end("Cancelled.", false);
    save_snapshot(*panel_, QStringLiteral("busy-2-failed"));
    panel_->end("Ready — 412 Wizz Air airports loaded. Choose your trip and press “Search outbound”.");
    save_snapshot(*panel_, QStringLiteral("busy-3-ready"));

    QFont large(QStringLiteral("Segoe UI"));
    large.setPointSizeF(18);
    panel_->setFont(large);
    panel_->begin("Loading Wizz Air route map…");
    panel_->layout()->activate();
    save_snapshot(*panel_, QStringLiteral("busy-4-large-font"));
    SUCCEED();
}

} // namespace
