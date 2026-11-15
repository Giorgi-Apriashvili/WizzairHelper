// LogView (01 §12.1): line prefix with the culture time separator, CRLF text for
// summary.json, caret and scroll at the end, no size limit, and the box's look.

#include "diagnostics_test_support.hpp"
#include "log_view.hpp"
#include "system_colours.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/ui/summary_json.hpp"

#include <QFont>
#include <QPalette>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::FixedClock;
using wizz::Locale;
using wizz::app::LogView;

class LogViewTest : public ::testing::Test {
protected:
    // 2026-11-02T10:00:00Z with the local zone at UTC+2: local 12:00:00.
    FixedClock clock_{FixedClock::kDefaultUtcNow, std::chrono::hours{2}};
    LogView view_{clock_, Locale::en_us()};
};

TEST_F(LogViewTest, Lines_get_the_local_time_prefix_and_a_crlf)
{
    view_.append_line("WizzairHelper started.");
    clock_.advance(std::chrono::seconds{65});
    view_.append_line("  note: indented lines keep their spaces");

    EXPECT_EQ(view_.text(), "[12:00:00] WizzairHelper started.\r\n"
                            "[12:01:05]   note: indented lines keep their spaces\r\n");
    EXPECT_EQ(view_.toPlainText().toStdString(), "[12:00:00] WizzairHelper started.\n"
                                                 "[12:01:05]   note: indented lines keep their spaces\n");
    EXPECT_EQ(view_.document()->blockCount(), 3);  // two lines and the empty one after the last CRLF
}

TEST_F(LogViewTest, Text_splits_into_the_summary_json_log)
{
    view_.append_line("Route map: 196 airports (from the local cache).");
    view_.append_line("Outbound: Budapest (+250 km) → London");
    const std::vector<std::string> expected{"[12:00:00] Route map: 196 airports (from the local cache).",
                                            "[12:00:00] Outbound: Budapest (+250 km) → London"};
    EXPECT_EQ(wizz::ui::split_log(view_.text()), expected);
}

TEST(LogViewLocale, The_prefix_uses_the_culture_time_separator)
{
    FixedClock clock{FixedClock::kDefaultUtcNow, std::chrono::hours{-3}};
    LogView view(clock, Locale::create("fi-FI"));
    view.append_line("x");
    EXPECT_EQ(view.text(), "[07.00.00] x\r\n");
}

TEST_F(LogViewTest, Is_a_read_only_unwrapped_courier_box_on_the_window_colour)
{
    EXPECT_TRUE(view_.isReadOnly());
    EXPECT_FALSE(view_.isUndoRedoEnabled());
    EXPECT_EQ(view_.lineWrapMode(), QPlainTextEdit::NoWrap);
    EXPECT_EQ(view_.horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
    EXPECT_EQ(view_.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
    EXPECT_EQ(view_.font().family(), QStringLiteral("Courier New"));
    EXPECT_DOUBLE_EQ(view_.font().pointSizeF(), 9.0);
    EXPECT_FALSE(view_.font().bold());
    EXPECT_EQ(view_.viewport()->backgroundRole(), QPalette::Base);
    EXPECT_EQ(view_.palette().color(QPalette::Base), wizz::app::system_colour(wizz::app::SystemColour::window));
    EXPECT_EQ(view_.palette().color(QPalette::Text), wizz::app::system_colour(wizz::app::SystemColour::window_text));

    view_.resize(300, 120);
    view_.show();
    view_.append_line("abc");
    QCoreApplication::processEvents();
    const QImage image = view_.viewport()->grab().toImage();
    wizz::test::save_snapshot(image, QStringLiteral("log-view-viewport"));
    EXPECT_TRUE(view_.viewport()->autoFillBackground());  // also after the style's polish
    const QColor window = wizz::app::system_colour(wizz::app::SystemColour::window);  // SystemColors.Window, not grey
    EXPECT_EQ(image.pixelColor(image.width() - 2, image.height() - 2), window);
    EXPECT_EQ(image.pixelColor(image.width() / 2, image.height() - 3), window);
}

TEST_F(LogViewTest, Appending_moves_the_caret_to_the_end_and_scrolls_there)
{
    view_.resize(400, 150);
    view_.show();
    for (int i = 0; i < 200; ++i) {
        view_.append_line("line " + std::to_string(i) + std::string(120, '-'));
    }
    QCoreApplication::processEvents();

    EXPECT_TRUE(view_.textCursor().atEnd());
    const QScrollBar* vertical = view_.verticalScrollBar();
    EXPECT_GT(vertical->maximum(), 0);
    EXPECT_EQ(vertical->value(), vertical->maximum());
    EXPECT_EQ(view_.horizontalScrollBar()->value(), 0);  // the caret sits at the start of the new line
    wizz::test::save_snapshot(view_, QStringLiteral("log-view"));
}

TEST_F(LogViewTest, Has_no_size_limit)
{
    const std::string long_line(40'000, 'x');
    view_.append_line(long_line);
    view_.append_line("after");
    EXPECT_GT(view_.text().size(), 40'000U);
    EXPECT_EQ(view_.document()->findBlockByNumber(1).text().toStdString(), "[12:00:00] after");
    EXPECT_EQ(view_.document()->firstBlock().text().size(), 40'000 + 11);
}

} // namespace
