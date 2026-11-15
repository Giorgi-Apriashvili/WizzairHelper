#include "app_info.hpp"

#include <QWidget>

#include <gtest/gtest.h>

namespace {

TEST(AppInfo, Main_window_title_matches_01_4_1)
{
    EXPECT_EQ(wizz::app::main_window_title().toStdString(),
              "WizzairHelper — cheapest Wizz Air flights from nearby airports");
    EXPECT_EQ(wizz::app::product_name(), QStringLiteral("WizzairHelper"));
}

TEST(AppInfo, Widgets_can_be_created_headless)
{
    QWidget widget;
    widget.resize(100, 50);
    EXPECT_EQ(widget.width(), 100);
}

} // namespace
