// ModernStyle (DECISIONS D-53): the metrics the cards, the results and the 14 round trips
// in view at the default window rely on. The suite runs under the app's look, so these are
// the sizes the app's controls really get.

#include "date_picker.hpp"
#include "fractional_spin_box.hpp"
#include "grid_table_view.hpp"
#include "modern_style.hpp"
#include "place_combo.hpp"
#include "ui_style.hpp"

#include "wizz/core/locale.hpp"

#include <QApplication>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QStyleOptionHeader>
#include <QStyleOptionMenuItem>
#include <QStyleOptionSlider>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QtMath>

#include <gtest/gtest.h>

#include <algorithm>

namespace {

namespace style = wizz::app::style;
using wizz::app::ModernStyle;

[[nodiscard]] QStyle& app_style()
{
    return *QApplication::style();
}

[[nodiscard]] int ui_control_height()
{
    return ModernStyle::control_height(QFontMetrics(style::form_font()));
}

/// A large "W" drawn as an item of `view` through the style (CE_ItemViewItem) in `state`, as
/// a delegate draws it (the option's widget is the view); `foreground` is the text colour a
/// model or text format gives the item.
[[nodiscard]] QImage draw_item(const QWidget* view, QStyle::State state, const QColor& foreground)
{
    QStyleOptionViewItem item;
    item.initFrom(view);
    item.state = state;
    item.rect = QRect(0, 0, 160, 80);
    item.text = QStringLiteral("W");
    item.features |= QStyleOptionViewItem::HasDisplay;
    item.font = style::form_font();
    item.font.setPixelSize(48);  // glyph cores in the exact text colour
    item.fontMetrics = QFontMetrics(item.font);
    item.displayAlignment = Qt::AlignCenter;
    item.showDecorationSelected = true;
    item.palette.setColor(QPalette::Text, foreground);
    QImage image(item.rect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(style::theme().surface);
    QPainter painter(&image);
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &item, &painter, view);
    return image;
}

[[nodiscard]] int pixels_of(const QImage& image, const QColor& colour)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += image.pixelColor(x, y).rgb() == colour.rgb() ? 1 : 0;
        }
    }
    return count;
}

TEST(ModernStyle, Is_the_application_style)
{
    ASSERT_TRUE(ModernStyle::is_active()) << "the suite runs under the app's look";
    EXPECT_EQ(ModernStyle::kRadius, 5.0);
    EXPECT_EQ(ModernStyle::kCardRadius, 8.0);
    EXPECT_EQ(ModernStyle::kSelectionBarWidth, 3);
}

TEST(ModernStyle, Every_input_and_button_is_one_control_height)
{
    // max(26, line height + 10): fields, drop-downs, spin boxes, date pickers and push
    // buttons line up in a card row (the search buttons use the same height, SearchButton).
    // The line is the ascent plus the descent rounded up, as text layout and QComboBox lay it
    // out; without hinting (D-58) QFontMetrics::height() rounds the two apart and can be a pixel
    // less (17 instead of 18 for the UI font), which made the drop-downs a pixel taller than
    // the fields.
    const QFontMetrics metrics(style::form_font());
    const int line = qCeil(QFontMetricsF(style::form_font()).height());
    EXPECT_EQ(ModernStyle::line_height(metrics), line);
    EXPECT_GE(line, metrics.height());
    EXPECT_EQ(ModernStyle::control_height(metrics), std::max(26, line + 10));
    QFont tiny = style::form_font();
    tiny.setPointSizeF(5.0);
    EXPECT_EQ(ModernStyle::control_height(QFontMetrics(tiny)), 26) << "never below 26";

    const int control = ui_control_height();
    QLineEdit edit;
    QComboBox combo;
    combo.addItem(QStringLiteral("Regular fares"));
    wizz::app::PlaceCombo place;
    wizz::app::FractionalSpinBox spin;
    wizz::app::DatePicker picker{wizz::Locale::en_us()};
    QPushButton button(QStringLiteral("Later"));
    for (const QWidget* widget : std::initializer_list<const QWidget*>{&edit, &combo, &place, &spin, &picker, &button}) {
        EXPECT_EQ(widget->sizeHint().height(), control) << widget->metaObject()->className();
    }
}

TEST(ModernStyle, Push_buttons_are_text_plus_padding_with_a_minimum_unless_flat)
{
    // The text's width rounded up (natural rendering places glyphs at fractional positions,
    // D-58) and 16 px on either side.
    const QFontMetricsF metrics(style::form_font());
    const auto text_width = [&metrics](const QPushButton& button) { return qCeil(metrics.horizontalAdvance(button.text())); };
    QPushButton ok(QStringLiteral("OK"));
    EXPECT_EQ(ok.sizeHint().width(), 88) << "dialog buttons keep a minimum width";
    QPushButton long_text(QStringLiteral("Restart and update now, please"));
    EXPECT_EQ(long_text.sizeHint().width(), text_width(long_text) + 2 * 16);
    QPushButton flat(QStringLiteral("Later"));
    flat.setFlat(true);
    EXPECT_EQ(flat.sizeHint().width(), text_width(flat) + 2 * 16) << "flat: as wide as its text";
    EXPECT_EQ(app_style().pixelMetric(QStyle::PM_ButtonDefaultIndicator), 0);
    EXPECT_EQ(app_style().pixelMetric(QStyle::PM_ButtonShiftHorizontal), 0);
    EXPECT_EQ(app_style().pixelMetric(QStyle::PM_ButtonShiftVertical), 0) << "a pressed button does not move";
}

TEST(ModernStyle, Tabs_headers_scroll_bars_and_check_boxes_keep_their_metrics)
{
    QStyle& s = app_style();
    // Underline tabs: 28 px of horizontal and 10 px of vertical room around the text, no
    // base line or overlap.
    EXPECT_EQ(s.pixelMetric(QStyle::PM_TabBarTabHSpace), 28);
    EXPECT_EQ(s.pixelMetric(QStyle::PM_TabBarTabVSpace), 10);
    EXPECT_EQ(s.pixelMetric(QStyle::PM_TabBarBaseOverlap), 0);
    EXPECT_EQ(s.pixelMetric(QStyle::PM_TabBarBaseHeight), 0);
    EXPECT_EQ(s.pixelMetric(QStyle::PM_TabBarTabOverlap), 0);

    // Header sections: one text line (ascent plus descent, rounded up) plus 5 px above and
    // below (the round-trip header row), the text (rounded up) plus 9 px either side.
    EXPECT_EQ(s.pixelMetric(QStyle::PM_HeaderMargin), 5);
    QHeaderView header(Qt::Horizontal);
    header.setFont(style::bold_form_font());
    QStyleOptionHeader option;
    option.initFrom(&header);
    option.text = QStringLiteral("Return price");
    const QSize section = s.sizeFromContents(QStyle::CT_HeaderSection, &option, QSize(), &header);
    const QFontMetricsF header_metrics(header.font());
    EXPECT_EQ(section.height(), qCeil(header_metrics.height()) + 2 * 5);
    EXPECT_EQ(section.width(), qCeil(header_metrics.horizontalAdvance(option.text)) + 2 * 9);
    option.sortIndicator = QStyleOptionHeader::SortDown;
    EXPECT_EQ(s.sizeFromContents(QStyle::CT_HeaderSection, &option, QSize(), &header).width(), section.width() + 14)
        << "room for the sort arrow";

    // Thin scroll bars without arrow buttons.
    EXPECT_EQ(s.pixelMetric(QStyle::PM_ScrollBarExtent), 12);
    EXPECT_EQ(s.pixelMetric(QStyle::PM_ScrollBarSliderMin), 32);
    QStyleOptionSlider bar;
    bar.orientation = Qt::Vertical;
    bar.rect = QRect(0, 0, 12, 300);
    bar.minimum = 0;
    bar.maximum = 100;
    bar.pageStep = 10;
    EXPECT_TRUE(s.subControlRect(QStyle::CC_ScrollBar, &bar, QStyle::SC_ScrollBarAddLine).isEmpty());
    EXPECT_TRUE(s.subControlRect(QStyle::CC_ScrollBar, &bar, QStyle::SC_ScrollBarSubLine).isEmpty());
    EXPECT_EQ(s.subControlRect(QStyle::CC_ScrollBar, &bar, QStyle::SC_ScrollBarGroove), bar.rect) << "the whole length";

    // Check boxes: an 18 px box, 8 px before the text.
    EXPECT_EQ(s.pixelMetric(QStyle::PM_IndicatorWidth), 18);
    EXPECT_EQ(s.pixelMetric(QStyle::PM_IndicatorHeight), 18);
    EXPECT_EQ(s.pixelMetric(QStyle::PM_CheckBoxLabelSpacing), 8);
    const QCheckBox box(QStringLiteral("Return to the airport I left from"));
    EXPECT_GE(box.sizeHint().height(), 18 + 4);
}

TEST(ModernStyle, Popup_list_and_menu_items_are_roomy)
{
    // Combo lists and the completer: items of the font height + 12 (D-53); menus the same.
    QListView list;
    QStandardItemModel model;
    model.appendRow(new QStandardItem(QStringLiteral("Regular fares")));
    list.setModel(&model);
    QStyleOptionViewItem item;
    item.initFrom(list.viewport());
    item.text = QStringLiteral("Regular fares");
    item.features |= QStyleOptionViewItem::HasDisplay;
    const QSize list_item = app_style().sizeFromContents(QStyle::CT_ItemViewItem, &item, QSize(), &list);
    EXPECT_GE(list_item.height(), list.fontMetrics().height() + 12);

    QMenu menu;
    QStyleOptionMenuItem entry;
    entry.initFrom(&menu);
    entry.menuItemType = QStyleOptionMenuItem::Normal;
    entry.text = QStringLiteral("Copy details");
    entry.font = menu.font();
    const QSize menu_item = app_style().sizeFromContents(QStyle::CT_MenuItem, &entry, QSize(), &menu);
    EXPECT_EQ(menu_item.height(), QFontMetrics(menu.font()).height() + 2 * 6);
    EXPECT_GE(menu_item.width(), 140);
    entry.menuItemType = QStyleOptionMenuItem::Separator;
    EXPECT_EQ(app_style().sizeFromContents(QStyle::CT_MenuItem, &entry, QSize(100, 1), &menu).height(), 9);
}

TEST(ModernStyle, Nothing_animates_on_its_own_and_no_mnemonic_underlines)
{
    // D-34: the style starts no animations (Fusion would animate busy indicators and
    // transitions); Windows' default of hidden mnemonic underlines; combo lists drop down
    // under the field instead of a menu over it.
    QStyle& s = app_style();
    const ModernStyle::KeyboardCuesProbe previous = ModernStyle::set_keyboard_cues_probe([] { return false; });
    EXPECT_EQ(s.styleHint(QStyle::SH_Widget_Animation_Duration), 0);
    EXPECT_EQ(s.styleHint(QStyle::SH_UnderlineShortcut), 0);
    ModernStyle::set_keyboard_cues_probe(previous);
    EXPECT_EQ(s.styleHint(QStyle::SH_ComboBox_Popup), 0);
    EXPECT_EQ(s.styleHint(QStyle::SH_ToolTipLabel_Opacity), 255);
    EXPECT_EQ(static_cast<QRgb>(s.styleHint(QStyle::SH_Table_GridLineColor)), style::theme().row_separator.rgb());
}

TEST(ModernStyle, Mnemonic_underlines_show_with_Windows_keyboard_cues_or_while_Alt_is_held)
{
    // As on Windows (and windowsvista): "Underline access keys" shows them always; else they
    // show in a window while its Alt key is held, so &Yes / &No can be found.
    QStyle& s = app_style();
    const ModernStyle::KeyboardCuesProbe previous = ModernStyle::set_keyboard_cues_probe([] { return false; });
    QWidget window;
    auto* yes = new QPushButton(QStringLiteral("&Yes"), &window);
    QWidget other;
    EXPECT_EQ(s.styleHint(QStyle::SH_UnderlineShortcut, nullptr, yes), 0);

    QKeyEvent press(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    QApplication::sendEvent(yes, &press);
    EXPECT_EQ(s.styleHint(QStyle::SH_UnderlineShortcut, nullptr, yes), 1) << "Alt held in its window";
    EXPECT_EQ(s.styleHint(QStyle::SH_UnderlineShortcut, nullptr, &other), 0) << "not in another window";
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
    QApplication::sendEvent(yes, &release);
    EXPECT_EQ(s.styleHint(QStyle::SH_UnderlineShortcut, nullptr, yes), 0) << "hidden again";

    ModernStyle::set_keyboard_cues_probe([] { return true; });
    EXPECT_EQ(s.styleHint(QStyle::SH_UnderlineShortcut, nullptr, yes), 1) << "the Windows setting is on";
    EXPECT_EQ(s.styleHint(QStyle::SH_UnderlineShortcut), 1);
    ModernStyle::set_keyboard_cues_probe(previous);
}

TEST(ModernStyle, Selected_text_is_the_accent_and_selected_items_the_tint)
{
    // Selected text in an input (the place box's inline completion, a select-all) is the
    // accent with white text, at least 3:1 on the input; a selected list item or calendar day
    // is the soft tint with the normal text colour, like a grid row (D-53).
    const style::Theme& theme = style::theme();
    const QPalette palette = QApplication::palette();
    EXPECT_EQ(palette.color(QPalette::Highlight), theme.text_selection);
    EXPECT_EQ(palette.color(QPalette::HighlightedText), theme.on_accent);

    QListView list;
    const QImage item = draw_item(&list, QStyle::State_Enabled | QStyle::State_Active | QStyle::State_Selected,
                                  theme.text);
    EXPECT_EQ(item.pixelColor(item.width() - 12, item.height() / 2), theme.selection) << "the tint, not the accent";
    EXPECT_GT(pixels_of(item, theme.selection_text), 20) << "the normal text colour, not white";

    QCalendarWidget calendar;
    const auto* days = calendar.findChild<QTableView*>();
    ASSERT_NE(days, nullptr);
    const QImage day = draw_item(days, QStyle::State_Enabled | QStyle::State_Active | QStyle::State_Selected,
                                 theme.accent_text);
    EXPECT_EQ(day.pixelColor(day.width() - 12, day.height() / 2), theme.selection);
}

TEST(ModernStyle, Calendar_days_that_cannot_be_picked_are_in_the_disabled_text_colour)
{
    // A day before the minimum date keeps its weekend or month format otherwise, and would
    // look as pickable as its neighbours.
    const style::Theme& theme = style::theme();
    QCalendarWidget calendar;
    const auto* days = calendar.findChild<QTableView*>();
    ASSERT_NE(days, nullptr);
    const QImage out_of_range = draw_item(days, QStyle::State_Active, theme.accent_text);
    EXPECT_GT(pixels_of(out_of_range, theme.text_disabled), 20);
    EXPECT_EQ(pixels_of(out_of_range, theme.accent_text), 0);
    const QImage pickable = draw_item(days, QStyle::State_Enabled | QStyle::State_Active, theme.accent_text);
    EXPECT_GT(pixels_of(pickable, theme.accent_text), 20) << "a pickable weekend keeps its colour";
}

TEST(ModernStyle, A_disabled_default_button_looks_like_any_disabled_button)
{
    // Message boxes make their default button primary (the accent); disabled it takes the
    // disabled fill and text instead of a faded accent that reads as pressed.
    const style::Theme& theme = style::theme();
    QPushButton primary(QStringLiteral("OK"));
    primary.setDefault(true);
    primary.resize(primary.sizeHint());
    QImage image = primary.grab().toImage();
    EXPECT_EQ(image.pixelColor(4, image.height() / 2), theme.accent);
    primary.setEnabled(false);
    image = primary.grab().toImage();
    EXPECT_EQ(image.pixelColor(4, image.height() / 2), theme.input_disabled);
    EXPECT_EQ(pixels_of(image, theme.accent), 0);
}

TEST(ModernStyle, Grid_rows_are_the_data_font_height_plus_12)
{
    // 28 px at 100 % with Segoe UI Variable Text 9 pt: what the 14 round trips in view at the
    // default window are counted in (LookSnapshots).
    wizz::app::GridTableView table;
    EXPECT_EQ(table.font(), style::data_font());
    EXPECT_EQ(table.row_height(), table.fontMetrics().height() + 12);
    EXPECT_EQ(table.verticalHeader()->defaultSectionSize(), table.row_height());
}

} // namespace
