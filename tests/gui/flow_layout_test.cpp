// FlowLayout: the WinForms FlowLayoutPanel semantics of the input rows (01 §4.3).

#include "flow_layout.hpp"

#include <QWidget>

#include <gtest/gtest.h>

#include <memory>

namespace {

using wizz::app::FlowLayout;

[[nodiscard]] QWidget* fixed_widget(QWidget* parent, int width, int height)
{
    auto* widget = new QWidget(parent);
    widget->setFixedSize(width, height);
    return widget;
}

void apply(FlowLayout& layout, int width)
{
    layout.setGeometry(QRect(0, 0, width, layout.heightForWidth(width)));
}

TEST(FlowLayout, Places_items_left_to_right_with_their_own_margins_and_no_spacing)
{
    QWidget host;
    auto* layout = new FlowLayout(&host);
    QWidget* a = fixed_widget(&host, 50, 20);
    QWidget* b = fixed_widget(&host, 30, 10);
    QWidget* c = fixed_widget(&host, 40, 20);
    layout->add_widget(a);                         // WinForms default margin 3
    layout->add_widget(b, QMargins(3, 7, 3, 0));   // Lbl(...)
    layout->add_gap(24);                           // Gap(24)
    layout->add_widget(c, QMargins());
    apply(*layout, 500);

    EXPECT_EQ(layout->count(), 4);
    EXPECT_EQ(layout->item_margins(0), QMargins(3, 3, 3, 3));
    EXPECT_EQ(layout->item_margins(1), QMargins(3, 7, 3, 0));
    EXPECT_EQ(layout->item_margins(2), QMargins());
    EXPECT_EQ(layout->item_margins(9), QMargins());
    EXPECT_EQ(a->geometry(), QRect(3, 3, 50, 20));
    EXPECT_EQ(b->geometry(), QRect(56 + 3, 7, 30, 10));  // top-aligned at line top + margin.top
    EXPECT_EQ(c->geometry(), QRect(56 + 36 + 24, 0, 40, 20));
    EXPECT_EQ(layout->line_count(500), 1);
    EXPECT_EQ(layout->heightForWidth(500), 26);  // tallest outer height: 3 + 20 + 3
    EXPECT_EQ(layout->sizeHint(), QSize(56 + 36 + 24 + 40, 26));
    EXPECT_TRUE(layout->hasHeightForWidth());
    EXPECT_EQ(layout->expandingDirections(), Qt::Orientations{});
}

TEST(FlowLayout, Wraps_items_that_do_not_fit_onto_new_lines)
{
    QWidget host;
    auto* layout = new FlowLayout(&host);
    QWidget* a = fixed_widget(&host, 100, 20);
    QWidget* b = fixed_widget(&host, 100, 40);
    QWidget* c = fixed_widget(&host, 100, 20);
    layout->add_widget(a);
    layout->add_widget(b);
    layout->add_widget(c);

    apply(*layout, 318);  // exactly three outer widths of 106
    EXPECT_EQ(layout->line_count(318), 1);
    EXPECT_EQ(c->geometry(), QRect(215, 3, 100, 20));

    apply(*layout, 317);  // one pixel less: c wraps
    EXPECT_EQ(layout->line_count(317), 2);
    EXPECT_EQ(a->geometry(), QRect(3, 3, 100, 20));
    EXPECT_EQ(b->geometry(), QRect(109, 3, 100, 40));
    EXPECT_EQ(c->geometry(), QRect(3, 46 + 3, 100, 20));  // line 1 is as tall as b (3 + 40 + 3)
    EXPECT_EQ(layout->heightForWidth(317), 46 + 26);

    apply(*layout, 50);  // narrower than any item: one item per line, never an empty line
    EXPECT_EQ(layout->line_count(50), 3);
    EXPECT_EQ(b->geometry().topLeft(), QPoint(3, 26 + 3));
    EXPECT_EQ(c->geometry().topLeft(), QPoint(3, 26 + 46 + 3));
}

TEST(FlowLayout, Contents_margins_are_part_of_the_height_and_the_usable_width)
{
    QWidget host;
    auto* layout = new FlowLayout(&host);
    layout->setContentsMargins(0, 1, 0, 1);  // the rows' Margin (0, 1, 0, 1)
    QWidget* a = fixed_widget(&host, 100, 20);
    layout->add_widget(a);
    apply(*layout, 400);
    EXPECT_EQ(a->geometry(), QRect(3, 4, 100, 20));
    EXPECT_EQ(layout->heightForWidth(400), 28);
    EXPECT_EQ(layout->minimumSize(), QSize(0, 28));
}

TEST(FlowLayout, Hidden_widgets_take_no_space)
{
    QWidget host;
    auto* layout = new FlowLayout(&host);
    QWidget* a = fixed_widget(&host, 100, 20);
    QWidget* b = fixed_widget(&host, 100, 20);
    QWidget* c = fixed_widget(&host, 100, 20);
    layout->add_widget(a);
    layout->add_widget(b);
    layout->add_widget(c);
    b->hide();
    apply(*layout, 220);
    EXPECT_EQ(layout->line_count(220), 1);
    EXPECT_EQ(c->geometry().topLeft(), QPoint(109, 3));
    EXPECT_EQ(layout->sizeHint().width(), 212);
}

TEST(FlowLayout, Minimum_size_has_no_width_and_takeAt_releases_ownership)
{
    QWidget host;
    auto* layout = new FlowLayout(&host);
    layout->add_widget(fixed_widget(&host, 100, 20));
    layout->add_widget(fixed_widget(&host, 210, 22));
    layout->add_gap(24);
    EXPECT_EQ(layout->minimumSize(), QSize(0, 28));  // items wider than the row overflow their line

    std::unique_ptr<QLayoutItem> taken(layout->takeAt(2));
    ASSERT_NE(taken, nullptr);
    EXPECT_NE(taken->spacerItem(), nullptr);
    EXPECT_EQ(layout->count(), 2);
    EXPECT_EQ(layout->takeAt(5), nullptr);
    EXPECT_EQ(layout->itemAt(-1), nullptr);
}

} // namespace
