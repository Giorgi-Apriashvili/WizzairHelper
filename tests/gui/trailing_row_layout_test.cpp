// TrailingRowLayout (DECISIONS D-53): a row of leading widgets with one trailing widget
// at its right end, which moves to a line of its own (still right-aligned) when the row
// is too narrow, so a card never clips its search button.

#include "trailing_row_layout.hpp"

#include <QWidget>

#include <gtest/gtest.h>

namespace {

using wizz::app::TrailingRowLayout;

/// A widget of a fixed size hint.
class Block final : public QWidget {
public:
    Block(QSize size, QWidget* parent) : QWidget(parent), size_(size) {}
    [[nodiscard]] QSize sizeHint() const override { return size_; }

private:
    QSize size_;
};

class TrailingRowLayoutTest : public ::testing::Test {
protected:
    TrailingRowLayoutTest()
    {
        layout_ = new TrailingRowLayout(&host_);
        first_ = new Block({100, 26}, &host_);
        second_ = new Block({60, 20}, &host_);
        trailing_ = new Block({120, 30}, &host_);
        layout_->add_widget(first_);
        layout_->add_widget(second_);
        layout_->set_trailing(trailing_);
        host_.setAttribute(Qt::WA_DontShowOnScreen);
        host_.show();
    }

    void place(int width)
    {
        host_.resize(width, layout_->heightForWidth(width));
        layout_->setGeometry(QRect(0, 0, width, layout_->heightForWidth(width)));
    }

    QWidget host_;
    TrailingRowLayout* layout_ = nullptr;
    Block* first_ = nullptr;
    Block* second_ = nullptr;
    Block* trailing_ = nullptr;
};

TEST_F(TrailingRowLayoutTest, One_line_when_the_row_fits)
{
    // Leading: 100 + 8 + 60 = 168; then 2 x 8 before the trailing 120: 304.
    EXPECT_EQ(TrailingRowLayout::kSpacing, 8);
    EXPECT_EQ(layout_->sizeHint(), QSize(304, 30));
    EXPECT_FALSE(layout_->wraps_at(304));
    EXPECT_EQ(layout_->heightForWidth(304), 30);
    place(400);
    EXPECT_EQ(first_->geometry(), QRect(0, 2, 100, 26)) << "vertically centred on the line";
    EXPECT_EQ(second_->geometry(), QRect(108, 5, 60, 20));
    EXPECT_EQ(trailing_->geometry(), QRect(280, 0, 120, 30)) << "at the right end";
}

TEST_F(TrailingRowLayoutTest, The_trailing_widget_moves_under_the_row_when_it_does_not_fit)
{
    EXPECT_TRUE(layout_->wraps_at(303));
    EXPECT_EQ(layout_->heightForWidth(303), 26 + 8 + 30) << "the leading line, the spacing, the trailing line";
    EXPECT_EQ(layout_->minimumSize(), QSize(168, 30)) << "the wider of the two lines";
    place(200);
    EXPECT_EQ(first_->geometry(), QRect(0, 0, 100, 26));
    EXPECT_EQ(second_->geometry(), QRect(108, 3, 60, 20));
    EXPECT_EQ(trailing_->geometry(), QRect(80, 34, 120, 30)) << "right-aligned on a line of its own";

    place(304);  // wide enough again
    EXPECT_EQ(trailing_->geometry(), QRect(184, 0, 120, 30));
    EXPECT_EQ(host_.height(), 30);
}

TEST_F(TrailingRowLayoutTest, Hidden_widgets_take_no_room_and_a_gap_adds_space)
{
    second_->hide();
    EXPECT_EQ(layout_->sizeHint().width(), 100 + 16 + 120);
    second_->show();
    auto* third = new Block({40, 20}, &host_);
    layout_->add_gap(24);
    layout_->add_widget(third);
    third->show();
    EXPECT_EQ(layout_->sizeHint().width(), 100 + 8 + 60 + 8 + 24 + 40 + 16 + 120);
    place(600);
    EXPECT_EQ(third->geometry().left(), 168 + 8 + 24);
    trailing_->hide();
    EXPECT_FALSE(layout_->wraps_at(10)) << "nothing to move";
    EXPECT_EQ(layout_->count(), 4);
}

} // namespace
