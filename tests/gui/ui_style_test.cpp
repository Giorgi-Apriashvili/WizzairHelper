// Fonts and colours of 01 §4.3-§4.5 and the application look.

#include "ui_style.hpp"

#include <QApplication>
#include <QStyle>
#include <QStyleFactory>

#include <gtest/gtest.h>

#include <windows.h>

namespace {

namespace style = wizz::app::style;

TEST(UiStyle, Form_font_magenta_and_GrayText)
{
    EXPECT_EQ(style::form_font().family(), QStringLiteral("Segoe UI"));
    EXPECT_EQ(style::form_font().pointSizeF(), 9.0);
    EXPECT_FALSE(style::form_font().bold());
    EXPECT_TRUE(style::bold_form_font().bold());
    EXPECT_EQ(style::bold_form_font().pointSizeF(), 9.0);
    EXPECT_EQ(style::wizz_magenta(), QColor(197, 0, 110));
    const DWORD gray = ::GetSysColor(COLOR_GRAYTEXT);
    EXPECT_EQ(style::gray_text(), QColor(GetRValue(gray), GetGValue(gray), GetBValue(gray)));
    EXPECT_EQ(style::search_button_disabled_text(), QColor(232, 153, 197));
}

TEST(UiStyle, Application_look_selects_the_native_style_and_the_form_font)
{
    const QString old_style = QApplication::style()->name();
    const QFont old_font = QApplication::font();
    const bool native_available = QStyleFactory::keys().contains(QStringLiteral("windowsvista"), Qt::CaseInsensitive);

    EXPECT_EQ(style::apply_application_look(), native_available);
    EXPECT_EQ(QApplication::font().family(), QStringLiteral("Segoe UI"));
    EXPECT_EQ(QApplication::font().pointSizeF(), 9.0);
    if (native_available) {
        EXPECT_EQ(QApplication::style()->name(), QStringLiteral("windowsvista"));
    }

    QApplication::setStyle(old_style);  // other tests keep the default look
    QApplication::setFont(old_font);
}

} // namespace
