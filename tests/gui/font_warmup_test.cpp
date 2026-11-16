// FontWarmup: the start-up worker that resolves the wait note's "⏳" (Windows font
// fallback, 0.2-0.4 s the first time) before any busy period can show it (05 §5,
// R-03: a 400 ms timer gap on the first retry notice without it), and draws it so the
// glyph data is read too (D-33: a 149 ms freeze on a cold runner after a measure-only
// warm-up).

#include "busy_panel.hpp"
#include "elided_label.hpp"
#include "font_warmup.hpp"
#include "ui_style.hpp"

#include "wizz/core/clocks.hpp"

#include <QFontMetrics>
#include <QImage>
#include <QLabel>

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using wizz::app::FontWarmup;

TEST(FontWarmup, The_sample_holds_the_wait_notes_and_the_fonts_are_the_ones_the_panel_draws)
{
    const QString sample = FontWarmup::default_sample();
    EXPECT_TRUE(sample.contains(QChar{0x23F3})) << "the hourglass of the retry countdown";
    EXPECT_TRUE(sample.contains(QStringLiteral("retrying in")));
    EXPECT_TRUE(sample.contains(QStringLiteral("waiting for server")));
    const std::vector<QFont> fonts = FontWarmup::default_fonts();
    const auto warmed = [&fonts](const QFont& font) { return std::ranges::find(fonts, font) != fonts.end(); };

    // Exactly the fonts the busy panel lays its texts out in (each is a font engine of its
    // own, with its own fallback for the "⏳"), so no busy period meets the cold path.
    wizz::FixedClock clock;
    const wizz::app::BusyPanel panel{clock, wizz::Locale::en_us()};
    ASSERT_FALSE(fonts.empty());
    EXPECT_EQ(fonts[0], wizz::app::style::form_font());
    EXPECT_EQ(panel.status_label()->font(), fonts[0]) << "the status line";
    EXPECT_TRUE(warmed(panel.wait_label()->font())) << "the wait note, where the hourglass appears";
    EXPECT_TRUE(warmed(panel.elapsed_label()->font())) << "the elapsed time";
    // And the other texts of the look.
    for (const QFont& font : {wizz::app::style::bold_form_font(), wizz::app::style::data_font(),
                              wizz::app::style::mono_font(9.5), wizz::app::style::hint_font(),
                              wizz::app::style::caption_font()}) {
        EXPECT_TRUE(warmed(font)) << font.toString().toStdString();
    }
}

TEST(FontWarmup, Runs_on_a_worker_and_waits_once)
{
    FontWarmup warmup;
    warmup.wait();
    warmup.wait();  // later calls return at once
    // The UI thread measures the same text afterwards without trouble.
    EXPECT_GT(QFontMetrics{wizz::app::style::bold_form_font()}.horizontalAdvance(FontWarmup::default_sample()), 0);
}

/// Pixels with any ink in `image`.
[[nodiscard]] int inked_pixels(const QImage& image)
{
    int inked = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            inked += qAlpha(image.pixel(x, y)) > 0 ? 1 : 0;
        }
    }
    return inked;
}

TEST(FontWarmup, Warming_a_font_draws_the_text_not_only_measures_it)
{
    // Measuring reads the metrics only; the first drawing would still read the outlines and
    // colour layers of the fallback font on the UI thread (D-33).
    const QImage hourglass = FontWarmup::warm(wizz::app::style::bold_form_font(), QString{QChar{0x23F3}});
    ASSERT_FALSE(hourglass.isNull());
    EXPECT_GT(inked_pixels(hourglass), 20) << "the hourglass was drawn";
    const QImage sample = FontWarmup::warm(wizz::app::style::form_font(), FontWarmup::default_sample());
    EXPECT_GE(sample.width(), QFontMetrics{wizz::app::style::form_font()}.horizontalAdvance(FontWarmup::default_sample()))
        << "the whole sample fits the image";
    EXPECT_GT(inked_pixels(sample), inked_pixels(hourglass));
}

TEST(FontWarmup, Warms_every_font_in_order_on_a_worker_thread)
{
    std::mutex mutex;
    std::vector<QFont> seen;
    std::vector<std::thread::id> threads;
    const std::vector<QFont> fonts = FontWarmup::default_fonts();
    FontWarmup warmup{fonts, QStringLiteral("x"), [&](const QFont& font, const QString&) {
                          const std::scoped_lock lock{mutex};
                          seen.push_back(font);
                          threads.push_back(std::this_thread::get_id());
                      }};
    warmup.wait();
    EXPECT_EQ(seen, fonts);
    EXPECT_TRUE(std::ranges::none_of(threads, [](std::thread::id id) { return id == std::this_thread::get_id(); }))
        << "the UI thread does not pay for it";
}

TEST(FontWarmup, The_sample_holds_the_letters_and_digits_of_status_lines)
{
    const QString sample = FontWarmup::default_sample();
    for (const QChar c : QStringLiteral("AZaz09—…→")) {
        EXPECT_TRUE(sample.contains(c)) << QString{c}.toStdString();
    }
}

TEST(FontWarmup, The_destructor_waits)
{
    {
        const FontWarmup warmup{{QFont{}}, QStringLiteral("x")};
    }
    SUCCEED();
}

} // namespace
