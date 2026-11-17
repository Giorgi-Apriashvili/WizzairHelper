// SearchButton: 01 §4.5 StyleSearchButton in the look of DECISIONS D-53 — the primary
// action: the accent (#C5006E) with white semibold text, as tall as the inputs beside it,
// rounded, lighter under the mouse, darker while pressed; disabled like every other
// disabled control.

#include "search_button.hpp"

#include "input_test_support.hpp"
#include "look_test_support.hpp"
#include "modern_style.hpp"
#include "ui_style.hpp"

#include <QComboBox>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QImage>
#include <QLineEdit>
#include <QtMath>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace {

namespace style = wizz::app::style;
using wizz::app::ModernStyle;
using wizz::app::SearchButton;
using namespace wizz::test::gui;

void expect_near(const QColor& actual, const QColor& expected, int tolerance, const char* what)
{
    EXPECT_NEAR(actual.red(), expected.red(), tolerance) << what;
    EXPECT_NEAR(actual.green(), expected.green(), tolerance) << what;
    EXPECT_NEAR(actual.blue(), expected.blue(), tolerance) << what;
}

TEST(SearchButton, Uses_the_semibold_UI_font_the_accent_and_white_text)
{
    SearchButton button(QStringLiteral("Search outbound"));
    const style::Theme& theme = style::theme();
    EXPECT_EQ(button.text(), QStringLiteral("Search outbound"));
    EXPECT_EQ(button.font(), style::bold_form_font());
    EXPECT_EQ(button.font().families().front(), style::windows10_fonts() ? QStringLiteral("Segoe UI Semibold")
                                                                         : QStringLiteral("Segoe UI Variable Text Semibold"));
    EXPECT_DOUBLE_EQ(button.font().pointSizeF(), 10.0);
    EXPECT_TRUE(button.isFlat());
    EXPECT_FALSE(button.autoDefault());
    EXPECT_EQ(button.background_color(), theme.accent);
    EXPECT_EQ(theme.accent.name(), QStringLiteral("#c5006e")) << "the brand magenta, light and dark";
    EXPECT_EQ(style::dark_theme().accent, theme.accent);
    EXPECT_EQ(button.text_color(), QColor(Qt::white));

    button.setAttribute(Qt::WA_UnderMouse);  // hovered: lighter (light) or brighter (dark)
    EXPECT_EQ(button.background_color(), theme.accent_hover);
    button.setDown(true);
    EXPECT_EQ(button.background_color(), theme.accent_pressed);
    button.setDown(false);
    button.setAttribute(Qt::WA_UnderMouse, false);

    button.setEnabled(false);  // disabled: the disabled input fill and text, like every disabled control
    EXPECT_EQ(button.background_color(), theme.input_disabled);
    EXPECT_EQ(button.text_color(), theme.text_disabled);
}

TEST(SearchButton, A_disabled_button_reads_as_disabled_in_both_schemes)
{
    // A search disables both buttons: in neither scheme may they look like an enabled (or a
    // pressed) accent button, and their label is clearly fainter than the enabled one.
    const wizz::test::look::LookGuard guard;
    for (const char* scheme : {"light", "dark"}) {
        qputenv("WIZZ_COLOR_SCHEME", scheme);
        style::refresh_appearance();
        const style::Theme& theme = style::theme();
        SearchButton button(QStringLiteral("Search outbound"));
        const double enabled = style::contrast_ratio(button.text_color(), button.background_color());
        button.setEnabled(false);
        const double disabled = style::contrast_ratio(button.text_color(), button.background_color());
        EXPECT_GE(enabled, 4.5) << scheme;
        EXPECT_LT(disabled, 3.0) << scheme << ": a disabled label is faint";
        EXPECT_GT(enabled - disabled, 2.5) << scheme;
        for (const QColor& accent : {theme.accent, theme.accent_hover, theme.accent_pressed}) {
            EXPECT_NE(button.background_color(), accent) << scheme;
        }
        EXPECT_LT(style::contrast_ratio(button.background_color(), theme.surface), 1.2)
            << scheme << ": the disabled face is close to the card, not an accent colour";
        const QColor face = button.background_color();
        EXPECT_LE(std::max({face.red(), face.green(), face.blue()}) - std::min({face.red(), face.green(), face.blue()}), 8)
            << scheme << ": a neutral grey, not magenta";
    }
}

TEST(SearchButton, As_tall_as_the_inputs_and_as_wide_as_icon_text_and_padding)
{
    // ModernStyle::control_height is the height of every input and push button; the search
    // buttons share it so a row of fields and its button line up (D-53).
    // The UI font's line rounded up (ModernStyle::line_height, D-58) and 10 px.
    const int control = ModernStyle::control_height(QFontMetrics(style::form_font()));
    EXPECT_EQ(control, std::max(26, qCeil(QFontMetricsF(style::form_font()).height()) + 10));
    QLineEdit edit;
    QComboBox combo;
    combo.addItem(QStringLiteral("Regular fares"));
    EXPECT_EQ(edit.sizeHint().height(), control);
    EXPECT_EQ(combo.sizeHint().height(), control);

    SearchButton button(QStringLiteral("Search return"));
    const QFontMetrics metrics(button.font());
    const QSize text = metrics.size(Qt::TextShowMnemonic, button.text());
    EXPECT_EQ(SearchButton::kPadding, QMargins(16, 2, 16, 2));
    EXPECT_EQ(button.sizeHint().width(), std::max(75, text.width() + 16 + 16));
    EXPECT_EQ(button.sizeHint().height(), std::max(control, text.height() + 2 + 2));
    EXPECT_EQ(button.sizeHint().height(), control);
    EXPECT_EQ(button.minimumSizeHint(), button.sizeHint()) << "never squeezed";

    const int plain = button.sizeHint().width();
    button.set_glyph(style::glyph::kSearch);
    const int glyph = static_cast<int>(std::ceil(style::icon_size(style::glyph::kSearch, 9.5).width()));
    EXPECT_EQ(button.sizeHint().width(), plain + glyph + 8) << "the icon and an 8 px gap before the text";

    SearchButton tiny(QStringLiteral("x"));
    EXPECT_EQ(tiny.sizeHint().width(), 75);  // never below 75 x 23
    EXPECT_GE(tiny.sizeHint().height(), 23);
}

TEST(SearchButton, Paints_a_rounded_accent_face_without_border)
{
    SearchButton button(QStringLiteral("Search outbound"));
    button.resize(button.sizeHint());
    const QImage image = button.grab().toImage();
    ASSERT_FALSE(image.isNull());
    const style::Theme& theme = style::theme();
    const int middle = image.height() / 2;
    // The face reaches the edges (no border, no frame) between the rounded corners.
    EXPECT_EQ(image.pixelColor(2, middle), theme.accent);
    EXPECT_EQ(image.pixelColor(image.width() - 3, middle), theme.accent);
    EXPECT_EQ(image.pixelColor(image.width() / 2, 1), theme.accent);
    EXPECT_EQ(image.pixelColor(image.width() / 2, image.height() - 2), theme.accent);
    // Rounded corners (radius 5): the pixel one in from each corner lies outside the arc, which
    // an antialiased square face would cover.
    EXPECT_NE(image.pixelColor(1, 1), theme.accent) << "rounded corners";
    EXPECT_NE(image.pixelColor(image.width() - 2, image.height() - 2), theme.accent) << "rounded corners";
    EXPECT_EQ(image.pixelColor(5, 1), theme.accent) << "the straight edge starts after the radius";
    save_render(button, QStringLiteral("search-button"));

    // Disabled: the disabled input fill inside an input-border outline.
    button.setEnabled(false);
    const QImage disabled = button.grab().toImage();
    expect_near(disabled.pixelColor(2, middle), theme.input_disabled, 2, "disabled face");
    expect_near(disabled.pixelColor(0, middle), theme.input_border, 2, "disabled outline");
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
