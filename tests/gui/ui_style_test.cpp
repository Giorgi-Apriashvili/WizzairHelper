// Fonts and colours of the look (DECISIONS D-53, replacing 01 §4.3-§4.5's Segoe UI 9 pt,
// the WinForms SystemColors and the native windowsvista style) and how the application
// installs it.

#include "look_test_support.hpp"
#include "modern_style.hpp"
#include "ui_style.hpp"

#include <QApplication>
#include <QStyle>
#include <QToolTip>

#include <gtest/gtest.h>

namespace {

namespace style = wizz::app::style;
using wizz::app::ModernStyle;
using wizz::test::look::LookGuard;

TEST(UiStyle, Fonts_follow_the_look_with_their_Windows_10_fallbacks)
{
    // UI: Segoe UI Variable Text 10 pt; Windows 10 picks the next family (QFont::setFamilies).
    const QFont form = style::form_font();
    EXPECT_EQ(form.families(), (QStringList{"Segoe UI Variable Text", "Segoe UI"}));
    EXPECT_DOUBLE_EQ(form.pointSizeF(), 10.0);
    EXPECT_FALSE(form.bold());
    // Semibold is a family of its own (weight 600 of the Text family would give its Bold).
    const QFont semibold = style::bold_form_font();
    EXPECT_EQ(semibold.families(), (QStringList{"Segoe UI Variable Text Semibold", "Segoe UI Semibold"}));
    EXPECT_DOUBLE_EQ(semibold.pointSizeF(), 10.0);
    EXPECT_EQ(style::semibold_font(8.0).families(), semibold.families());
    // Grid cells: 9 pt, denser than the UI.
    EXPECT_EQ(style::data_font().families(), form.families());
    EXPECT_DOUBLE_EQ(style::data_font().pointSizeF(), 9.0);
    // Card captions: 8 pt semibold, upper case, tracked; hints 9 pt.
    const QFont caption = style::caption_font();
    EXPECT_EQ(caption.families(), (QStringList{"Segoe UI Variable Small Semibold", "Segoe UI Semibold"}));
    EXPECT_DOUBLE_EQ(caption.pointSizeF(), 8.0);
    EXPECT_EQ(caption.capitalization(), QFont::AllUppercase);
    EXPECT_EQ(caption.letterSpacingType(), QFont::AbsoluteSpacing);
    EXPECT_NEAR(caption.letterSpacing(), 0.8, 0.01);  // stored in 1/64 px
    EXPECT_EQ(style::hint_font().families(), form.families());
    EXPECT_DOUBLE_EQ(style::hint_font().pointSizeF(), 9.0);
    // Monospace: Cascadia Mono (Windows 11, or Windows Terminal on Windows 10), Consolas.
    const QFont mono = style::mono_font(9.5);
    EXPECT_EQ(mono.families(), (QStringList{"Cascadia Mono", "Consolas", "Courier New"}));
    EXPECT_TRUE(mono.fixedPitch());
    EXPECT_EQ(mono.styleHint(), QFont::TypeWriter);
    EXPECT_EQ(mono.weight(), QFont::Normal);
    EXPECT_EQ(style::mono_font(10.0, true).weight(), QFont::DemiBold);
}

TEST(UiStyle, Magenta_muted_text_and_the_disabled_search_text)
{
    EXPECT_EQ(style::wizz_magenta(), QColor(197, 0, 110));
    EXPECT_EQ(style::wizz_magenta(), style::light_theme().accent);
    EXPECT_EQ(style::wizz_magenta(), style::dark_theme().accent);
    EXPECT_EQ(style::gray_text(), style::theme().text_muted) << "secondary text is the theme's muted text";
    EXPECT_EQ(style::search_button_disabled_text(), QColor(255, 255, 255, 178)) << "white at 70 %";
}

TEST(UiStyle, The_application_look_is_ModernStyle_with_the_theme_palette_and_the_UI_font)
{
    // What main() installs (D-53): Fusion under ModernStyle, the theme's palette (also for
    // tooltips) and the UI font, also over Qt's per-class fonts, which setStyle() fills with
    // Windows' Segoe UI 9 pt.
    const LookGuard guard;
    qputenv("WIZZ_COLOR_SCHEME", "light");
    EXPECT_TRUE(style::apply_application_look()) << "Fusion is available";
    EXPECT_TRUE(ModernStyle::is_active());
    const auto* modern = dynamic_cast<const ModernStyle*>(QApplication::style());
    ASSERT_NE(modern, nullptr);
    EXPECT_EQ(modern->baseStyle()->name().toLower(), QStringLiteral("fusion")) << "ModernStyle paints over Fusion";
    EXPECT_EQ(style::appearance(), style::Appearance::light);

    EXPECT_EQ(QApplication::font().families(), style::form_font().families());
    EXPECT_DOUBLE_EQ(QApplication::font().pointSizeF(), 10.0);
    for (const char* widget_class : {"QAbstractItemView", "QMenu", "QMessageBox", "QTipLabel", "QHeaderView"}) {
        EXPECT_EQ(QApplication::font(widget_class).families(), style::form_font().families()) << widget_class;
        EXPECT_DOUBLE_EQ(QApplication::font(widget_class).pointSizeF(), 10.0) << widget_class;
    }

    const QPalette expected = style::make_palette(style::light_theme());
    const QPalette palette = QApplication::palette();
    for (const QPalette::ColorRole role : {QPalette::Window, QPalette::WindowText, QPalette::Base, QPalette::Text,
                                           QPalette::Button, QPalette::ButtonText, QPalette::Highlight,
                                           QPalette::HighlightedText, QPalette::PlaceholderText, QPalette::Accent}) {
        EXPECT_EQ(palette.color(role), expected.color(role)) << role;
    }
    EXPECT_EQ(palette.color(QPalette::Disabled, QPalette::Text), style::light_theme().text_disabled);
    EXPECT_EQ(QToolTip::palette().color(QPalette::ToolTipBase), style::light_theme().surface);
    EXPECT_EQ(QToolTip::palette().color(QPalette::ToolTipText), style::light_theme().text);
}

TEST(UiStyle, The_palette_maps_every_token_to_its_role)
{
    for (const style::Theme& theme : {style::light_theme(), style::dark_theme()}) {
        SCOPED_TRACE(theme.dark ? "dark" : "light");
        const QPalette palette = style::make_palette(theme);
        EXPECT_EQ(palette.color(QPalette::Window), theme.window);
        EXPECT_EQ(palette.color(QPalette::WindowText), theme.text);
        EXPECT_EQ(palette.color(QPalette::Base), theme.input);
        EXPECT_EQ(palette.color(QPalette::AlternateBase), theme.surface_alt);
        EXPECT_EQ(palette.color(QPalette::Text), theme.text);
        EXPECT_EQ(palette.color(QPalette::PlaceholderText), theme.text_muted);
        EXPECT_EQ(palette.color(QPalette::Button), theme.button);
        EXPECT_EQ(palette.color(QPalette::ButtonText), theme.text);
        EXPECT_EQ(palette.color(QPalette::Link), theme.accent_text);
        EXPECT_EQ(palette.color(QPalette::Accent), theme.accent);
        EXPECT_EQ(palette.color(QPalette::ToolTipBase), theme.dark ? theme.surface_alt : theme.surface);
        EXPECT_EQ(palette.color(QPalette::Disabled, QPalette::WindowText), theme.text_disabled);
        EXPECT_EQ(palette.color(QPalette::Disabled, QPalette::Base), theme.input_disabled);
    }
}

} // namespace
