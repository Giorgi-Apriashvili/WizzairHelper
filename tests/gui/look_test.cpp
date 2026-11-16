// The application look (DECISIONS D-53..D-56): how the look is chosen (High Contrast,
// WIZZ_COLOR_SCHEME, the Windows app mode), that a change applies to a running window
// without a restart, the High Contrast fallback, the Windows 10 font fallbacks and the
// contrast of the colour tokens.

#include "app_test_support.hpp"
#include "busy_panel.hpp"
#include "date_picker.hpp"
#include "elided_label.hpp"
#include "flight_table_model.hpp"
#include "grid_row_delegate.hpp"
#include "grid_table_view.hpp"
#include "log_view.hpp"
#include "look_test_support.hpp"
#include "modern_style.hpp"
#include "section_card.hpp"
#include "ui_style.hpp"
#include "update_bar.hpp"

#include <QAccessibilityHints>
#include <QApplication>
#include <QCalendarWidget>
#include <QLabel>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyleOptionViewItem>
#include <QTextCharFormat>
#include <QToolTip>
#include <QImage>

#include <gtest/gtest.h>

#include <initializer_list>
#include <utility>

namespace {

namespace style = wizz::app::style;
namespace gui = wizz::test::gui;
using wizz::app::ModernStyle;
using wizz::test::app::WindowRig;
using wizz::test::look::LookGuard;

constexpr wizz::Date d(int year, unsigned month, unsigned day)
{
    return wizz::Date::from_ymd(year, month, day);
}

[[nodiscard]] QColor text_colour(const QWidget* widget)
{
    return widget->palette().color(QPalette::WindowText);
}

/// The background brush the outbound model gives its cheapest row.
[[nodiscard]] QColor cheapest_background(const WindowRig& rig)
{
    const QAbstractItemModel* model = const_cast<WindowRig&>(rig).outbound().table()->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, wizz::app::FlightTableModel::RowToneRole).toInt() ==
            static_cast<int>(wizz::ui::RowTone::cheapest)) {
            return model->data(index, Qt::BackgroundRole).value<QBrush>().color();
        }
    }
    return {};
}

void section11_search(WindowRig& rig)
{
    rig.input().set_from_radius_km(200);
    rig.input().set_to_radius_km(30);
    rig.input().apply_autotest_dates(wizz::ui::DateWindow{.start = d(2026, 11, 10), .end = d(2026, 11, 20)},
                                     wizz::ui::DateWindow{.start = d(2026, 11, 12), .end = d(2026, 11, 20)});
    rig.run(rig.controller().search_outbound());
}

/// Every place that keeps a token of `expected` in a palette, a brush or a format.
void expect_window_follows(WindowRig& rig, const style::Theme& expected)
{
    EXPECT_EQ(QApplication::palette().color(QPalette::Window), expected.window);
    EXPECT_EQ(QApplication::palette().color(QPalette::Highlight), expected.selection);
    EXPECT_EQ(QToolTip::palette().color(QPalette::ToolTipText), expected.text);
    EXPECT_EQ(rig.outbound().table()->palette().color(QPalette::Base), expected.surface);
    EXPECT_EQ(rig.trips().table()->palette().color(QPalette::Text), expected.text);
    EXPECT_EQ(rig.window().log_view()->palette().color(QPalette::Base), expected.surface);
    EXPECT_EQ(rig.busy().background_colour(), expected.surface);
    EXPECT_EQ(rig.busy().status_colour(), expected.success);  // the search ended well
    EXPECT_EQ(text_colour(rig.busy().status_label()), expected.success);
    EXPECT_EQ(text_colour(rig.busy().wait_label()), expected.warning);
    EXPECT_EQ(text_colour(rig.busy().elapsed_label()), expected.text_secondary);
    const auto* where = rig.input().findChild<wizz::app::SectionCard*>(QStringLiteral("whereCard"));
    ASSERT_NE(where, nullptr);
    EXPECT_EQ(text_colour(where->title_label()), expected.text_secondary);
    EXPECT_EQ(text_colour(where->hint_label()), expected.text_muted);
    EXPECT_EQ(text_colour(rig.input().resolved_label()), expected.text_muted);
    const QCalendarWidget* calendar = rig.input().out_from_picker()->calendarWidget();
    EXPECT_EQ(calendar->weekdayTextFormat(Qt::Saturday).foreground().color(), expected.accent_text);
    EXPECT_EQ(calendar->weekdayTextFormat(Qt::Sunday).foreground().color(), expected.accent_text);
    EXPECT_EQ(calendar->palette().color(QPalette::Disabled, QPalette::Text), expected.text_muted);
    EXPECT_EQ(cheapest_background(rig), expected.cheapest);
    // The grid delegate's kept selection brush (a selected row that is not a cheapest one).
    const wizz::app::GridTableView* table = rig.outbound().table();
    const auto* delegate = dynamic_cast<const wizz::app::GridRowDelegate*>(table->itemDelegate());
    ASSERT_NE(delegate, nullptr);
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const QModelIndex index = table->model()->index(row, 2);
        if (index.data(wizz::app::FlightTableModel::RowToneRole).toInt() !=
            static_cast<int>(wizz::ui::RowTone::cheapest)) {
            QStyleOptionViewItem option;
            option.initFrom(table->viewport());
            option.state.setFlag(QStyle::State_Selected, true);
            delegate->initStyleOption(&option, index);
            EXPECT_EQ(option.backgroundBrush.color(), expected.selection) << "the delegate's selection brush";
            EXPECT_EQ(option.palette.color(QPalette::Text), expected.selection_text);
            break;
        }
    }
    // The update bar's text, and the busy bar's resting fill (the search ended well: full, in
    // the success colour softened towards the track).
    EXPECT_EQ(text_colour(rig.window().update_bar()->message_label()), expected.text);
    const QImage bar = rig.busy().progress_bar()->grab().toImage();
    const QColor resting((expected.success.red() + expected.track.red()) / 2,
                         (expected.success.green() + expected.track.green()) / 2,
                         (expected.success.blue() + expected.track.blue()) / 2);
    EXPECT_EQ(bar.pixelColor(bar.width() / 2, bar.height() / 2), resting) << "the busy bar's fill";
}

TEST(Look, Override_values_parse)
{
    EXPECT_EQ(style::parse_color_scheme("dark"), Qt::ColorScheme::Dark);
    EXPECT_EQ(style::parse_color_scheme(" Light\n"), Qt::ColorScheme::Light);
    EXPECT_EQ(style::parse_color_scheme("DARK"), Qt::ColorScheme::Dark);
    EXPECT_EQ(style::parse_color_scheme(""), std::nullopt);
    EXPECT_EQ(style::parse_color_scheme("auto"), std::nullopt);
    EXPECT_EQ(style::parse_switch("1"), true);
    EXPECT_EQ(style::parse_switch(" On "), true);
    EXPECT_EQ(style::parse_switch("false"), false);
    EXPECT_EQ(style::parse_switch("0"), false);
    EXPECT_EQ(style::parse_switch("maybe"), std::nullopt);
}

TEST(Look, High_contrast_wins_then_the_override_then_Windows)
{
    using style::Appearance;
    using Scheme = Qt::ColorScheme;
    EXPECT_EQ(style::choose_appearance(true, Scheme::Dark, Scheme::Dark), Appearance::high_contrast);
    EXPECT_EQ(style::choose_appearance(true, std::nullopt, Scheme::Light), Appearance::high_contrast);
    EXPECT_EQ(style::choose_appearance(false, Scheme::Dark, Scheme::Light), Appearance::dark);
    EXPECT_EQ(style::choose_appearance(false, Scheme::Light, Scheme::Dark), Appearance::light);
    EXPECT_EQ(style::choose_appearance(false, std::nullopt, Scheme::Dark), Appearance::dark);
    EXPECT_EQ(style::choose_appearance(false, std::nullopt, Scheme::Light), Appearance::light);
    EXPECT_EQ(style::choose_appearance(false, std::nullopt, Scheme::Unknown), Appearance::light);
}

TEST(Look, The_high_contrast_probe_is_injectable_and_the_variable_overrides_Windows)
{
    LookGuard guard;
    style::set_high_contrast_probe([] { return true; });
    EXPECT_TRUE(style::high_contrast_active());
    style::set_high_contrast_probe({});  // the default: the variable, else SPI_GETHIGHCONTRAST
    qputenv("WIZZ_HIGH_CONTRAST", "1");
    EXPECT_TRUE(style::high_contrast_active());
    qputenv("WIZZ_HIGH_CONTRAST", "off");
    EXPECT_FALSE(style::high_contrast_active());
}

TEST(Look, WIZZ_COLOR_SCHEME_switches_a_running_window_in_place)
{
    LookGuard guard;
    qputenv("WIZZ_COLOR_SCHEME", "light");
    ASSERT_TRUE(style::apply_application_look());
    ASSERT_EQ(style::appearance(), style::Appearance::light);
    ASSERT_TRUE(ModernStyle::is_active());
    WindowRig rig;
    rig.start();
    section11_search(rig);
    ASSERT_NE(cheapest_background(rig), QColor{}) << "the search found no cheapest row";
    expect_window_follows(rig, style::light_theme());

    int notified = 0;
    QObject::connect(style::theme_notifier(), &style::ThemeNotifier::changed, [&notified] { ++notified; });
    qputenv("WIZZ_COLOR_SCHEME", "dark");
    EXPECT_TRUE(style::refresh_appearance());
    EXPECT_EQ(notified, 1);
    EXPECT_EQ(style::appearance(), style::Appearance::dark);
    EXPECT_TRUE(ModernStyle::is_active());
    expect_window_follows(rig, style::dark_theme());
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        EXPECT_EQ(QGuiApplication::styleHints()->colorScheme(), Qt::ColorScheme::Dark);  // the title bar follows
    }
    EXPECT_FALSE(style::refresh_appearance()) << "nothing changed, nothing to install";
    EXPECT_EQ(notified, 1);

    qputenv("WIZZ_COLOR_SCHEME", "light");
    EXPECT_TRUE(style::refresh_appearance());
    expect_window_follows(rig, style::light_theme());
}

TEST(Look, A_change_reported_by_Windows_is_applied_from_the_event_loop)
{
    LookGuard guard;
    qputenv("WIZZ_COLOR_SCHEME", "light");
    ASSERT_TRUE(style::apply_application_look());
    WindowRig rig;
    rig.start();
    section11_search(rig);

    // Windows switches to dark: Qt sends ThemeChange to the windows (and emits
    // colorSchemeChanged); the look is evaluated again once the event loop runs.
    qputenv("WIZZ_COLOR_SCHEME", "dark");
    QEvent change{QEvent::ThemeChange};
    QApplication::sendEvent(&rig.window(), &change);
    EXPECT_EQ(style::appearance(), style::Appearance::light) << "applied from the event loop, not inside the event";
    ASSERT_TRUE(gui::wait_until([] { return style::appearance() == style::Appearance::dark; }));
    expect_window_follows(rig, style::dark_theme());

    qputenv("WIZZ_COLOR_SCHEME", "light");
    Q_EMIT QGuiApplication::styleHints()->colorSchemeChanged(Qt::ColorScheme::Light);
    ASSERT_TRUE(gui::wait_until([] { return style::appearance() == style::Appearance::light; }));
    expect_window_follows(rig, style::light_theme());
}

TEST(Look, High_contrast_lets_the_system_palette_and_a_plain_style_win)
{
    LookGuard guard;
    ASSERT_TRUE(style::apply_application_look());
    WindowRig rig;
    rig.start();
    section11_search(rig);
    ASSERT_TRUE(ModernStyle::is_active());

    bool contrast = true;
    style::set_high_contrast_probe([&contrast] { return contrast; });
    auto* hints = const_cast<QAccessibilityHints*>(QGuiApplication::styleHints()->accessibility());
    Q_EMIT hints->contrastPreferenceChanged(Qt::ContrastPreference::HighContrast);
    ASSERT_TRUE(gui::wait_until([] { return style::appearance() == style::Appearance::high_contrast; }));

    EXPECT_FALSE(ModernStyle::is_active());
    const bool vista = QStyleFactory::keys().contains(QStringLiteral("windowsvista"), Qt::CaseInsensitive);
    EXPECT_EQ(QApplication::style()->name().toLower(), vista ? QStringLiteral("windowsvista") : QStringLiteral("windows"));
    const QPalette system = QApplication::palette();
    const style::Theme& theme = style::theme();
    EXPECT_TRUE(theme.high_contrast);
    EXPECT_EQ(theme.text, system.color(QPalette::WindowText));
    EXPECT_EQ(theme.text_muted, system.color(QPalette::WindowText)) << "no softened text";
    EXPECT_EQ(theme.accent, system.color(QPalette::Highlight));
    EXPECT_EQ(theme.selection, system.color(QPalette::Highlight));
    EXPECT_EQ(theme.selection_text, system.color(QPalette::HighlightedText));
    EXPECT_EQ(theme.success, system.color(QPalette::WindowText)) << "no colour coding";
    EXPECT_EQ(QToolTip::palette().color(QPalette::ToolTipBase), system.color(QPalette::ToolTipBase));
    EXPECT_EQ(rig.busy().background_colour(), system.color(QPalette::Window));
    EXPECT_EQ(rig.busy().status_colour(), system.color(QPalette::WindowText));
    EXPECT_EQ(rig.outbound().table()->palette().color(QPalette::Base), system.color(QPalette::Window));
    EXPECT_EQ(cheapest_background(rig), system.color(QPalette::Window)) << "no tint";
    const QCalendarWidget* calendar = rig.input().out_from_picker()->calendarWidget();
    EXPECT_FALSE(calendar->weekdayTextFormat(Qt::Saturday).hasProperty(QTextFormat::ForegroundBrush));

    contrast = false;
    QEvent change{QEvent::ThemeChange};
    QApplication::sendEvent(&rig.window(), &change);
    ASSERT_TRUE(gui::wait_until([] { return style::appearance() != style::Appearance::high_contrast; }));
    EXPECT_TRUE(ModernStyle::is_active());
    EXPECT_FALSE(style::theme().high_contrast);
    EXPECT_EQ(QApplication::palette().color(QPalette::Window), style::theme().window);
    EXPECT_EQ(rig.busy().background_colour(), style::theme().surface);
}

TEST(Look, Windows_10_fonts_leave_out_the_Windows_11_families)
{
    LookGuard guard;
    EXPECT_EQ(style::form_font().families(), (QStringList{"Segoe UI Variable Text", "Segoe UI"}));
    EXPECT_EQ(style::mono_font(9.5).families().front(), QStringLiteral("Cascadia Mono"));
    qputenv("WIZZ_FONTS", "windows10");
    EXPECT_TRUE(style::windows10_fonts());
    EXPECT_EQ(style::form_font().families(), QStringList{"Segoe UI"});
    EXPECT_EQ(style::hint_font().families(), QStringList{"Segoe UI"});
    EXPECT_EQ(style::semibold_font(10.0).families(), QStringList{"Segoe UI Semibold"});
    EXPECT_EQ(style::caption_font().families(), QStringList{"Segoe UI Semibold"});
    EXPECT_EQ(style::mono_font(9.5).families(), (QStringList{"Consolas", "Courier New"}));
    EXPECT_EQ(style::icon_font(10.0).family(), QStringLiteral("Segoe MDL2 Assets"));
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        // The icons keep their code points in Segoe MDL2 Assets.
        for (const char16_t glyph : {style::glyph::kSearch, style::glyph::kMapPin, style::glyph::kAirplane,
                                     style::glyph::kPeople, style::glyph::kInfo}) {
            EXPECT_FALSE(style::icon_size(glyph, 10.0).isEmpty()) << static_cast<int>(glyph);
        }
    }
}

TEST(Look, Selections_use_the_tint_with_text_colour)
{
    for (const style::Theme& theme : {style::light_theme(), style::dark_theme()}) {
        const QPalette palette = style::make_palette(theme);
        EXPECT_EQ(palette.color(QPalette::Highlight), theme.selection);
        EXPECT_EQ(palette.color(QPalette::HighlightedText), theme.selection_text);
        EXPECT_EQ(palette.color(QPalette::Inactive, QPalette::Highlight), theme.selection);
        EXPECT_EQ(palette.color(QPalette::ToolTipText), theme.text);
    }
}

TEST(Look, The_contrast_ratio_is_WCAG_2s)
{
    // The ratio the token test relies on, against WCAG 2's reference values: black on
    // white 21:1, a colour on itself 1:1, #767676 the lightest grey with 4.5:1 on white,
    // #777777 just below, and the ratio does not depend on the order.
    EXPECT_DOUBLE_EQ(style::contrast_ratio(QColor(Qt::black), QColor(Qt::white)), 21.0);
    EXPECT_DOUBLE_EQ(style::contrast_ratio(QColor(0x12, 0x34, 0x56), QColor(0x12, 0x34, 0x56)), 1.0);
    EXPECT_NEAR(style::contrast_ratio(QColor(0x76, 0x76, 0x76), QColor(Qt::white)), 4.54, 0.01);
    EXPECT_LT(style::contrast_ratio(QColor(0x77, 0x77, 0x77), QColor(Qt::white)), 4.5);
    EXPECT_NEAR(style::contrast_ratio(QColor(0xC5, 0x00, 0x6E), QColor(Qt::white)), 5.83, 0.01) << "the brand magenta";
    EXPECT_DOUBLE_EQ(style::contrast_ratio(QColor(Qt::white), QColor(0x76, 0x76, 0x76)),
                     style::contrast_ratio(QColor(0x76, 0x76, 0x76), QColor(Qt::white)));
}

TEST(Look, Token_pairs_meet_WCAG_AA)
{
    for (const style::Theme& t : {style::light_theme(), style::dark_theme()}) {
        const char* const scheme = t.dark ? "dark" : "light";
        const auto expect_at_least = [scheme](double minimum, const QColor& fore, const QColor& back, const char* what) {
            EXPECT_GE(style::contrast_ratio(fore, back), minimum)
                << scheme << ": " << what << " " << fore.name().toStdString() << " on " << back.name().toStdString();
        };
        // Text (4.5:1) on every surface it is drawn on.
        for (const auto& [back, where] :
             {std::pair{t.window, "window"}, std::pair{t.surface, "surface"}, std::pair{t.surface_alt, "surface_alt"},
              std::pair{t.input, "input"}, std::pair{t.selection, "selection"}, std::pair{t.hover, "hover"},
              std::pair{t.update_tint, "update tint"}}) {
            expect_at_least(4.5, t.text, back, where);
            expect_at_least(4.5, t.text_secondary, back, where);
            expect_at_least(4.5, t.text_muted, back, where);
            expect_at_least(4.5, t.accent_text, back, where);
        }
        // Grid rows: the cheapest rows hold available fares in the normal text colour.
        expect_at_least(4.5, t.text, t.cheapest, "text on cheapest");
        expect_at_least(4.5, t.text, t.cheapest_selection, "text on cheapest selection");
        expect_at_least(4.5, t.text_muted, t.cheapest, "unavailable on cheapest");
        // Status line, wait note and the primary buttons.
        for (const QColor& status : {t.success, t.failure, t.warning}) {
            expect_at_least(4.5, status, t.surface, "status on the status bar");
        }
        expect_at_least(4.5, t.on_accent, t.accent, "text on the accent");
        expect_at_least(4.5, t.on_accent, t.accent_hover, "text on the hovered accent");
        expect_at_least(4.5, t.on_accent, t.accent_pressed, "text on the pressed accent");
        // Non-text (3:1): focus rings, selection pills, the selected tab, the input stroke.
        for (const QColor& back : {t.window, t.surface, t.input, t.selection}) {
            expect_at_least(3.0, t.accent_line, back, "accent line");
        }
        expect_at_least(3.0, t.input_underline, t.input, "input bottom stroke");
    }
}

} // namespace
