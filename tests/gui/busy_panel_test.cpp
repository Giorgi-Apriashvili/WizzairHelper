// BusyPanel widget (01 §10, §18.2-18.3; 05 U-05..U-08, R-04; 07 §4; D-04, D-11, D-34) in
// the look of D-53: texts, the theme's colours, enabled states, bar, spinner, timers,
// taskbar forwarding, Cancel/Esc idempotence, the completely still idle bar and the
// font-driven layout of §10.1. Time comes from a FixedClock.

#include "busy_panel.hpp"
#include "diagnostics_test_support.hpp"
#include "elided_label.hpp"
#include "mock_taskbar_list.hpp"
#include "spinner_widget.hpp"
#include "ui_style.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/ui/strings.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPalette>
#include <QProgressBar>
#include <QAbstractAnimation>
#include <QImage>
#include <QProxyStyle>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>


namespace {

namespace style = wizz::app::style;
using namespace std::chrono_literals;
using wizz::Duration;
using wizz::FixedClock;
using wizz::Locale;
using wizz::SearchProgress;
using wizz::app::BusyPanel;
using wizz::app::BusyPanelMetrics;
using wizz::app::PanelProgressBar;
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

TEST_F(BusyPanelTest, Fonts_and_colours_follow_the_theme)
{
    // 01 §10.1's ControlLight / ControlText / DarkOrange and Courier New became the look's
    // tokens and fonts (D-53): a status bar on the surface colour, the elapsed time in
    // Cascadia Mono 10 pt semibold, the wait note semibold in the warning colour.
    const style::Theme& theme = style::theme();
    EXPECT_EQ(panel_->background_colour(), theme.surface);

    const QFont elapsed = panel_->elapsed_label()->font();
    EXPECT_EQ(elapsed, BusyPanel::elapsed_font());
    EXPECT_EQ(elapsed.families(), style::mono_font(10.0).families());
    // Cascadia Mono, Consolas under WIZZ_FONTS=windows10 (a Windows 10 without Windows Terminal).
    EXPECT_EQ(elapsed.families().front(), style::windows10_fonts() ? QStringLiteral("Consolas") : QStringLiteral("Cascadia Mono"));
    EXPECT_DOUBLE_EQ(elapsed.pointSizeF(), 10.0);
    EXPECT_EQ(elapsed.weight(), QFont::DemiBold);

    const QLabel* wait = panel_->wait_label();
    EXPECT_EQ(wait->font().weight(), QFont::DemiBold);
    EXPECT_EQ(wait->font().families(), panel_->font().families());
    EXPECT_DOUBLE_EQ(wait->font().pointSizeF(), panel_->font().pointSizeF());
    EXPECT_EQ(wait->palette().color(wait->foregroundRole()), theme.warning);

    EXPECT_EQ(status_colour(), theme.text);
    EXPECT_EQ(panel_->status_colour(), theme.text);
    const QLabel* elapsed_label = panel_->elapsed_label();
    EXPECT_EQ(elapsed_label->palette().color(elapsed_label->foregroundRole()), theme.text_secondary);
}

TEST_F(BusyPanelTest, Background_is_the_surface_under_a_top_border)
{
    panel_->resize(500, panel_->height());
    const QImage image = panel_->grab().toImage();
    const style::Theme& theme = style::theme();
    EXPECT_EQ(image.pixelColor(1, 1), theme.surface);
    EXPECT_EQ(image.pixelColor(image.width() - 2, image.height() - 2), theme.surface);
    EXPECT_EQ(image.pixelColor(image.width() / 2, 2), theme.surface);  // above the bar (top padding)
    for (const int x : {0, image.width() / 2, image.width() - 1}) {
        EXPECT_EQ(image.pixelColor(x, 0), theme.border) << "the status bar's 1 px top edge at x " << x;
    }
}

// ---------------------------------------------------------------------------
// Begin / Report / End (01 §10.3, U-05)
// ---------------------------------------------------------------------------

TEST_F(BusyPanelTest, Begin_starts_a_cancellable_marquee_busy_period)
{
    panel_->begin("Loading Wizz Air route map…");

    EXPECT_TRUE(panel_->is_busy());
    EXPECT_EQ(status(), "Loading Wizz Air route map…");
    EXPECT_EQ(status_colour(), style::theme().text);
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
    EXPECT_EQ(status_colour(), style::theme().success);
    EXPECT_EQ(panel_->status_colour(), style::theme().success);
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
    EXPECT_EQ(status_colour(), style::theme().failure);
    EXPECT_EQ(panel_->status_colour(), style::theme().failure);
    EXPECT_EQ(panel_->progress_bar()->value(), 150);  // (int)(0.15 × 1000): left where it was
    ASSERT_EQ(mock_.calls.size(), 3U);
    EXPECT_TRUE(is_state_call(mock_.calls[2], TBPF_ERROR));
    EXPECT_TRUE(error_reset_timer()->isActive());

    fire_error_reset_timer();
    ASSERT_EQ(mock_.calls.size(), 4U);
    EXPECT_TRUE(is_state_call(mock_.calls[3], TBPF_NOPROGRESS));
}

// ---------------------------------------------------------------------------
// The idle bar is completely still (D-34)
// ---------------------------------------------------------------------------

/// Counts every progress-bar element a widget asks its style to draw. Styles animate
/// what they draw (the "windowsvista" glow over a part-filled bar, Fusion's moving
/// stripes over a busy indicator); the bar of D-34 paints itself and asks for none.
class ProgressElementRecorder final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override
    {
        if (element == CE_ProgressBar || element == CE_ProgressBarGroove || element == CE_ProgressBarContents ||
            element == CE_ProgressBarLabel) {
            ++requests;
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override
    {
        if (element == PE_IndicatorProgressChunk) {
            ++requests;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    mutable int requests = 0;
};

/// Gives the bar back the application's style when a test ends, also after a failed ASSERT.
class StyleOverride {
public:
    StyleOverride(QWidget& widget, QStyle& style) : widget_(widget) { widget_.setStyle(&style); }
    ~StyleOverride() { widget_.setStyle(nullptr); }
    StyleOverride(const StyleOverride&) = delete;
    StyleOverride& operator=(const StyleOverride&) = delete;

private:
    QWidget& widget_;
};

/// Counts the paints and the style's animation frames that reach a widget.
class RepaintCounter final : public QObject {
public:
    explicit RepaintCounter(QWidget* target) { target->installEventFilter(this); }
    int paints = 0;
    int animation_frames = 0;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Paint) {
            ++paints;
        } else if (event->type() == QEvent::StyleAnimationUpdate) {
            ++animation_frames;
        }
        return QObject::eventFilter(watched, event);
    }
};

/// A reference clock in event-loop terms: a precise 16 ms timer (an animation frame). A
/// window of kStillTicks of its ticks gives anything that animates as many chances to
/// repaint, however slow the machine is: a busy runner only makes the window longer.
class ReferenceTicks {
public:
    static constexpr int kStillTicks = 25;

    ReferenceTicks()
    {
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setInterval(16ms);
        QObject::connect(&timer_, &QTimer::timeout, [this] { ++ticks_; });
        timer_.start();
    }
    /// Lets kStillTicks ticks pass.
    [[nodiscard]] bool wait_window()
    {
        const int end = ticks_ + kStillTicks;
        return pump_until([this, end] { return ticks_ >= end; });
    }

private:
    QTimer timer_;
    int ticks_ = 0;
};

/// The bar repainted now, with every update asked for before delivered.
[[nodiscard]] QImage settled_paint(QWidget& bar, const RepaintCounter& counter)
{
    const int painted = counter.paints;
    bar.update();
    EXPECT_TRUE(pump_until([&] { return counter.paints > painted; })) << "the bar was never painted";
    return bar.grab().toImage();
}

/// Whether `bar` has a running timer or animation of its own.
[[nodiscard]] bool has_running_animation(const QWidget& bar)
{
    for (const QTimer* timer : bar.findChildren<QTimer*>()) {
        if (timer->isActive()) {
            return true;
        }
    }
    for (const QAbstractAnimation* animation : bar.findChildren<QAbstractAnimation*>()) {
        if (animation->state() == QAbstractAnimation::Running) {
            return true;
        }
    }
    return false;
}

TEST_F(BusyPanelTest, An_idle_bar_is_completely_still_D_34)
{
    // D-34 (owner's requirement): when nothing is being processed the bar does not move.
    // While busy the bar is repainted (the marquee advances with the 40 ms timer: the
    // detector sees paints on this platform); once idle — full after a success, part-filled
    // after a failure, empty after an early end — nothing repaints it, no timer or
    // animation of its own runs, the style draws no part of it, and two paints a window
    // apart are pixel-identical.
    QProgressBar* bar = panel_->progress_bar();
    ProgressElementRecorder recorder{QStyleFactory::create(QStringLiteral("Fusion"))};  // takes ownership
    const StyleOverride style{*bar, recorder};
    panel_->resize(800, panel_->height());
    panel_->show();
    panel_->layout()->activate();
    RepaintCounter counter{bar};
    ReferenceTicks reference;

    struct Case {
        const char* name;
        bool report;  ///< a report moves the bar to 150 of 1001 first
        bool success;
        int expected_value;
    };
    for (const Case& c : {Case{"failed end", true, false, 150}, Case{"successful end", true, true, 1000},
                          Case{"early failed end", false, false, 0}}) {
        SCOPED_TRACE(c.name);
        panel_->begin("Outbound: starting…");
        ASSERT_EQ(bar->maximum(), 0) << "busy: a marquee";
        counter.paints = 0;
        ASSERT_TRUE(pump_until([&counter] { return counter.paints >= 3; })) << "the busy marquee is repainted";
        if (c.report) {
            panel_->report(progress("Fetching fares", "x", 4, 8));  // shown 0.15 → 150
            ASSERT_EQ(bar->value(), 150);
        }

        panel_->end(c.success ? "Done." : "Cancelled.", c.success);
        ASSERT_EQ(bar->value(), c.expected_value);
        ASSERT_EQ(bar->maximum(), BusyPanel::kBarRangeMax) << "End shows the continuous bar";
        const QImage first = settled_paint(*bar, counter);

        counter.paints = 0;
        counter.animation_frames = 0;
        ASSERT_TRUE(reference.wait_window());
        EXPECT_EQ(counter.paints, 0) << "something repaints the idle bar";
        EXPECT_EQ(counter.animation_frames, 0);
        EXPECT_FALSE(panel_->animation_timer()->isActive());
        EXPECT_FALSE(has_running_animation(*bar));
        EXPECT_EQ(bar->grab().toImage(), first) << "two idle paints differ: the idle bar moves";
    }
    EXPECT_EQ(recorder.requests, 0) << "the bar asked its style to draw it (styles animate what they draw)";
}

TEST(PanelProgressBarTest, A_marquee_moves_only_while_live)
{
    // The bar itself: only a live marquee changes from one paint to the next (the
    // comparison above sees such a change); an idle marquee is the empty track, still.
    PanelProgressBar bar;
    bar.resize(400, 18);
    bar.setRange(0, 0);
    ReferenceTicks reference;

    bar.set_live(true);
    const QImage live_first = bar.grab().toImage();
    ASSERT_TRUE(reference.wait_window());
    EXPECT_NE(bar.grab().toImage(), live_first) << "a live marquee advances";

    bar.set_live(false);
    const QImage idle_first = bar.grab().toImage();
    EXPECT_FALSE(has_running_animation(bar));
    ASSERT_TRUE(reference.wait_window());
    EXPECT_EQ(bar.grab().toImage(), idle_first);
    const QColor track = style::theme().track;
    for (const int x : {4, 200, 395}) {
        EXPECT_EQ(idle_first.pixelColor(x, 9), track) << "an idle marquee is the empty track, x " << x;
    }
}

/// The colour of the bar's middle line at `x`.
[[nodiscard]] QColor bar_pixel(const QImage& image, int x)
{
    return image.pixelColor(x, image.height() / 2);
}

/// The fill's width on the middle line, from the left (or the right) edge: up to the last
/// pixel of exactly `fill` (the rounded ends are blended), 0 when there is none.
[[nodiscard]] int fill_width(const QImage& image, const QColor& fill, bool from_right = false)
{
    constexpr int kRoundedEnd = 4;
    int last = -1;
    for (int i = 0; i < image.width(); ++i) {
        const int x = from_right ? image.width() - 1 - i : i;
        if (bar_pixel(image, x) == fill) {
            last = i;
        } else if (i > kRoundedEnd && last < i - kRoundedEnd) {
            break;
        }
    }
    return last + 1;
}

TEST_F(BusyPanelTest, The_bar_shows_its_value_in_the_accent_then_the_outcome_colour)
{
    // The bar paints a 6 px rounded track and a fill of the value's width: the accent while
    // busy, the outcome colour softened halfway towards the track at rest (D-34, D-53).
    QProgressBar* bar = panel_->progress_bar();
    panel_->resize(800, panel_->height());
    panel_->show();
    panel_->layout()->activate();
    const style::Theme& theme = style::theme();
    const auto softened = [&theme](const QColor& outcome) {
        return QColor((outcome.red() + theme.track.red()) / 2, (outcome.green() + theme.track.green()) / 2,
                      (outcome.blue() + theme.track.blue()) / 2);
    };
    const auto expected_width = [bar](int value) { return bar->width() * value / BusyPanel::kBarRangeMax; };

    panel_->begin("Outbound: starting…");
    panel_->report(progress("Fetching fares", "x", 4, 8));  // 150 of 1001
    QImage image = bar->grab().toImage();
    EXPECT_NEAR(fill_width(image, theme.accent), expected_width(150), 3) << "busy: the accent";
    EXPECT_EQ(bar_pixel(image, bar->width() - 4), theme.track);
    EXPECT_NE(image.pixelColor(bar->width() / 2, 1), theme.track) << "a 6 px track, not the whole height";

    panel_->end("Cancelled.", false);
    image = bar->grab().toImage();
    EXPECT_NEAR(fill_width(image, softened(theme.failure)), expected_width(150), 3) << "failed: left where it was";
    EXPECT_EQ(bar_pixel(image, bar->width() - 4), theme.track);

    bar->setLayoutDirection(Qt::RightToLeft);
    image = bar->grab().toImage();
    EXPECT_NEAR(fill_width(image, softened(theme.failure), true), expected_width(150), 3) << "right to left";
    bar->setLayoutDirection(Qt::LeftToRight);

    panel_->begin("Return: starting…");
    panel_->end("Done.");
    image = bar->grab().toImage();
    EXPECT_NEAR(fill_width(image, softened(theme.success)), expected_width(1000), 3) << "done: full";
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
    EXPECT_EQ(status_colour(), style::theme().success);
}

TEST_F(BusyPanelTest, Beginning_again_restores_the_text_colour)
{
    panel_->begin("One");
    panel_->end("Failed.", false);
    panel_->begin("Two");
    EXPECT_EQ(status_colour(), style::theme().text);
}

TEST_F(BusyPanelTest, End_if_still_busy_is_the_unexpected_error_safety_net)
{
    panel_->end_if_still_busy();
    EXPECT_EQ(status(), "Ready.");

    panel_->begin("Return: starting…");
    panel_->end_if_still_busy();
    EXPECT_FALSE(panel_->is_busy());
    EXPECT_EQ(status(), "Stopped by an unexpected error — details are in %LOCALAPPDATA%\\WizzairHelper\\errors.log.");
    EXPECT_EQ(status_colour(), style::theme().failure);

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

TEST_F(BusyPanelTest, Metrics_follow_01_10_1_with_the_status_bar_padding)
{
    // The rows of 01 §10.1, measured from the fonts; the padding is the look's 14 px at
    // the sides (D-53, 01 had 8) so the panel lines up with the cards above it.
    const BusyPanelMetrics& metrics = panel_->metrics();
    EXPECT_EQ(metrics.padding, QMargins(14, 5, 14, 5));
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
    EXPECT_EQ(panel_->wait_label()->font().weight(), QFont::DemiBold);
    EXPECT_DOUBLE_EQ(panel_->elapsed_label()->font().pointSizeF(), 10.0);
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
    // 01 §10.1's grid and cell margins; the side padding (14) and the bar's right margin
    // (14) are the look's (D-53).
    const BusyPanelMetrics& m = panel_->metrics();
    const int side = m.padding.left();
    ASSERT_EQ(side, 14);

    const QRect spinner = in_panel(panel_->spinner());
    EXPECT_EQ(spinner, QRect(side, 5, 36, m.bar_row + m.status_row));
    EXPECT_EQ(panel_->spinner()->contentsRect().width(), 30);  // Margin right 6

    const QRect bar = in_panel(panel_->progress_bar());
    const QRect status = in_panel(panel_->status_label());
    EXPECT_EQ(bar.left(), side + 36);
    EXPECT_EQ(bar.top(), 5 + 2);
    EXPECT_EQ(bar.height(), m.bar_row - 4);
    EXPECT_EQ(status.left(), side + 36);
    EXPECT_EQ(status.top(), 5 + m.bar_row);
    EXPECT_EQ(status.height(), m.status_row);
    EXPECT_EQ(bar.width(), status.width() - 14);  // the bar keeps 14 px to the elapsed column

    const QRect cancel = in_panel(panel_->cancel_button());
    EXPECT_EQ(cancel.right() + 1, kWidth - side - 3);
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
