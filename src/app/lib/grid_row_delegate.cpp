#include "grid_row_delegate.hpp"

#include "grid_colors.hpp"
#include "modern_style.hpp"
#include "ui_style.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QToolTip>

namespace wizz::app {

GridRowDelegate::GridRowDelegate(QObject* parent)
    : QStyledItemDelegate(parent), selection_(grid_colors::highlight())
{
    style::on_theme_change(this, [this] { selection_ = QBrush{grid_colors::highlight()}; });
}

void GridRowDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    option->state.setFlag(QStyle::State_MouseOver, false);  // DataGridView rows have no hover effect
    option->state.setFlag(QStyle::State_HasFocus, false);   // no per-cell focus frame: the row shows it
    if (!option->state.testFlag(QStyle::State_Selected)) {
        return;
    }
    const QVariant custom = index.data(SelectionBackgroundRole);
    const QColor text = grid_colors::highlight_text();
    option->state.setFlag(QStyle::State_Selected, false);
    option->backgroundBrush = custom.isValid() ? custom.value<QBrush>() : selection_;
    option->palette.setColor(QPalette::Text, text);
    option->palette.setColor(QPalette::HighlightedText, text);
}

void GridRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyledItemDelegate::paint(painter, option, index);
    const QRect r = option.rect;
    const style::Theme& theme = style::theme();
    painter->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), theme.row_separator);
    const auto* view = qobject_cast<const QAbstractItemView*>(option.widget);
    if (view == nullptr || r.left() > 0 || r.right() < 0) {
        return;  // the bar belongs to the cell at the view's visible left edge
    }
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool focused = view->hasFocus();
    const QModelIndex current = view->currentIndex();
    const bool keyboard_row = focused && current.isValid() && current.row() == index.row();
    if (!selected && !keyboard_row) {
        return;
    }
    // A 3 px bar at the row's left edge: the accent on the selected row of a focused grid and
    // on its current row even when that is not selected (Ctrl+Space, Ctrl+click), so the
    // keyboard focus always shows; the muted text colour on the selected row of a grid without
    // focus, so the row the menu and Copy act on stays marked beyond its tint. High Contrast:
    // the selected row is the system highlight, so the bar takes its text colour.
    QColor bar = focused ? theme.accent_line : theme.text_muted;
    if (theme.high_contrast) {
        bar = selected ? theme.selection_text : theme.text;
    }
    painter->fillRect(QRect(0, r.top(), ModernStyle::kSelectionBarWidth, r.height()), bar);
}

bool GridRowDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                                const QModelIndex& index)
{
    if (event == nullptr || view == nullptr || event->type() != QEvent::ToolTip || !index.isValid()) {
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
    QStyleOptionViewItem cell = option;
    initStyleOption(&cell, index);
    if (!cell.text.isEmpty() && text_truncated(cell, view)) {
        QToolTip::showText(event->globalPos(), cell.text, view->viewport(), cell.rect);
    } else {
        QToolTip::hideText();
    }
    return true;
}

bool GridRowDelegate::text_truncated(const QStyleOptionViewItem& option, const QWidget* widget)
{
    const QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    // QCommonStyle's item text margin: PM_FocusFrameHMargin + 1 on each side.
    const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, &option, widget) + 1;
    return option.fontMetrics.horizontalAdvance(option.text) + 2 * margin > option.rect.width();
}

} // namespace wizz::app
