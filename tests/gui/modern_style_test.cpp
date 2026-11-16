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
#include <QCheckBox>
#include <QComboBox>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStyleOptionHeader>
#include <QStyleOptionMenuItem>
#include <QStyleOptionSlider>
#include <QStyleOptionViewItem>
#include <QTabBar>

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

TEST(ModernStyle, Is_the_application_style)
{
    ASSERT_TRUE(ModernStyle::is_active()) << "the suite runs under the app's look";
    EXPECT_EQ(ModernStyle::kRadius, 5.0);
    EXPECT_EQ(ModernStyle::kCardRadius, 8.0);
    EXPECT_EQ(ModernStyle::kSelectionBarWidth, 3);
}

TEST(ModernStyle, Every_input_and_button_is_one_control_height)
{
    // max(26, font height + 10): fields, drop-downs, spin boxes, date pickers and push
    // buttons line up in a card row (the search buttons use the same height, SearchButton).
    const QFontMetrics metrics(style::form_font());
    EXPECT_EQ(ModernStyle::control_height(metrics), std::max(26, metrics.height() + 10));
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
    const QFontMetrics metrics(style::form_font());
    QPushButton ok(QStringLiteral("OK"));
    EXPECT_EQ(ok.sizeHint().width(), 88) << "dialog buttons keep a minimum width";
    QPushButton long_text(QStringLiteral("Restart and update now, please"));
    EXPECT_EQ(long_text.sizeHint().width(), metrics.horizontalAdvance(long_text.text()) + 2 * 16);
    QPushButton flat(QStringLiteral("Later"));
    flat.setFlat(true);
    EXPECT_EQ(flat.sizeHint().width(), metrics.horizontalAdvance(flat.text()) + 2 * 16) << "flat: as wide as its text";
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

    // Header sections: one text line plus 5 px above and below (the round-trip header row).
    EXPECT_EQ(s.pixelMetric(QStyle::PM_HeaderMargin), 5);
    QHeaderView header(Qt::Horizontal);
    header.setFont(style::bold_form_font());
    QStyleOptionHeader option;
    option.initFrom(&header);
    option.text = QStringLiteral("Return price");
    const QSize section = s.sizeFromContents(QStyle::CT_HeaderSection, &option, QSize(), &header);
    EXPECT_EQ(section.height(), QFontMetrics(header.font()).height() + 2 * 5);
    EXPECT_EQ(section.width(), QFontMetrics(header.font()).horizontalAdvance(option.text) + 2 * 9);
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
    EXPECT_EQ(s.styleHint(QStyle::SH_Widget_Animation_Duration), 0);
    EXPECT_EQ(s.styleHint(QStyle::SH_UnderlineShortcut), 0);
    EXPECT_EQ(s.styleHint(QStyle::SH_ComboBox_Popup), 0);
    EXPECT_EQ(s.styleHint(QStyle::SH_ToolTipLabel_Opacity), 255);
    EXPECT_EQ(static_cast<QRgb>(s.styleHint(QStyle::SH_Table_GridLineColor)), style::theme().row_separator.rgb());
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
