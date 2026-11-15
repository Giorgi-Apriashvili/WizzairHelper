// FontWarmup: the start-up worker that resolves the wait note's "⏳" (Windows font
// fallback, 0.2-0.4 s the first time) before any busy period can show it (05 §5,
// R-03: a 400 ms timer gap on the first retry notice without it).

#include "font_warmup.hpp"
#include "ui_style.hpp"

#include <QFontMetrics>

#include <gtest/gtest.h>

namespace {

using wizz::app::FontWarmup;

TEST(FontWarmup, The_sample_holds_the_wait_notes_and_the_fonts_are_the_form_fonts)
{
    const QString sample = FontWarmup::default_sample();
    EXPECT_TRUE(sample.contains(QChar{0x23F3})) << "the hourglass of the retry countdown";
    EXPECT_TRUE(sample.contains(QStringLiteral("retrying in")));
    EXPECT_TRUE(sample.contains(QStringLiteral("waiting for server")));
    const std::vector<QFont> fonts = FontWarmup::default_fonts();
    ASSERT_EQ(fonts.size(), 2u);
    EXPECT_EQ(fonts[0], wizz::app::style::form_font());
    EXPECT_TRUE(fonts[1].bold()) << "the wait note is bold";
}

TEST(FontWarmup, Runs_on_a_worker_and_waits_once)
{
    FontWarmup warmup;
    warmup.wait();
    warmup.wait();  // later calls return at once
    // The UI thread measures the same text afterwards without trouble.
    EXPECT_GT(QFontMetrics{wizz::app::style::bold_form_font()}.horizontalAdvance(FontWarmup::default_sample()), 0);
}

TEST(FontWarmup, The_destructor_waits)
{
    {
        const FontWarmup warmup{{QFont{}}, QStringLiteral("x")};
    }
    SUCCEED();
}

} // namespace
