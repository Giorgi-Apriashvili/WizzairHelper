// SearchButton: 01 §4.5 StyleSearchButton.

#include "search_button.hpp"

#include "input_test_support.hpp"
#include "ui_style.hpp"

#include <QFontMetrics>
#include <QImage>

#include <gtest/gtest.h>

namespace {

using wizz::app::SearchButton;
using namespace wizz::test::gui;

TEST(SearchButton, Uses_the_bold_form_font_magenta_background_and_white_text)
{
    SearchButton button(QStringLiteral("Search outbound"));
    EXPECT_EQ(button.text(), QStringLiteral("Search outbound"));
    EXPECT_EQ(button.font().family(), QStringLiteral("Segoe UI"));
    EXPECT_EQ(button.font().pointSizeF(), 9.0);
    EXPECT_TRUE(button.font().bold());
    EXPECT_TRUE(button.isFlat());
    EXPECT_FALSE(button.autoDefault());
    EXPECT_EQ(button.background_color(), QColor(197, 0, 110));
    EXPECT_EQ(button.background_color().name(), QStringLiteral("#c5006e"));
    EXPECT_EQ(button.text_color(), QColor(Qt::white));

    button.setEnabled(false);  // disabled: same magenta, lighter text tint (01 §4.5 open question)
    EXPECT_EQ(button.background_color(), QColor(197, 0, 110));
    EXPECT_EQ(button.text_color(), wizz::app::style::search_button_disabled_text());
    EXPECT_NE(button.text_color(), QColor(Qt::white));
}

TEST(SearchButton, Auto_size_is_text_plus_padding_and_never_below_75_by_23)
{
    SearchButton button(QStringLiteral("Search return"));
    const QFontMetrics metrics(button.font());
    const QSize text = metrics.size(Qt::TextShowMnemonic, button.text());
    const QSize hint = button.sizeHint();
    EXPECT_EQ(hint.width(), std::max(75, text.width() + 8 + 8 + 4));
    EXPECT_EQ(hint.height(), std::max(23, text.height() + 2 + 2 + 4));
    EXPECT_GE(hint.width(), 75);
    EXPECT_GE(hint.height(), 23);
    EXPECT_EQ(SearchButton::kPadding, QMargins(8, 2, 8, 2));

    SearchButton tiny(QStringLiteral("x"));
    EXPECT_EQ(tiny.sizeHint().width(), 75);
}

TEST(SearchButton, Paints_a_flat_magenta_face_without_border)
{
    SearchButton button(QStringLiteral("Search outbound"));
    button.resize(button.sizeHint());
    const QImage image = button.grab().toImage();
    ASSERT_FALSE(image.isNull());
    const QColor magenta(197, 0, 110);
    // Corners and edges are background (no border, no visual-style frame).
    EXPECT_EQ(image.pixelColor(0, 0), magenta);
    EXPECT_EQ(image.pixelColor(image.width() - 1, image.height() - 1), magenta);
    EXPECT_EQ(image.pixelColor(image.width() / 2, 0), magenta);
    save_render(button, QStringLiteral("search-button"));

    button.setEnabled(false);
    EXPECT_EQ(button.grab().toImage().pixelColor(0, 0), magenta);
    save_render(button, QStringLiteral("search-button-disabled"));
}

TEST(SearchButton, Enter_and_Space_click_the_focused_button)
{
    SearchButton button(QStringLiteral("Search return"));
    int clicks = 0;
    QObject::connect(&button, &QPushButton::clicked, [&clicks] { ++clicks; });
    EXPECT_TRUE(send_key(&button, Qt::Key_Return));
    EXPECT_EQ(clicks, 1);
    EXPECT_TRUE(send_key(&button, Qt::Key_Enter, {}, Qt::KeypadModifier));
    EXPECT_EQ(clicks, 2);
    send_key(&button, Qt::Key_Space, QStringLiteral(" "));
    EXPECT_EQ(clicks, 3);

    button.setEnabled(false);
    send_key(&button, Qt::Key_Return);
    EXPECT_EQ(clicks, 3);
}

} // namespace
